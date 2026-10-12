#include <acpi/lapic.h>
#include <asm.h>
#include <atomic.h>
#include <enforce.h>
#include <global.h>
#include <irq/ipi.h>
#include <log.h>
#include <math/bit.h>
#include <sch/irql.h>
#include <smp/core.h>
#include <structures/cpu_mask.h>
#include <virt/kvm.h>

#define IPI_ICR_FIXED (LAPIC_DELIVERY_FIXED | LAPIC_LEVEL_ASSERT)

enum ipi_mask_backend {
    IPI_BACKEND_UNKNOWN = 0,
    IPI_BACKEND_XAPIC_PHYSICAL,
    IPI_BACKEND_X2APIC_CLUSTER,
    IPI_BACKEND_KVM_PV,
};

/* All CPUs would compute the same thing so it's fine to race */
static atomic_uint32_t ipi_backend = IPI_BACKEND_UNKNOWN;

void ipi_send(cpu_id_t cpu, uint8_t vector) {
    lapic_icr_write(smp_apic_id(cpu),
                    vector | IPI_ICR_FIXED | LAPIC_DEST_PHYSICAL);
}

bool ipi_send_try(cpu_id_t cpu, uint8_t vector) {
    return lapic_icr_try_write(smp_apic_id(cpu),
                               vector | IPI_ICR_FIXED | LAPIC_DEST_PHYSICAL);
}

void nmi_send(cpu_id_t cpu) {
    enforce(cpu != smp_id(TOPC_NONE)); /* NMI'ing ourselves is a MASSIVE
                                        * risk, likely buggy code, panic */

    lapic_icr_write(smp_apic_id(cpu), LAPIC_DELIVERY_NMI | LAPIC_LEVEL_ASSERT |
                                          LAPIC_DEST_PHYSICAL);
}

void nmi_send_others(cpu_id_t self) {
    size_t i;
    for_each_cpu_id(i) {
        if (i == self)
            continue;

        nmi_send(i);
    }
}

static void ipi_send_shorthand(uint32_t shorthand, uint8_t vector) {
    lapic_icr_write(0,
                    vector | IPI_ICR_FIXED | LAPIC_DEST_PHYSICAL | shorthand);
}

struct x2apic_cluster_acc {
    uint32_t cluster;
    uint16_t bits;
};

static void x2apic_cluster_flush(struct x2apic_cluster_acc *acc,
                                 uint8_t vector) {
    if (!acc->bits)
        return;

    uint32_t ldr = (acc->cluster << X2APIC_LDR_CLUSTER_SHIFT) | acc->bits;
    lapic_icr_write(ldr, vector | IPI_ICR_FIXED | LAPIC_DEST_LOGICAL);
    acc->bits = 0;
}

/* Batches consecutive same-cluster IDs into one ICR write */
static void x2apic_cluster_add(struct x2apic_cluster_acc *acc, uint32_t apic_id,
                               uint8_t vector) {
    uint32_t cluster =
        (apic_id >> X2APIC_CLUSTER_SHIFT) & X2APIC_CLUSTER_ID_MASK;

    if (acc->bits && cluster != acc->cluster)
        x2apic_cluster_flush(acc, vector);

    acc->cluster = cluster;
    acc->bits |= (uint16_t) BIT(apic_id & (X2APIC_CLUSTER_SIZE - 1));
}

static void ipi_send_mask_cluster(const struct cpu_mask *targets,
                                  uint8_t vector) {
    struct x2apic_cluster_acc acc = {0};
    size_t cpu;

    for_each_cpu(cpu, targets) {
        x2apic_cluster_add(&acc, smp_apic_id(cpu), vector);
    }

    x2apic_cluster_flush(&acc, vector);
}

static void ipi_send_mask_physical(const struct cpu_mask *targets,
                                   uint8_t vector) {
    size_t cpu;
    for_each_cpu(cpu, targets) {
        ipi_send(cpu, vector);
    }
}

static enum ipi_mask_backend ipi_backend_get(void) {
    enum ipi_mask_backend b = atomic_load_acq(&ipi_backend);
    if (b != IPI_BACKEND_UNKNOWN)
        return b;

    if (!x2apic_enabled)
        b = IPI_BACKEND_XAPIC_PHYSICAL;
    else if (kvm_has_feature(KVM_FEATURE_PV_SEND_IPI))
        b = IPI_BACKEND_KVM_PV;
    else
        b = IPI_BACKEND_X2APIC_CLUSTER;

    atomic_store_release(&ipi_backend, b);
    return b;
}

static void kvm_pv_window_send(const uint64_t win[2], uint32_t min,
                               uint8_t vector) {
    enum err e = kvm_hypercall4(KVM_HC_SEND_IPI, win[0], win[1], min,
                                vector | LAPIC_DELIVERY_FIXED, NULL);
    if (e == ERR_OK)
        return;

    log_msg_once(LOG_WARN, "KVM PV IPI failed (%s), using x2APIC clusters",
                 errno_to_str(e));
    atomic_store_release(&ipi_backend, IPI_BACKEND_X2APIC_CLUSTER);

    struct x2apic_cluster_acc acc = {0};
    for (uint32_t i = 0; i < KVM_PV_IPI_WINDOW; i++) {
        if (win[i / 64] & BIT(i % 64))
            x2apic_cluster_add(&acc, min + i, vector);
    }

    x2apic_cluster_flush(&acc, vector);
}

/* One hypercall per 128-ID window starting at the lowest ID in it */
static void ipi_send_mask_kvm_pv(const struct cpu_mask *targets,
                                 uint8_t vector) {
    uint64_t win[2] = {0, 0};
    uint32_t min = 0;
    bool open = false;
    size_t cpu;

    cpu_full_fence();
    for_each_cpu(cpu, targets) {
        uint32_t apic_id = smp_apic_id(cpu);

        if (!open || apic_id < min || apic_id - min >= KVM_PV_IPI_WINDOW) {
            if (open)
                kvm_pv_window_send(win, min, vector);

            win[0] = win[1] = 0;
            min = apic_id;
            open = true;
        }

        uint32_t off = apic_id - min;
        win[off / 64] |= BIT(off % 64);
    }

    if (open)
        kvm_pv_window_send(win, min, vector);
}

struct ipi_mask_plan {
    size_t others; /* targets excluding self */
    size_t sends;  /* writes/hypercalls the exact path would issue */
    bool self;
};

static struct ipi_mask_plan ipi_mask_plan(const struct cpu_mask *targets,
                                          enum ipi_mask_backend backend,
                                          size_t this_cpu) {
    struct ipi_mask_plan p = {0};
    uint32_t group = UINT32_MAX;
    size_t cpu;

    for_each_cpu(cpu, targets) {
        if (cpu == this_cpu)
            p.self = true;
        else
            p.others++;

        uint32_t apic_id = smp_apic_id(cpu);
        switch (backend) {
        case IPI_BACKEND_X2APIC_CLUSTER: {
            uint32_t c = apic_id >> X2APIC_CLUSTER_SHIFT;
            if (c != group) {
                group = c;
                p.sends++;
            }
            break;
        }

        case IPI_BACKEND_KVM_PV:
            if (group == UINT32_MAX || apic_id < group ||
                apic_id - group >= KVM_PV_IPI_WINDOW) {
                group = apic_id;
                p.sends++;
            }
            break;

        default: p.sends++; break;
        }
    }

    return p;
}

static bool ipi_mask_should_broadcast(const struct ipi_mask_plan *p,
                                      size_t online,
                                      enum ipi_mask_flags flags) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return false;

    if (online > CPU_MASK_BITS)
        return false;

    size_t bystanders = online - 1 - p->others;
    if (!bystanders)
        return true;

    if (!(flags & IPI_MASK_SUPERSET_OK) || p->sends <= 1)
        return false;

    return (p->sends - 1) * 256 >= bystanders * IPI_BCAST_RATIO_Q8;
}

size_t ipi_send_cost(const struct cpu_mask *targets) {
    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);

    cpu_id_t this_cpu = smp_id(TOPC_IRQL);

    enum ipi_mask_backend backend = ipi_backend_get();
    struct ipi_mask_plan plan = ipi_mask_plan(targets, backend, this_cpu);

    irql_lower(irql);

    return plan.sends;
}

void ipi_send_mask(const struct cpu_mask *targets, uint8_t vector,
                   enum ipi_mask_flags flags) {
    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);

    cpu_id_t this_cpu = smp_id(TOPC_IRQL);
    size_t online = global.core_count;
    enum ipi_mask_backend backend = ipi_backend_get();
    struct ipi_mask_plan plan = ipi_mask_plan(targets, backend, this_cpu);

    if (!plan.others && !plan.self)
        goto out;

    if (plan.others && ipi_mask_should_broadcast(&plan, online, flags)) {
        ipi_send_shorthand(plan.self ? LAPIC_DEST_SHORTHAND_ALL
                                     : LAPIC_DEST_SHORTHAND_OTHERS,
                           vector);
        goto out;
    }

    switch (backend) {
    case IPI_BACKEND_KVM_PV: ipi_send_mask_kvm_pv(targets, vector); break;
    case IPI_BACKEND_X2APIC_CLUSTER:
        ipi_send_mask_cluster(targets, vector);
        break;

    default: ipi_send_mask_physical(targets, vector); break;
    }

out:
    irql_lower(irql);
}
