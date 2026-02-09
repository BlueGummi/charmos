#include <global.h>
#include <mem/asan.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

static bool asan_ready = false;
static uint8_t *asan_shadow_base;
static size_t asan_shadow_size;

/* Compute the shadow byte for a given real address */
static inline uint8_t *asan_shadow_for_internal(void *addr) {
    return asan_shadow_base + ((uintptr_t) addr >> ASAN_SHADOW_SCALE);
}

/* Poison/unpoison helpers */
void asan_poison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    uint8_t *shadow_start = asan_shadow_for_internal(addr);
    uint8_t *shadow_end = asan_shadow_for_internal((uint8_t *) addr + size - 1);
    for (uint8_t *s = shadow_start; s <= shadow_end; s++)
        *s = 0xFF;
}

void asan_unpoison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    uint8_t *shadow_start = asan_shadow_for_internal(addr);
    uint8_t *shadow_end = asan_shadow_for_internal((uint8_t *) addr + size - 1);
    for (uint8_t *s = shadow_start; s <= shadow_end; s++)
        *s = 0;
}

void asan_init(void) {
    return;
    k_info("ASAN", K_INFO, "Bringing up ASAN... this will take time...");
    asan_shadow_size = (global.total_pages * PAGE_SIZE) >> ASAN_SHADOW_SCALE;

    paddr_t shadow_phys = pmm_alloc_pages(
        (asan_shadow_size + PAGE_SIZE - 1) / PAGE_SIZE, ALLOC_FLAGS_DEFAULT);

    if (!shadow_phys)
        k_panic("ASAN: could not allocate shadow memory\n");

    asan_shadow_base = (uint8_t *) (shadow_phys + global.hhdm_offset);

    size_t remaining = asan_shadow_size;
    uint64_t phys = shadow_phys;
    uint64_t virt = ASAN_SHADOW_OFFSET;

    while (remaining >= PAGE_2MB && (phys % PAGE_2MB) == 0 &&
           (virt % PAGE_2MB) == 0) {
        if (vmm_map_2mb_page(virt, phys, PAGING_PRESENT | PAGING_WRITE,
                             VMM_FLAG_NONE) < 0)
            k_panic("ASAN: failed to map 2MB page at %lx\n", virt);
        phys += PAGE_2MB;
        virt += PAGE_2MB;
        remaining -= PAGE_2MB;
    }

    while (remaining > 0) {
        if (vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE,
                         VMM_FLAG_NONE) < 0)
            k_panic("ASAN: failed to map 4KB page at %lx\n", virt);
        phys += PAGE_SIZE;
        virt += PAGE_SIZE;
        remaining = remaining > PAGE_SIZE ? remaining - PAGE_SIZE : 0;
    }

    k_info("ASAN", K_INFO, "ASAN mapped up and brought up... memsetting..");
    /* Initialize shadow memory to poisoned */
    memset(asan_shadow_base, 0xFF, asan_shadow_size);

    k_info("ASAN", K_INFO, "shadow memory initialized at 0x%lx",
           asan_shadow_base);

    asan_ready = true;
}

static inline uint8_t *asan_shadow_for(const void *addr) {
    uintptr_t a = (uintptr_t) addr;
    size_t idx = a >> ASAN_SHADOW_SCALE;
    if (idx >= asan_shadow_size) {
        return NULL;
    }
    return asan_shadow_base + idx;
}

/* Report function called on violation. Prints info and panics. */
static void __asan_report_and_panic(const char *what, const void *addr,
                                    size_t size, bool is_write) {
    k_printf("[ASAN] %s at %p size=%zu %s\n", what, addr, size,
             is_write ? "store" : "load");
    k_panic("ASAN: aborting due to memory error\n");
}

/* Core access check: conservative: checks every shadow byte spanned by access.
 * If any shadow byte != 0, treat as violation.
 */
#include "mem/slab/internal.h"
static inline void asan_check_access_core(const void *addr, size_t size,
                                          bool is_write) {
    ASAN_ABORT_IF_NOT_READY();
    vaddr_t v = (vaddr_t) addr;
    if (v < SLAB_HEAP_START || v > SLAB_HEAP_END)
        return;

    if (size == 0)
        return;

    const uint8_t *start = (const uint8_t *) addr;
    const uint8_t *end = start + (size - 1);

    uintptr_t start_idx = (uintptr_t) start >> ASAN_SHADOW_SCALE;
    uintptr_t end_idx = (uintptr_t) end >> ASAN_SHADOW_SCALE;

    /* bounds-check shadow index to avoid accessing past shadow table */
    if (end_idx >= asan_shadow_size) {
        __asan_report_and_panic("ASAN: access out-of-shadow-range", addr, size,
                                is_write);
        return;
    }

    uint8_t *s = asan_shadow_base + start_idx;
    size_t count = (end_idx - start_idx) + 1;

    /* If any shadow byte is non-zero => poisoned. */
    for (size_t i = 0; i < count; i++) {
        if (s[i] != 0) {
            __asan_report_and_panic("ASAN: invalid memory access (poisoned)",
                                    addr, size, is_write);
            return;
        }
    }
}

/* Public runtime functions used by compiler instrumentation.
   We'll implement 1/2/4/8 byte variants for loads and stores. */

void __asan_load1(const void *addr) {
    asan_check_access_core(addr, 1, false);
}
void __asan_load2(const void *addr) {
    asan_check_access_core(addr, 2, false);
}
void __asan_load4(const void *addr) {
    asan_check_access_core(addr, 4, false);
}
void __asan_load8(const void *addr) {
    asan_check_access_core(addr, 8, false);
}
void __asan_load16(const void *addr) {
    asan_check_access_core(addr, 16, false);
} /* some compilers */

void __asan_store1(const void *addr) {
    asan_check_access_core(addr, 1, true);
}
void __asan_store2(const void *addr) {
    asan_check_access_core(addr, 2, true);
}
void __asan_store4(const void *addr) {
    asan_check_access_core(addr, 4, true);
}
void __asan_store8(const void *addr) {
    asan_check_access_core(addr, 8, true);
}
void __asan_store16(const void *addr) {
    asan_check_access_core(addr, 16, true);
}

/* Generic wrappers the compiler sometimes uses */
void __asan_loadN(const void *addr, size_t size) {
    asan_check_access_core(addr, size, false);
}
void __asan_storeN(const void *addr, size_t size) {
    wait_for_interrupt();
    asan_check_access_core(addr, size, true);
}

void __asan_poison_memory_region(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (size == 0)
        return;

    uint8_t *start_shadow = asan_shadow_for(addr);
    if (!start_shadow)
        return; /* out-of-range: ignore conservatively */

    uint8_t *end_shadow = asan_shadow_for((uint8_t *) addr + size - 1);
    if (!end_shadow) {
        /* If end is out-of-range, compute up to last valid shadow byte. */
        size_t start_idx = (uintptr_t) addr >> ASAN_SHADOW_SCALE;
        if (start_idx >= asan_shadow_size)
            return;
        end_shadow = asan_shadow_base + (asan_shadow_size - 1);
    }

    for (uint8_t *p = start_shadow; p <= end_shadow; p++)
        *p = ASAN_POISON_VALUE;
}

void __asan_unpoison_memory_region(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (size == 0)
        return;

    uint8_t *start_shadow = asan_shadow_for(addr);
    if (!start_shadow)
        return;

    uint8_t *end_shadow = asan_shadow_for((uint8_t *) addr + size - 1);
    if (!end_shadow) {
        size_t start_idx = (uintptr_t) addr >> ASAN_SHADOW_SCALE;
        if (start_idx >= asan_shadow_size)
            return;
        end_shadow = asan_shadow_base + (asan_shadow_size - 1);
    }

    for (uint8_t *p = start_shadow; p <= end_shadow; p++)
        *p = 0;
}

/* Minimal global registration support:
 * The compiler emits calls to __asan_register_globals() for static globals;
 * the runtime gets an array of descriptors with address+size. We'll unpoison
 * each global and poison small redzones around it (simple fixed redzone).
 *
 * This structure layout is what Clang typically expects; compilers may vary.
 * For a minimal build, the compiler-provided descriptors should be compatible.
 *
 * We provide a conservative implementation: unpoison the region and poison
 * a fixed small left/right redzone.
 */
struct __asan_global {
    void *addr;
    size_t size;
    const char *name;
    /* some targets include more fields; we ignore them */
};

void __asan_register_globals(struct __asan_global *globals, size_t n) {
    ASAN_ABORT_IF_NOT_READY();
    const size_t redzone = 16;
    for (size_t i = 0; i < n; i++) {
        void *addr = globals[i].addr;
        size_t size = globals[i].size;
        if (!addr || size == 0)
            continue;

        /* poison left redzone (if address is valid) */
        if ((uintptr_t) addr >= redzone) /* simple check */
            __asan_poison_memory_region((uint8_t *) addr - redzone, redzone);
        __asan_unpoison_memory_region(addr, size);
        __asan_poison_memory_region((uint8_t *) addr + size, redzone);
    }
}

/* Minimal unregister (no-op) */
void __asan_unregister_globals(void *globals, size_t n) {
    (void) globals;
    (void) n;
}

#define ASAN_MAX_STACK_RECORDS 1024
struct stack_record {
    void *addr;
    size_t size;
};

static struct stack_record stack_records[ASAN_MAX_STACK_RECORDS];
static size_t stack_records_count = 0;

void __asan_stack_malloc(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (!addr || size == 0)
        return;
    /* Poison entire region, then unpoison the real payload so redzones are left
       poisoned. For simplicity pick small redzones here. */
    const size_t rz = 16;
    __asan_poison_memory_region((uint8_t *) addr - rz, size + rz * 2);
    __asan_unpoison_memory_region(addr, size);

    if (stack_records_count < ASAN_MAX_STACK_RECORDS) {
        stack_records[stack_records_count].addr = addr;
        stack_records[stack_records_count].size = size;
        stack_records_count++;
    }
}

void __asan_stack_free(void *addr) {
    ASAN_ABORT_IF_NOT_READY();
    /* Unpoison and remove record. */
    for (size_t i = 0; i < stack_records_count; i++) {
        if (stack_records[i].addr == addr) {
            size_t size = stack_records[i].size;
            const size_t rz = 16;
            __asan_poison_memory_region((uint8_t *) addr - rz, size + rz * 2);
            /* shift tail down */
            stack_records[i] = stack_records[stack_records_count - 1];
            stack_records_count--;
            return;
        }
    }
}

/* Optional report helpers (compiler may call __asan_report_*); implement a
   minimal mapping to the same panic/report routine so user sees a message. */

void __asan_report_load1(void *addr) {
    __asan_report_and_panic("ASAN: load1", addr, 1, false);
}
void __asan_report_load2(void *addr) {
    __asan_report_and_panic("ASAN: load2", addr, 2, false);
}
void __asan_report_load4(void *addr) {
    __asan_report_and_panic("ASAN: load4", addr, 4, false);
}
void __asan_report_load8(void *addr) {
    __asan_report_and_panic("ASAN: load8", addr, 8, false);
}
void __asan_report_load16(void *addr) {
    __asan_report_and_panic("ASAN: invalid 16-byte load", addr, 16, false);
}

void __asan_report_load32(void *addr) {
    __asan_report_and_panic("ASAN: invalid 32-byte load", addr, 32, false);
}

void __asan_report_load64(void *addr) {
    __asan_report_and_panic("ASAN: invalid 64-byte load", addr, 64, false);
}

void __asan_report_load_n(void *addr, size_t size) {
    __asan_report_and_panic("ASAN: invalid variable-size load", addr, size,
                            false);
}

void __asan_report_store1(void *addr) {
    __asan_report_and_panic("ASAN: store1", addr, 1, true);
}
void __asan_report_store2(void *addr) {
    __asan_report_and_panic("ASAN: store2", addr, 2, true);
}
void __asan_report_store4(void *addr) {
    __asan_report_and_panic("ASAN: store4", addr, 4, true);
}
void __asan_report_store8(void *addr) {
    __asan_report_and_panic("ASAN: store8", addr, 8, true);
}
void __asan_report_store16(void *addr) {
    __asan_report_and_panic("ASAN: invalid 16-byte store", addr, 16, true);
}

void __asan_report_store32(void *addr) {
    __asan_report_and_panic("ASAN: invalid 32-byte store", addr, 32, true);
}

void __asan_report_store64(void *addr) {
    __asan_report_and_panic("ASAN: invalid 64-byte store", addr, 64, true);
}

void __asan_report_store_n(void *addr, size_t size) {
    __asan_report_and_panic("ASAN: invalid variable-size store", addr, size,
                            true);
}

/* --- Stack-use-after-return configuration --- */

/* Compiler checks this flag to see if it should try to detect
 * stack-use-after-return. We can just say "false" (0).
 */
int __asan_option_detect_stack_use_after_return = 0;

/* Stack alloc/free variants, indexed by small number suffix (0–9) */

void *__asan_stack_malloc_0(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_1(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_2(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_3(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_4(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_5(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_6(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_7(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_8(size_t size) {
    (void) size;
    return NULL;
}
void *__asan_stack_malloc_9(size_t size) {
    (void) size;
    return NULL;
}

void __asan_stack_free_0(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_1(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_2(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_3(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_4(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_5(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_6(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_7(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_8(void *p, size_t size) {
    (void) p;
    (void) size;
}
void __asan_stack_free_9(void *p, size_t size) {
    (void) p;
    (void) size;
}

void *__asan_malloc(size_t size) {
    /* Not relevant for kernel: just panic if somehow called */
    __asan_report_and_panic("asan_malloc called", NULL, size, true);
    return NULL;
}

void __asan_free(void *p) {
    __asan_report_and_panic("asan_free called", p, 0, true);
}

void __asan_malloc_hook(void *ptr, size_t size) {
    (void) ptr;
    (void) size;
}
void __asan_free_hook(void *ptr) {
    (void) ptr;
}

/* --- Init/fini stubs --- */

void __asan_init(void) {
    /* Your kernel’s asan_init() should be called early in boot. */
    k_info("ASAN", K_INFO, "__asan_init runtime stub called");
}

void __asan_before_dynamic_init(const char *module_name) {
    (void) module_name;
}
void __asan_after_dynamic_init(void) {}

/* --- Miscellaneous runtime entrypoints --- */

/* Compiler sometimes emits these for non-instrumented copies. */
void *__asan_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}
void *__asan_memmove(void *dst, const void *src, size_t n) {
    return memmove(dst, src, n);
}
void *__asan_memset(void *s, int c, size_t n) {
    return memset(s, c, n);
}

/* Some compilers also expect these “weak” entrypoints */
void __asan_handle_no_return(void) {}
void __asan_after_load(void) {}
void __asan_after_store(void) {}
void __asan_before_memory_access(void) {}
void __asan_after_memory_access(void) {}

/* Optional runtime interface for global poisoning/unpoisoning */
void __asan_set_shadow_00_to_0x00(void) {}
void __asan_set_shadow_f8_to_0x00(void) {}

void __asan_alloca_poison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (!addr || size == 0)
        return;
    /* Conservative: poison the whole region. The compiler expects
       later an unpoison to unmark the payload itself. */
    __asan_poison_memory_region(addr, size);
}

void __asan_allocas_unpoison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (!addr || size == 0)
        return;
    __asan_unpoison_memory_region(addr, size);
}

/* Extra aliases that some toolchains expect (no-ops or forwards).
   Provide weak aliases so they can be overridden if needed. */
__attribute__((weak)) void __asan_alloca_poison_0(void *addr, size_t size) {
    __asan_alloca_poison(addr, size);
}

__attribute__((weak)) void __asan_allocas_unpoison_0(void *addr, size_t size) {
    __asan_allocas_unpoison(addr, size);
}
