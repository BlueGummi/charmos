#include <console/crash.h>
#include <console/printf.h>
#include <dbg.h>
#include <irq/exception_sync_cb.h>
#include <irq/irq.h>
#include <math/range.h>
#include <mem/address_range.h>
#include <mem/demand_page.h>
#include <mem/hhdm.h>
#include <mem/page_fault.h>
#include <mem/page_table.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <string.h>
#include <sync/spinlock.h>
#include <thread/thread.h>

static enum exception_sync_cb_result
page_fault_sync_cb(struct exception_sync_cb *this, struct irq_context *irqc,
                   uint8_t buf[EXCEPTION_SYNC_CB_SCRATCH_BUFFER_SIZE]);

static void cc_noreturn page_fault_report_crash(vaddr_t fault_addr,
                                                uint64_t error_code,
                                                struct irq_context *irqc);

EXCEPTION_SYNC_CB_REGISTER(page_fault, IRQ_PAGE_FAULT, page_fault_sync_cb,
                           NULL);

static struct spinlock pf_lock = SPINLOCK_INIT;

enum irq_result page_fault_isr(void *context, uint8_t vector,
                               struct irq_context *rsp) {
    cc_ignore(context, vector);
    /* Synchronization here is actually OK: we can only receive one
     * IRQ per CPU at any moment, and the caller passes the thread-local buffer
     * to the exception_sync_cb below, this is just how the ISR
     * gets access to said buffer without extra parameters */
    struct page_fault_scratch_buffer *pfsb =
        (struct page_fault_scratch_buffer *) rsp->irq_stack_scratch_buf;

    uint64_t error_code = rsp->regs->error_code;
    uint64_t fault_addr;
    asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
    pfsb->error_code = error_code;
    pfsb->virt = fault_addr;

    return IRQ_HANDLED;
}

static enum exception_sync_cb_result
page_fault_sync_cb(struct exception_sync_cb *this, struct irq_context *irqc,
                   uint8_t buf[EXCEPTION_SYNC_CB_SCRATCH_BUFFER_SIZE]) {
    cc_ignore(this);
    struct page_fault_scratch_buffer *pfsb =
        (struct page_fault_scratch_buffer *) buf;

    vaddr_t vaddr = pfsb->virt;
    uint64_t error = pfsb->error_code;

    enum page_fault_access access_error;
    if (error & PAGE_FAULT_EC_WRITE) {
        access_error = PAGE_FAULT_WRITE;
    } else if (error & PAGE_FAULT_EC_INSTRUCTION) {
        access_error = PAGE_FAULT_EXEC;
    } else {
        access_error = PAGE_FAULT_READ;
    }

    struct page_fault_info pfi = {
        .addr = vaddr,
        .user = error & PAGE_FAULT_EC_USER,
        .was_present = error & PAGE_FAULT_EC_PRESENT,
        .access = access_error,
    };

    /*
     * I'll need to rework this model... just leave it absent for now,
     * no harm done, yet... TODO:
    if (smp_core()->irq_entered_irql != IRQL_PASSIVE_LEVEL)
        goto crash; */

    /* Do the full crash here, it's not a valid address_range,
     * we need the other information here */
    struct address_range *adr = address_range_for_addr(vaddr);
    if (!adr)
        goto crash;

    /* it just needs to exist */
    struct page_fault_handler *pfh = adr->page_fault_handler;
    enforce(pfh);
    enforce(pfh->ops && pfh->ops->is_valid_fault);

    if (!pfh->ops->is_valid_fault(&pfi))
        goto crash;

    /* Great, we have a valid vaddr, let's bring it in */

    /* TODO: order > 0, i.e. hugepages */
    pte_t pte = vmm_get_leaf_pte(vaddr);

    /* Nice, another CPU mapped this in
     * for us while we were dillying */
    if (pte & PAGE_PRESENT)
        goto done;

    struct pte_tagged ptag = pte_tagged_unpack(pte);
    enforce(ptag.type == PTE_TAG_TYPE_DEMAND_PAGED);
    enforce((ptag.payload & DEMAND_PAGE_FLAG_ZERO_MEMORY) ||
            (ptag.payload & DEMAND_PAGE_FLAG_NONE));
    bool zeroed_out = ptag.payload & DEMAND_PAGE_FLAG_ZERO_MEMORY;
    paddr_t paddr;

    /* TODO: memory locality in allocations, we can use the alloc_pages
     * function to supply that, but we do need to take note of this */
    if (!pfh->ops->alloc_pages) {
        paddr = pmm_alloc_page(); /* TODO: order > 0 */
    } else {
        paddr = pfh->ops->alloc_pages(vaddr, 0);
    }

    enforce(paddr); /* TODO: might be recoverable? many say no */

    if (zeroed_out)
        memset(hhdm_paddr_to_ptr(paddr), 0, PAGE_SIZE);

    enum err e = vmm_map_demand_page(vaddr, paddr, ptag.payload);
    if (e == ERR_EXIST) {
        pmm_free_page(paddr);
        return EXCEPTION_SYNC_CB_OK;
    }

    if (pfh->ops->update_after_map)
        if (!pfh->ops->update_after_map(vaddr, page_for_paddr(paddr)))
            pmm_free_page(paddr);

done:
    return EXCEPTION_SYNC_CB_OK;

crash:
    page_fault_report_crash(vaddr, error, irqc);
}

static bool addr_is_mapped(uint64_t addr) {
    return vmm_get_phys((vaddr_t) PAGE_ALIGN_DOWN(addr)) != (uintptr_t) -1;
}

static void dump_slab_exec_fault(struct thread *curr, struct irq_context *ctx) {
    printf("\n=== SLAB EXEC FAULT DEBUG ===\n");

    struct irq_registers *rsp = ctx->regs;
    printf("Faulting RIP (from CPU): %p\n", (void *) rsp->rip);
    printf("Faulting RSP (from CPU): %p\n", (void *) rsp->rsp);

    printf("\n--- Current thread struct ---\n");
    printf("Thread struct addr: %p\n", (void *) curr);

    if (!addr_is_mapped((uint64_t) curr)) {
        printf("  Thread pointer is NOT MAPPED - cannot dump\n");
        return;
    }

    printf("  tid:   %lu\n", curr->id);
    printf("  name:  %p", (void *) curr->name);
    if (curr->name && addr_is_mapped((uint64_t) curr->name))
        printf(" -> \"%s\"", curr->name);
    printf("\n");
    printf("  entry: %p\n", (void *) curr->entry);
    printf("  stack: %p (size %lu)\n", (void *) curr->stack, curr->stack_size);
    printf("  state: %u\n", (uint32_t) atomic_load_rlx(&curr->state));
    printf("  core:  %u\n", (uint32_t) curr->curr_core);
    printf("  flags: 0x%lx\n", (uint64_t) thread_get_flags(curr));
    printf("  ref:   %lu\n", (uint64_t) refcount_read(&curr->refcount));

    printf("\n--- Saved regs (post-switch residual) ---\n");
    printf("  rbx = %p\n", (void *) curr->regs.rbx);
    printf("  rbp = %p\n", (void *) curr->regs.rbp);
    printf("  r12 = %p\n", (void *) curr->regs.r12);
    printf("  r13 = %p\n", (void *) curr->regs.r13);
    printf("  r14 = %p\n", (void *) curr->regs.r14);
    printf("  r15 = %p\n", (void *) curr->regs.r15);
    printf("  rsp = %p\n", (void *) curr->regs.rsp);
    printf("  rip = %p\n", (void *) curr->regs.rip);

    printf("\n--- Thread struct raw dump (992 bytes) ---\n");
    debug_print_memory((void *) curr, 992);

    uint8_t *prev_obj = (uint8_t *) curr - 992;
    printf("\n--- Preceding slab object tail (last 256 bytes) ---\n");
    if (addr_is_mapped((uint64_t) prev_obj)) {
        debug_print_memory(prev_obj + 992 - 256, 256);
    } else {
        printf("  Preceding object at %p is not mapped\n", (void *) prev_obj);
    }

    printf("\n--- Stack at fault RSP (%p) ---\n", (void *) rsp->rsp);
    if (addr_is_mapped(rsp->rsp)) {
        debug_print_memory((void *) rsp->rsp, 256);
    } else {
        printf("  RSP is not mapped!\n");
    }

    printf("\n--- ISR context (regs at fault time) ---\n");
    printf("  rax=%p  rbx=%p\n", (void *) rsp->rax, (void *) rsp->rbx);
    printf("  rcx=%p  rdx=%p\n", (void *) rsp->rcx, (void *) rsp->rdx);
    printf("  rdi=%p  rsi=%p\n", (void *) rsp->rdi, (void *) rsp->rsi);
    printf("  rbp=%p  rsp=%p\n", (void *) rsp->rbp, (void *) rsp->rsp);
    printf("  r8 =%p  r9 =%p\n", (void *) rsp->r8, (void *) rsp->r9);
    printf("  r10=%p  r11=%p\n", (void *) rsp->r10, (void *) rsp->r11);
    printf("  r12=%p  r13=%p\n", (void *) rsp->r12, (void *) rsp->r13);
    printf("  r14=%p  r15=%p\n", (void *) rsp->r14, (void *) rsp->r15);
    printf("  rip=%p  rfl=%p\n", (void *) rsp->rip, (void *) rsp->rflags);
    printf("  cs=%p   ss=%p\n", (void *) rsp->cs, (void *) rsp->ss);

    /* Crash: raw smp_id is fine */
    struct scheduler *sched = global.schedulers[smp_id_raw()];
    printf("\n--- Scheduler state (core %zu) ---\n", smp_id_raw());
    printf("  sched->current = %p\n", (void *) sched->current);
    printf("  sched->drop_last_ref = %p\n", (void *) sched->drop_last_ref);
    printf("  sched->other_locked = %p\n", (void *) sched->other_locked);
    printf("  sched->stealing_work = %u\n",
           (uint32_t) atomic_load_rlx(&sched->stealing_work));

    printf("\n--- Thread migration info ---\n");
    printf("  migrate_to = %ld\n", (int64_t) atomic_load(&curr->migrate_to));
    printf("  migration_gen = 0x%lx\n",
           atomic_load_rlx(&curr->migration_generation));
    printf("  scheduler = %p\n", (void *) atomic_load(&curr->scheduler));
}

static cc_noreturn void page_fault_report_crash(vaddr_t fault_addr,
                                                uint64_t error_code,
                                                struct irq_context *ctx) {

    struct thread *curr = thread_get_current();

    struct address_range *ar = address_range_for_addr(fault_addr);
    const char *name = ar ? ar->name : "UNKNOWN";

    struct irq_registers *irqc = ctx->regs;

    spin_lock_raw(&pf_lock);

    printf("\n=== PAGE FAULT === @ %p\n", (void *) irqc->rip);
    printf("Faulting Address (CR2): %p (ar: %s)\n", (void *) fault_addr, name);
    printf("Error Code: 0x%lx\n", error_code);
    printf("  - Page not Present (P): %s\n",
           (error_code & PAGE_FAULT_EC_PRESENT) ? "Yes" : "No");
    printf("  - Write Access (W/R): %s\n",
           (error_code & PAGE_FAULT_EC_WRITE) ? "Write" : "Read");
    printf("  - User Mode (U/S): %s\n",
           (error_code & PAGE_FAULT_EC_USER) ? "User" : "Supervisor");
    printf("  - Reserved Bit Set (RSVD): %s\n",
           (error_code & PAGE_FAULT_EC_RESERVED) ? "Yes" : "No");
    printf("  - Instruction Fetch (I/D): %s\n",
           (error_code & PAGE_FAULT_EC_INSTRUCTION) ? "Yes" : "No");
    printf("  - Protection Key Violation (PK): %s\n",
           (error_code & PAGE_FAULT_EC_PROTECTION_KEY) ? "Yes" : "No");
    printf("  - Kernel stack %p -> %p\n", curr->stack,
           (void *) ((uintptr_t) curr->stack + curr->stack_size));

    vaddr_t protector_base = (uintptr_t) curr->stack - PAGE_SIZE;
    vaddr_t protector_top = (uintptr_t) curr->stack;
    if (IN_RANGE(fault_addr, protector_base, protector_top))
        printf("Likely stack overflow!! Fault in protector page!!!\n");

    vaddr_t code = PAGE_ALIGN_DOWN(irqc->rip);
    if (vmm_get_phys(code) != (paddr_t) -1) {
        printf("\n--- Bytes at RIP %p ---\n", (void *) irqc->rip);

        for (int64_t i = -16; i < 16; i++) {
            vaddr_t at = irqc->rip + i;

            if (PAGE_ALIGN_DOWN(at) != code && vmm_get_phys(at) == (paddr_t) -1)
                continue;

            printf("%02x ", *(const uint8_t *) at);
        }

        printf("\n");
    }

    printf("\n--- Stack at fault RSP ---\n");
    debug_print_stack_from((uint64_t *) irqc->rsp, 0);

    bool is_slab_exec = (error_code & PAGE_FAULT_EC_INSTRUCTION) && ar &&
                        strcmp(ar->name, "slab") == 0;

    if (!is_slab_exec && (error_code & PAGE_FAULT_EC_INSTRUCTION)) {
        struct address_range *rip_ar = address_range_for_addr(irqc->rip);
        if (rip_ar && strcmp(rip_ar->name, "slab") == 0)
            is_slab_exec = true;
    }

    if (is_slab_exec)
        dump_slab_exec_fault(curr, ctx);

    struct crash_regs pregs;
    irq_context_to_crash_regs(irqc, &pregs);
    char msg[CRASH_MSG_MAX];
    snprintf(msg, sizeof(msg),
             "Kernel Page Fault at %p (CR2: %p, ar: %s, ec: 0x%lx, thread: %s)",
             (void *) irqc->rip, (void *) fault_addr, name, error_code,
             curr ? curr->name : "none");

    spin_unlock_raw(&pf_lock);

    crash_full(&(struct crash_context){
        .source = CRASH_SOURCE_CPU_EXCEPTION,
        .formats = CRASH_FMT_DEFAULT,
        .file = __FILE__,
        .line = __LINE__,
        .func = __func__,
        .msg = msg,
        .regs = &pregs,
    });
}
