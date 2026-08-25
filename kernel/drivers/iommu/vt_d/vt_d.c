#include <asm.h>
#include <console/printf.h>
#include <drivers/iommu/vt_d.h>
#include <drivers/mmio.h>
#include <log.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

LOG_HANDLE_DECLARE_PRINT(vtd);
LOG_SITE_DECLARE_PRINT(vtd);

const struct iommu_ops vtd_iommu_ops = {};

void vtd_write_gcmd(struct vtd_unit *u, uint32_t cmd) {
    uint32_t status = mmio_read_32(&u->regs->global_status);

    uint32_t preserved =
        status & (GSTS_TRANSLATION_ENABLED | GSTS_ROOT_TABLE_PTR_SET |
                  GSTS_QUEUED_INVAL_ENABLED | GSTS_INTERRUPT_REMAP_ENABLED |
                  GSTS_COMPAT_FORMAT_INTERRUPT_STATUS);

    mmio_write_32(&u->regs->global_command, preserved | cmd);
}

void vtd_wait_gsts(struct vtd_unit *u, uint32_t bit, bool set) {
    while (1) {
        uint32_t s = mmio_read_32(&u->regs->global_status);
        if (set && (s & bit))
            break;

        if (!set && !(s & bit))
            break;

        cpu_relax(); /* pause */
    }
}

enum iommu_error vtd_iq_init(struct vtd_unit *u) {
    if (!ECAP_QUEUED_INVALIDATION(u->ecap)) {
        vtd_warn("QI not supported by this unit, cannot continue");
        return IOMMU_ERR_UNSUPPORTED;
    }

    if (mmio_read_32(&u->regs->global_status) & GSTS_QUEUED_INVAL_ENABLED) {
        vtd_info("QI already enabled, disabling before reset");
        vtd_write_gcmd(u, mmio_read_32(&u->regs->global_status) &
                              ~GCMD_QUEUED_INVAL_ENABLE);
        vtd_wait_gsts(u, GSTS_QUEUED_INVAL_ENABLED, false);
    }

    mmio_write_64(&u->regs->invalidation_queue_tail, 0);
    mmio_write_64(&u->regs->invalidation_queue_head, 0);

    u->iq_size = 256;
    paddr_t phys = pmm_alloc_page();
    if (!phys)
        return IOMMU_ERR_NO_MEM;

    u->iq_phys = phys;
    u->iq_base = mmio_map(phys, PAGE_SIZE);
    u->iq_head = 0;
    u->iq_tail = 0;
    memset(u->iq_base, 0, PAGE_SIZE);

    mmio_write_64(&u->regs->invalidation_queue_addr,
                  IQA_REG_BUILD(phys, IQA_SIZE_256_ENTRIES));

    vtd_write_gcmd(u, GCMD_QUEUED_INVAL_ENABLE);
    vtd_wait_gsts(u, GSTS_QUEUED_INVAL_ENABLED, true);

    vtd_info("IQ initialized: phys=0x%llx entries=%u", phys, u->iq_size);
    return IOMMU_ERR_OK;
}

void vtd_iq_submit(struct vtd_unit *u, struct vtd_inv_desc desc) {
    struct vtd_inv_desc *ring = u->iq_base;
    mmio_write_64(&ring[u->iq_tail].hi, desc.hi);
    mmio_write_64(&ring[u->iq_tail].lo, desc.lo);

    u->iq_tail = (u->iq_tail + 1) % u->iq_size;
    mmio_write_64(&u->regs->invalidation_queue_tail, u->iq_tail << 4);
}

void vtd_iq_flush(struct vtd_unit *u) {
    static volatile uint32_t wait_token __attribute__((aligned(4)));
    paddr_t token_phys = vmm_get_phys((vaddr_t) &wait_token, VMM_FLAG_NONE);

    wait_token = 0;

    vtd_iq_submit(u, WAIT_DESC(token_phys, 1));

    while (wait_token != 1)
        cpu_relax();
}

enum iommu_error vtd_root_table_init(struct vtd_unit *u) {
    paddr_t phys = pmm_alloc_page();
    if (!phys)
        return IOMMU_ERR_NO_MEM;

    u->root_table_phys = phys;
    u->root_table = mmio_map(phys, PAGE_SIZE);
    memset(u->root_table, 0, PAGE_SIZE);

    mmio_write_64(&u->regs->root_table_addr,
                  phys | RTADDR_TRANSLATION_MODE_LEGACY);

    vtd_write_gcmd(u, GCMD_SET_ROOT_TABLE_PTR);
    vtd_wait_gsts(u, GSTS_ROOT_TABLE_PTR_SET, true);

    vtd_iq_submit(u, CTX_INVAL_DESC_GLOBAL);
    vtd_iq_submit(u, IOTLB_INVAL_DESC_GLOBAL);
    vtd_iq_flush(u);

    vtd_info("root table initialized: phys=0x%llx", phys);
    return IOMMU_ERR_OK;
}

/*
 * vtd_build_identity_sl
 *
 * Build minimal second-level page table that identity-maps the full
 * 512 gb low address space using 1 gb hugepages
 *
 * One PML4 entry covers all 512 PDPT slots = 512 GiB
 */
static paddr_t vtd_build_identity_sl(struct vtd_unit *u) {
    (void) u;

    paddr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys)
        return 0;
    uint64_t *pml4 = mmio_map(pml4_phys, PAGE_SIZE);
    memset(pml4, 0, PAGE_SIZE);

    paddr_t pdpt_phys = pmm_alloc_page();
    if (!pdpt_phys)
        return 0;
    uint64_t *pdpt = mmio_map(pdpt_phys, PAGE_SIZE);

    for (int i = 0; i < 512; i++) {
        pdpt[i] = ((uint64_t) i << 30) | SL_PTE_LARGE_PAGE |
                  SL_PTE_MEMORY_TYPE_WRITE_BACK | SL_PTE_IGNORE_PAT |
                  SL_PTE_READ | SL_PTE_WRITE;
    }

    /* SL_TABLE_ENTRY sets the physical address + PRESENT (R|W) */
    pml4[0] = SL_TABLE_ENTRY(pdpt_phys);

    return pml4_phys;
}

/*
 * Populate every root+context entry so no BDF produces a translation
 * fault after GCMD_TRANSLATION_ENABLE is set
 *
 * All devices are placed in domain 1 and
 * pointed at the shared identity SL table built above
 */
static enum iommu_error vtd_passthrough_all_devices(struct vtd_unit *u) {
    paddr_t sl_phys = vtd_build_identity_sl(u);
    if (!sl_phys)
        return IOMMU_ERR_NO_MEM;

    const uint16_t domain_id = 1;

    uint64_t ctx_lo =
        CTX_ENTRY_SET_LO(sl_phys, CTX_ENTRY_TRANSLATION_TYPE_UNTRANSLATED);
    uint64_t ctx_hi = CTX_ENTRY_SET_HI(domain_id, CTX_ENTRY_ADDR_WIDTH_48BIT);

    struct vtd_root_entry *root = (struct vtd_root_entry *) u->root_table;

    for (int bus = 0; bus < 256; bus++) {
        if (!(root[bus].lo & ROOT_ENTRY_PRESENT)) {
            paddr_t ctx_phys = pmm_alloc_page();
            if (!ctx_phys)
                return IOMMU_ERR_NO_MEM;

            void *ctx_virt = mmio_map(ctx_phys, PAGE_SIZE);
            memset(ctx_virt, 0, PAGE_SIZE);

            root[bus].lo = ROOT_ENTRY_SET_CONTEXT_TABLE_PTR(ctx_phys);
            root[bus].hi = 0;
        }

        paddr_t ctx_phys = root[bus].lo & ROOT_ENTRY_CONTEXT_TABLE_PTR_MASK;
        struct vtd_context_entry *ctx = mmio_map(ctx_phys, PAGE_SIZE);

        for (int df = 0; df < 256; df++) {
            ctx[df].lo = ctx_lo;
            ctx[df].hi = ctx_hi;
        }
    }

    vtd_iq_submit(u, CTX_INVAL_DESC_GLOBAL);
    vtd_iq_submit(u, IOTLB_INVAL_DESC_GLOBAL);
    vtd_iq_flush(u);

    vtd_info("passthrough identity domain installed for all devices (dom=%u "
             "sl=0x%llx)",
             domain_id, sl_phys);
    return IOMMU_ERR_OK;
}

enum iommu_error vtd_unit_init(struct iommu *unit) {
    struct vtd_unit *u = unit->private;

    vtd_info("initializing VT-d unit seg=%u regs=%p", u->segment, u->regs);
    vtd_info("  CAP:  SAGAW=%02x MGAW=%u CM=%u RWBF=%u domains=%u",
             CAP_SUPPORTED_ADDR_WIDTHS(u->cap), CAP_MAX_ADDR_WIDTH(u->cap),
             CAP_CACHING_MODE(u->cap), CAP_REQUIRES_WRITE_BUF_FLUSH(u->cap),
             vtd_cap_domain_count(u->cap));
    vtd_info("  ECAP: QI=%u IR=%u PT=%u SC=%u C=%u",
             ECAP_QUEUED_INVALIDATION(u->ecap),
             ECAP_INTERRUPT_REMAPPING(u->ecap), ECAP_PASS_THROUGH(u->ecap),
             ECAP_SNOOP_CONTROL(u->ecap), ECAP_COHERENCY(u->ecap));

    if (!(CAP_SUPPORTED_ADDR_WIDTHS(u->cap) & ADDR_WIDTH_48BIT)) {
        vtd_warn("4-level page tables not supported, aborting");
        return IOMMU_ERR_UNSUPPORTED;
    }

    if (CAP_REQUIRES_WRITE_BUF_FLUSH(u->cap)) {
        vtd_write_gcmd(u, GCMD_WRITE_BUFFER_FLUSH);
        vtd_wait_gsts(u, GSTS_WRITE_BUFFER_FLUSH_STATUS, false);
    }

    enum iommu_error err;

    err = vtd_iq_init(u);
    if (err != IOMMU_ERR_OK)
        return err;

    err = vtd_root_table_init(u);
    if (err != IOMMU_ERR_OK)
        return err;

    err = vtd_passthrough_all_devices(u);
    if (err != IOMMU_ERR_OK)
        return err;

    vtd_write_gcmd(u, GCMD_TRANSLATION_ENABLE);
    vtd_wait_gsts(u, GSTS_TRANSLATION_ENABLED, true);

    vtd_info("translation enabled");
    unit->status = IOMMU_STATUS_ACTIVE;
    return IOMMU_ERR_OK;
}

struct iommu *vtd_unit_create(uint64_t base_phys, uint16_t segment,
                              uint8_t size_field) {
    struct iommu *unit = kmalloc(sizeof(*unit));
    if (!unit)
        return NULL;

    struct vtd_unit *u = kmalloc(sizeof(*u));
    if (!u) {
        kfree(unit);
        return NULL;
    }

    memset(unit, 0, sizeof(*unit));
    memset(u, 0, sizeof(*u));

    u->segment = segment;
    u->regs = mmio_map(base_phys, 1u << (size_field + 12));
    u->cap = u->regs->capabilities;
    u->ecap = u->regs->extended_capabilities;
    u->domain_count = vtd_cap_domain_count(u->cap);

    unit->ops = &vtd_iommu_ops;
    unit->private = u;
    unit->status = IOMMU_STATUS_INACTIVE;

    vtd_unit_init(unit);
    return unit;
}
