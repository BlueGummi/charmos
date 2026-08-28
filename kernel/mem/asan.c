#include <console/crash.h>
#include <dbg.h>
#include <global.h>
#include <kassert.h>
#include <math/align.h>
#include <mem/asan.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <ndjson.h>
#include <stack_depot.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "mem/slab/internal.h"

#define kasan_panic(what, addr, size, is_write)                                \
    do {                                                                       \
        char _kasan_msg[CRASH_MSG_MAX];                                        \
        snprintf(_kasan_msg, sizeof(_kasan_msg),                               \
                 "KASAN: %s at %p (size=%zu, %s)", (what), (addr),             \
                 (size_t) (size), (is_write) ? "store" : "load");              \
        crash(&(struct crash_context){                                         \
            .source = CRASH_SOURCE_KASAN,                                      \
            .formats = CRASH_FMT_DEFAULT,                                      \
            .file = __FILE__,                                                  \
            .line = __LINE__,                                                  \
            .func = __func__,                                                  \
            .msg = _kasan_msg,                                                 \
        });                                                                    \
    } while (0)

#ifdef DEBUG_ASAN
LOG_SITE_DECLARE_PRINT(asan);
LOG_HANDLE_DECLARE_PRINT(asan);

/* If we widen this, ASAN_SHADOW_OFFSET must be recomputed, because right now
 * it maps exactly this window into the shadow region */
#define ASAN_COVERED_START SLAB_HEAP_START
#define ASAN_COVERED_END SLAB_HEAP_END

static bool asan_ready = false;

/* Two pages that everything aliases to for pages that are fully OK/not OK to
 * read/modify */
static paddr_t asan_zero_shadow_phys;  /* never had a chunk: reads accessible */
static paddr_t asan_freed_shadow_phys; /* had one, gave it back: reads freed */

static inline bool asan_addr_covered(uintptr_t a, size_t size) {
    return a >= ASAN_COVERED_START && (a + size) <= ASAN_COVERED_END;
}

static inline uint8_t *asan_shadow_for_internal(const void *addr) {
    return (uint8_t *) ASAN_SHADOW_ADDR(addr);
}

/* Number of shadow bytes covering [addr, addr + size) */
static inline size_t asan_shadow_span(const void *addr, size_t size) {
    const uint8_t *end = (const uint8_t *) addr + size - 1;
    return (size_t) (asan_shadow_for_internal(end) -
                     asan_shadow_for_internal(addr)) +
           1;
}

static void asan_mark_valid(void *addr, size_t size) {
    kassert(IS_ALIGNED((uintptr_t) addr, ASAN_GRANULE));

    uint8_t *shadow = asan_shadow_for_internal(addr);
    size_t full = size >> ASAN_SHADOW_SCALE;
    size_t tail = size & (ASAN_GRANULE - 1);

    memset(shadow, 0, full);
    if (tail)
        shadow[full] = (uint8_t) tail;
}

static void asan_mark_poisoned(void *addr, size_t size, uint8_t value) {
    kassert(IS_ALIGNED((uintptr_t) addr, ASAN_GRANULE));

    memset(asan_shadow_for_internal(addr), value, asan_shadow_span(addr, size));
}

void asan_alloc(void *addr, size_t requested, size_t slot) {
    ASAN_ABORT_IF_NOT_READY();

    if (!slot || !asan_addr_covered((uintptr_t) addr, slot))
        return;

    kassert(requested && requested <= slot);

    asan_mark_valid(addr, requested);

    size_t redzone = ALIGN_UP(requested, ASAN_GRANULE);
    if (redzone < slot)
        asan_mark_poisoned((uint8_t *) addr + redzone, slot - redzone,
                           ASAN_POISON_HEAP_REDZONE);
}

void asan_free(void *addr, size_t slot) {
    ASAN_ABORT_IF_NOT_READY();

    if (!slot || !asan_addr_covered((uintptr_t) addr, slot))
        return;

    asan_mark_poisoned(addr, slot, ASAN_POISON_HEAP_FREED);
}

void asan_poison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();

    if (!size || !asan_addr_covered((uintptr_t) addr, size))
        return;

    asan_mark_poisoned(addr, size, ASAN_POISON_VALUE);
}

void asan_unpoison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();

    if (!size || !asan_addr_covered((uintptr_t) addr, size))
        return;

    asan_mark_valid(addr, size);
}

static inline bool asan_shadow_is_shared(vaddr_t shadow_page) {
    paddr_t p = vmm_get_phys(shadow_page);
    return p == asan_zero_shadow_phys || p == asan_freed_shadow_phys;
}

static bool asan_page_is_uniform(const void *addr, uint8_t value) {
    const uint64_t *p = addr;
    uint64_t want = value * 0x0101010101010101ULL;

    for (size_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        if (p[i] != want)
            return false;
    }

    return true;
}

static inline vaddr_t asan_shadow_page_first(vaddr_t base) {
    return ALIGN_DOWN(ASAN_SHADOW_ADDR(base), PAGE_SIZE);
}

static inline vaddr_t asan_shadow_page_limit(vaddr_t base, size_t len) {
    return ALIGN_UP(ASAN_SHADOW_ADDR(base + len - 1) + 1, PAGE_SIZE);
}

enum errno asan_shadow_install(vaddr_t base, size_t len) {
    if (!asan_ready)
        return ERR_OK;

    if (!len || !asan_addr_covered(base, len))
        return ERR_OK;

    vaddr_t limit = asan_shadow_page_limit(base, len);

    for (vaddr_t v = asan_shadow_page_first(base); v < limit; v += PAGE_SIZE) {
        if (!asan_shadow_is_shared(v))
            continue;

        /* Per page rather than once up front, so a range of any length works:
         * the walk is cheap and only happens for a page we are about to back */
        enum errno err =
            vmm_unshare_path(v, VMM_MAP_PAGE_SIZE_4KB, VMM_FLAG_NONE);
        if (err < 0)
            return err;

        paddr_t phys = pmm_alloc_page();
        if (!phys)
            return ERR_NO_MEM;

        err =
            vmm_map_page_internal(v, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_XD,
                                  VMM_FLAG_MODIFY_LEAF, VMM_MAP_PAGE_SIZE_4KB);
        if (err < 0) {
            pmm_free_page(phys);
            return err;
        }

        memset((void *) v, ASAN_POISON_VALUE, PAGE_SIZE);
    }

    return ERR_OK;
}

void asan_shadow_release(vaddr_t base, size_t len) {
    if (!asan_ready)
        return;

    if (!len || !asan_addr_covered(base, len))
        return;

    vaddr_t limit = asan_shadow_page_limit(base, len);

    for (vaddr_t v = asan_shadow_page_first(base); v < limit; v += PAGE_SIZE) {
        if (asan_shadow_is_shared(v))
            continue;

        if (!asan_page_is_uniform((void *) v, ASAN_POISON_HEAP_FREED))
            continue;

        paddr_t phys = vmm_get_phys(v);

        if (vmm_map_page_internal(v, asan_freed_shadow_phys,
                                  PAGE_PRESENT | PAGE_XD, VMM_FLAG_MODIFY_LEAF,
                                  VMM_MAP_PAGE_SIZE_4KB) < 0)
            continue;

        pmm_free_page(phys);
    }
}

static void asan_map_early_shadow(void) {
    vaddr_t start = ASAN_SHADOW_ADDR(ASAN_COVERED_START);
    vaddr_t end = ASAN_SHADOW_ADDR(ASAN_COVERED_END);

    asan_zero_shadow_phys = pmm_alloc_page(ALLOC_FLAGS_ZERO);
    if (!asan_zero_shadow_phys)
        kasan_panic("could not allocate the shared shadow page", NULL, 0,
                    false);

    asan_freed_shadow_phys = pmm_alloc_page();
    if (!asan_freed_shadow_phys)
        kasan_panic("could not allocate the shared freed shadow page", NULL, 0,
                    false);

    memset(hhdm_paddr_to_ptr(asan_freed_shadow_phys), ASAN_POISON_HEAP_FREED,
           PAGE_SIZE);

    /* Deliberately RO and aliased */
    enum errno err = vmm_map_aliased(start, end - start, asan_zero_shadow_phys,
                                     PAGE_PRESENT | PAGE_XD, VMM_FLAG_NONE);
    if (err < 0)
        kasan_panic("could not map the shared shadow window",
                    (const void *) start, end - start, false);

    asan_info("shared shadow: [%lx, %lx), %zu GiB of window on one zero page "
              "at %lx\n",
              start, end, (size_t) ((end - start) / GB(1)),
              asan_zero_shadow_phys);
}

void asan_init(void) {
    asan_info("bringing ASAN up...");

    asan_map_early_shadow();

    asan_ready = true;
}

NDJSON_DECLARE(asan_fault, NDJSON_SECTION_ASAN, NDJSON_KIND_FAULT, 1,
               NDJSON_STR(what), NDJSON_HEX(addr), NDJSON_U64(size),
               NDJSON_STR(access));

NDJSON_DECLARE(asan_frame, NDJSON_SECTION_ASAN, NDJSON_KIND_FRAME, 1,
               NDJSON_U64(idx), NDJSON_HEX(addr), NDJSON_STR(sym),
               NDJSON_U64(off));

static void asan_report_shadow(const void *addr) {
    const uint8_t *sh = asan_shadow_for_internal(addr);

    printf("[ASAN] shadow near %p, one byte per %u bytes, fault at column 0:\n",
           addr, (unsigned) ASAN_GRANULE);
    for (int row = -1; row <= 1; row++) {
        printf("[ASAN]  %+6d ", row * 16 * (int) ASAN_GRANULE);
        for (int i = 0; i < 16; i++)
            printf("%02x ", sh[row * 16 + i]);
        printf("\n");
    }
}

/* stack_depot_print is bare addresses, so we debug_symbolize */
static void asan_report_stack(stack_handle_t h) {
    struct stack_depot_record *rec = stack_depot_get_record(h);

    if (!rec) {
        printf("[ASAN]   <trace no longer in the depot>\n");
        return;
    }

    for (size_t i = 0; i < rec->num_entries; i++) {
        uint64_t off = 0;
        const char *sym = debug_symbolize(rec->entries[i], &off);

        if (sym)
            printf("[ASAN]   #%-2lu %p %s+0x%lx\n", (unsigned long) i,
                   (void *) rec->entries[i], sym, (unsigned long) off);
        else
            printf("[ASAN]   #%-2lu %p\n", (unsigned long) i,
                   (void *) rec->entries[i]);

        ndjson_emit(asan_frame, .idx = i, .addr = rec->entries[i], .sym = sym,
                    .off = off);
    }
}

/* Slabs keep a stack depot handle per object when DEBUG_SLAB_DEEP, so
 * we must tread carefully when retrieving the backtrace, lest we
 * dereference an unmapped page or cause other problems */
static void asan_report_owner(const void *addr) {
#ifdef DEBUG_SLAB_DEEP
    static bool reporting;

    if (reporting || !slab_ptr_in_slab((void *) addr))
        return;
    reporting = true;

    if (slab_order_map_get((vaddr_t) addr) == SLAB_POW2_ORDER_EMPTY) {
        printf("[ASAN] %p is in the slab heap but no chunk is mapped there\n",
               addr);
        goto out;
    }

    struct slab *s = slab_for_ptr((void *) addr);
    struct slab_cache *cache = s->parent_cache;

    /* Metadata may be what went wrong, so sanity check before trusting it */
    if ((uintptr_t) cache < SLAB_HEAP_START || !cache->obj_stride ||
        !cache->objs_per_slab || s->mem < (vaddr_t) s ||
        (vaddr_t) addr < s->mem) {
        printf("[ASAN] slab metadata at %p is not usable (cache=%p)\n",
               (void *) s, (void *) cache);
        goto out;
    }

    size_t idx = slab_allocation_index(s, (void *) addr);
    if (idx >= cache->objs_per_slab) {
        printf("[ASAN] object index %lu out of range for this slab\n",
               (unsigned long) idx);
        goto out;
    }

    stack_handle_t h = s->traces[idx];
    if (!h) {
        printf("[ASAN] object %lu of slab %p has no recorded allocation\n",
               (unsigned long) idx, (void *) s);
        goto out;
    }

    printf("[ASAN] object %lu of slab %p was last allocated at:\n",
           (unsigned long) idx, (void *) s);
    asan_report_stack(h);

out:
    reporting = false;
#else
    (void) addr;
#endif
}

static void __asan_report_and_panic(const char *what, const void *addr,
                                    size_t size, bool is_write) {
    printf("[ASAN] %s at %p size=%zu %s\n", what, addr, size,
           is_write ? "store" : "load");

    ndjson_emit(asan_fault, .what = what, .addr = (uint64_t) (uintptr_t) addr,
                .size = size, .access = is_write ? "store" : "load");
    asan_report_shadow(addr);
    asan_report_owner(addr);

    char msg[CRASH_MSG_MAX];
    snprintf(msg, sizeof(msg), "KASAN: %s at %p (size=%zu, %s)",
             what ? what : "<fault>", addr, size, is_write ? "store" : "load");
    crash(&(struct crash_context){
        .source = CRASH_SOURCE_KASAN,
        .formats = CRASH_FMT_DEFAULT,
        .file = __FILE__,
        .line = __LINE__,
        .func = __func__,
        .msg = msg,
    });
}

static const char *asan_poison_reason(uint8_t shadow) {
    switch (shadow) {
    case ASAN_POISON_HEAP_FREED: return "use-after-free";
    case ASAN_POISON_HEAP_REDZONE: return "heap redzone (out of bounds)";
    case ASAN_POISON_VALUE: return "poisoned (unallocated)";
    default:
        return asan_shadow_is_poison(shadow)
                   ? "poisoned"
                   : "heap overflow (partial granule)";
    }
}

static inline void asan_check_access_core(const void *addr, size_t size,
                                          bool is_write) {
    ASAN_ABORT_IF_NOT_READY();

    if (size == 0)
        return;

    if (!asan_addr_covered((uintptr_t) addr, size))
        return;

    uintptr_t start = (uintptr_t) addr;
    uintptr_t last = start + (size - 1);

    for (uintptr_t g = start & ~(ASAN_GRANULE - 1); g <= last;
         g += ASAN_GRANULE) {
        uint8_t s = *asan_shadow_for_internal((const void *) g);
        if (s == 0)
            continue;

        /* Bytes of this granule the access actually touches: [lo, hi) */
        uintptr_t hi =
            (last - g < ASAN_GRANULE - 1) ? (last - g) + 1 : ASAN_GRANULE;

        /* A partial granule keeps its first `s` bytes accessible */
        if (!asan_shadow_is_poison(s) && hi <= s)
            continue;

        __asan_report_and_panic(asan_poison_reason(s), addr, size, is_write);
        return;
    }
}

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
    asan_check_access_core(addr, size, true);
}

/* The compiler's spelling of the same two operations */
void __asan_poison_memory_region(void *addr, size_t size) {
    asan_poison(addr, size);
}

void __asan_unpoison_memory_region(void *addr, size_t size) {
    asan_unpoison(addr, size);
}

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

int __asan_option_detect_stack_use_after_return = 0;

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

void __asan_init(void) {
    asan_info("__asan_init runtime stub called");
}

void __asan_before_dynamic_init(const char *module_name) {
    (void) module_name;
}
void __asan_after_dynamic_init(void) {}

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
    __asan_poison_memory_region(addr, size);
}

void __asan_allocas_unpoison(void *addr, size_t size) {
    ASAN_ABORT_IF_NOT_READY();
    if (!addr || size == 0)
        return;
    __asan_unpoison_memory_region(addr, size);
}

__attribute__((weak)) void __asan_alloca_poison_0(void *addr, size_t size) {
    __asan_alloca_poison(addr, size);
}

__attribute__((weak)) void __asan_allocas_unpoison_0(void *addr, size_t size) {
    __asan_allocas_unpoison(addr, size);
}

#define ASAN_ALIAS(name, target) __attribute__((alias(#target))) void name

/* Outline callbacks (forced via -asan-instrumentation-with-call-threshold=0).
 */
ASAN_ALIAS(__asan_load1_noabort, __asan_load1)(const void *addr);
ASAN_ALIAS(__asan_load2_noabort, __asan_load2)(const void *addr);
ASAN_ALIAS(__asan_load4_noabort, __asan_load4)(const void *addr);
ASAN_ALIAS(__asan_load8_noabort, __asan_load8)(const void *addr);
ASAN_ALIAS(__asan_load16_noabort, __asan_load16)(const void *addr);
ASAN_ALIAS(__asan_store1_noabort, __asan_store1)(const void *addr);
ASAN_ALIAS(__asan_store2_noabort, __asan_store2)(const void *addr);
ASAN_ALIAS(__asan_store4_noabort, __asan_store4)(const void *addr);
ASAN_ALIAS(__asan_store8_noabort, __asan_store8)(const void *addr);
ASAN_ALIAS(__asan_store16_noabort, __asan_store16)(const void *addr);
ASAN_ALIAS(__asan_loadN_noabort, __asan_loadN)(const void *addr, size_t size);
ASAN_ALIAS(__asan_storeN_noabort, __asan_storeN)(const void *addr, size_t size);

ASAN_ALIAS(__asan_report_load1_noabort, __asan_report_load1)(void *addr);
ASAN_ALIAS(__asan_report_load2_noabort, __asan_report_load2)(void *addr);
ASAN_ALIAS(__asan_report_load4_noabort, __asan_report_load4)(void *addr);
ASAN_ALIAS(__asan_report_load8_noabort, __asan_report_load8)(void *addr);
ASAN_ALIAS(__asan_report_load16_noabort, __asan_report_load16)(void *addr);
ASAN_ALIAS(__asan_report_store1_noabort, __asan_report_store1)(void *addr);
ASAN_ALIAS(__asan_report_store2_noabort, __asan_report_store2)(void *addr);
ASAN_ALIAS(__asan_report_store4_noabort, __asan_report_store4)(void *addr);
ASAN_ALIAS(__asan_report_store8_noabort, __asan_report_store8)(void *addr);
ASAN_ALIAS(__asan_report_store16_noabort, __asan_report_store16)(void *addr);
ASAN_ALIAS(__asan_report_load_n_noabort, __asan_report_load_n)(void *addr,
                                                               size_t size);
ASAN_ALIAS(__asan_report_store_n_noabort, __asan_report_store_n)(void *addr,
                                                                 size_t size);
#endif
