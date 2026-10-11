#include <asm.h>
#include <atomic.h>
#include <cpuid_defs.h>
#include <math/bit.h>
#include <string.h>
#include <virt/kvm.h>

enum kvm_state {
    KVM_STATE_UNKNOWN = 0,
    KVM_STATE_ABSENT,
    KVM_STATE_VMCALL,  /* Intel */
    KVM_STATE_VMMCALL, /* AMD, Hygon */
};

/* Races are fine since everyone would report the same thing */
static atomic_uint32_t kvm_state = KVM_STATE_UNKNOWN;
static atomic_uint32_t kvm_features;

static enum kvm_state kvm_probe(uint32_t *features) {
    uint32_t a, b, c, d;

    cpuid_count(CPUID_LEAF_FEATURES, 0, &a, &b, &c, &d);
    if (!(c & CPUID_ECX_HYPERVISOR))
        return KVM_STATE_ABSENT;

    uint32_t base = CPUID_HV_LEAF_BASE;
    for (; base < CPUID_HV_LEAF_LIMIT; base += CPUID_HV_LEAF_STRIDE) {
        uint8_t sig[CPUID_HV_SIGNATURE_LEN];
        cpuid_count(base, 0, &a, &b, &c, &d);
        memcpy(sig + 0, &b, 4);
        memcpy(sig + 4, &c, 4);
        memcpy(sig + 8, &d, 4);

        /* Old KVM reports a max leaf of 0, meaning base + 1 */
        if (!memcmp(sig, KVM_CPUID_SIGNATURE, sizeof(sig)) &&
            (!a || a >= base + KVM_CPUID_FEATURES_OFF))
            break;
    }

    if (base >= CPUID_HV_LEAF_LIMIT)
        return KVM_STATE_ABSENT;

    cpuid_count(base + KVM_CPUID_FEATURES_OFF, 0, &a, &b, &c, &d);
    *features = a;

    cpuid_count(CPUID_LEAF_VENDOR, 0, &a, &b, &c, &d);
    bool amd = b == CPUID_VENDOR_EBX_AMD || b == CPUID_VENDOR_EBX_HYGON;
    return amd ? KVM_STATE_VMMCALL : KVM_STATE_VMCALL;
}

static enum kvm_state kvm_state_get(void) {
    enum kvm_state s = atomic_load_acq(&kvm_state);
    if (s != KVM_STATE_UNKNOWN)
        return s;

    uint32_t features = 0;
    s = kvm_probe(&features);

    atomic_store_rlx(&kvm_features, features);
    atomic_store_release(&kvm_state, s);
    return s;
}

bool kvm_detect(void) {
    return kvm_state_get() != KVM_STATE_ABSENT;
}

bool kvm_has_feature(uint32_t feature) {
    if (!kvm_detect())
        return false;

    return atomic_load_rlx(&kvm_features) & BIT(feature);
}

static enum err kvm_err(int64_t ret) {
    switch (-ret) {
    case KVM_ENOSYS:
    case KVM_EOPNOTSUPP: return ERR_NOT_IMPL;
    case KVM_EFAULT: return ERR_FAULT;
    case KVM_EINVAL: return ERR_INVAL;
    case KVM_E2BIG: return ERR_OVERFLOW;
    case KVM_EPERM: return ERR_PERM;
    default: return ERR_UNKNOWN;
    }
}

enum err kvm_hypercall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t *out) {
    int64_t ret;

    switch (kvm_state_get()) {
    case KVM_STATE_VMMCALL:
        asm volatile("vmmcall"
                     : "=a"(ret)
                     : "a"(nr), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
                     : "memory");
        break;

    case KVM_STATE_VMCALL:
        asm volatile("vmcall"
                     : "=a"(ret)
                     : "a"(nr), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
                     : "memory");
        break;

    default: return ERR_NO_DEV;
    }

    if (ret < 0)
        return kvm_err(ret);

    if (out)
        *out = (uint64_t) ret;

    return ERR_OK;
}
