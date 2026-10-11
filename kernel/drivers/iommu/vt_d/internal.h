#include <asm.h>
#include <console/printf.h>
#include <drivers/iommu/vt_d.h>
#include <log.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

#define PT_LEVELS 4

/*
 * VT-d Second-Level PTE locking
 */
#define SL_PTE_LOCK_BIT BIT(61)
#define SL_PTE_DEAD_BIT BIT(60)

typedef atomic_uint64_t sl_pte_atomic_t;

static inline bool vtd_pt_trylock(sl_pte_atomic_t *pte,
                                  enum irql       *irql_out) TSA_NO_ANALYSIS {
    *irql_out = irql_raise(IRQL_DISPATCH_LEVEL);

    uint64_t old = atomic_load_rlx(pte);
    do {
        if (old & (SL_PTE_LOCK_BIT | SL_PTE_DEAD_BIT)) {
            irql_lower(*irql_out);
            return false;
        }

    } while (
        !atomic_cas_weak(pte, &old, old | SL_PTE_LOCK_BIT, mo_acquire, mo_rlx));

    return true;
}

static inline void vtd_pt_unlock_internal(sl_pte_atomic_t *pte) {
    atomic_fetch_and_release(pte, ~SL_PTE_LOCK_BIT);
}

static inline void vtd_pt_mark_dead(sl_pte_atomic_t *pte) {
    atomic_fetch_or_release(pte, SL_PTE_DEAD_BIT);
}

enum vtd_pt_lock_result {
    VTD_PT_LOCK_OK = 0,
    VTD_PT_LOCK_NOT_PRESENT,
    VTD_PT_LOCK_DEAD,
};

static inline enum vtd_pt_lock_result
vtd_pt_lock_internal(sl_pte_atomic_t *pte) {
    while (true) {
        uint64_t old = atomic_load_rlx(pte);

        if (!(old & SL_PTE_PRESENT))
            return VTD_PT_LOCK_NOT_PRESENT;
        if (old & SL_PTE_DEAD_BIT)
            return VTD_PT_LOCK_DEAD;
        if (old & SL_PTE_LOCK_BIT) {
            cpu_pause();
            continue;
        }

        if (atomic_cas_weak(pte, &old, old | SL_PTE_LOCK_BIT, mo_acquire,
                            mo_rlx))
            return VTD_PT_LOCK_OK;

        cpu_pause();
    }
}

static inline enum irql vtd_pt_lock(sl_pte_atomic_t *pte) TSA_NO_ANALYSIS {
    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    while (true) {
        uint64_t val = atomic_load_rlx(pte);
        if (val & SL_PTE_LOCK_BIT) {
            cpu_pause();
            continue;
        }
        if (atomic_cas_weak(pte, &val, val | SL_PTE_LOCK_BIT, mo_acquire,
                            mo_rlx))
            return old;
    }
}

static inline void vtd_pt_unlock(sl_pte_atomic_t *pte,
                                 enum irql        old_irql) TSA_NO_ANALYSIS {
    vtd_pt_unlock_internal(pte);
    irql_lower(old_irql);
}

void vtd_write_gcmd(struct vtd_unit *u, uint32_t cmd);
void vtd_wait_gsts(struct vtd_unit *u, uint32_t bit, bool set);
enum iommu_error vtd_iq_init(struct vtd_unit *u);
void vtd_iq_submit(struct vtd_unit *u, struct vtd_inv_desc desc);
void vtd_iq_flush(struct vtd_unit *u);
enum iommu_error vtd_root_table_init(struct vtd_unit *u);
enum iommu_error vtd_root_table_init(struct vtd_unit *u);
void vtd_iotlb_flush_global(struct vtd_unit *u);
void vtd_iotlb_flush_domain(struct vtd_unit *u, uint16_t domain_id);
void vtd_iotlb_flush_page(struct vtd_unit *u, uint16_t domain_id, iova_t iova,
                          uint8_t am);
void vtd_iotlb_flush_range(struct vtd_unit *u, uint16_t domain_id, iova_t iova,
                           size_t size);
void vtd_iotlb_flush_range_batched(struct vtd_unit *u, uint16_t domain_id,
                                   iova_t iova, size_t size);

size_t vtd_domain_alloc(struct vtd_unit *unit);
size_t vtd_domain_free(struct vtd_unit *unit, size_t domain);
bool vtd_domain_init(struct vtd_unit *unit);
extern const struct iommu_ops vtd_iommu_ops;
