#include <log.h>
#include <mem/address_range.h>
#include <mem/page.h>
#include <string.h>

LOG_SITE_DECLARE_PRINT(address_range);
LOG_HANDLE_DECLARE_PRINT(address_range);

#define ar_log(lvl, fmt, ...)                                                  \
    log(LOG_SITE(address_range), LOG_HANDLE(address_range), lvl, fmt,          \
        ##__VA_ARGS__)

#define ar_err(fmt, ...) ar_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define ar_warn(fmt, ...) ar_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define ar_info(fmt, ...) ar_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define ar_debug(fmt, ...) ar_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define ar_trace(fmt, ...) ar_log(LOG_TRACE, fmt, ##__VA_ARGS__)

static struct rbt ar_tree;

static void print_bytes(uint64_t bytes) {
    const uint64_t kib = 1024ULL;
    const uint64_t mib = 1024ULL * kib;
    const uint64_t gib = 1024ULL * mib;
    const uint64_t tib = 1024ULL * gib;
    const uint64_t pib = 1024ULL * tib;
    const uint64_t eib = 1024ULL * pib;

    if (bytes == 0) {
        printf("0 bytes\n");
        return;
    }

    uint64_t v;

    v = bytes / eib;
    if (v)
        printf("%llu eib", v);
    bytes %= eib;

    v = bytes / pib;
    if (v)
        printf("%llu pib", v);
    bytes %= pib;

    v = bytes / tib;
    if (v)
        printf("%llu tib", v);
    bytes %= tib;

    v = bytes / gib;
    if (v)
        printf("%llu gib", v);
    bytes %= gib;

    v = bytes / mib;
    if (v)
        printf("%llu mib", v);
    bytes %= mib;

    v = bytes / kib;
    if (v)
        printf("%llu kib", v);
    bytes %= kib;

    if (bytes) {
        printf("%llu bytes ", bytes);
    }

    printf("\n");
}

static inline size_t ar_end(struct address_range *ar) {
    return ar->base + ar->size;
}

static size_t ar_get_data(struct rbt_node *rn) {
    return container_of(rn, struct address_range, rbt_node_internal)->base;
}

static int32_t ar_cmp(const struct rbt_node *a, const struct rbt_node *b) {
    vaddr_t l = ar_get_data((void *) a);
    vaddr_t r = ar_get_data((void *) b);
    return (l > r) - (l < r);
}

static bool address_ranges_overlap(struct address_range *a,
                                   struct address_range *b) {
    return a->base < ar_end(b) && b->base < ar_end(a);
}

static void add_static_address_range(struct address_range *ar) {
    struct rbt_node *prev = rbt_find_predecessor(&ar_tree, ar->base);
    struct rbt_node *next = rbt_find_successor(&ar_tree, ar->base);

    if (prev) {
        struct address_range *p =
            rbt_entry(prev, struct address_range, rbt_node_internal);

        if (address_ranges_overlap(p, ar))
            panic("Address range '%s' overlaps '%s'", ar->name, p->name);
    }

    if (next) {
        struct address_range *n =
            rbt_entry(next, struct address_range, rbt_node_internal);

        if (address_ranges_overlap(ar, n))
            panic("Address range '%s' overlaps '%s'", ar->name, n->name);
    }

    rbt_insert(&ar_tree, &ar->rbt_node_internal);
}

static bool gap_fits(vaddr_t need_align, size_t need_size, vaddr_t gap_base,
                     vaddr_t gap_end, vaddr_t *out_base) {
    vaddr_t aligned = ALIGN_UP(gap_base, need_align);
    vaddr_t end = aligned + need_size;
    if (aligned < gap_end && end <= gap_end) {
        *out_base = aligned;
        return true;
    }
    return false;
}

static void add_dynamic_address_range(struct address_range *ar) {
    kassert(ar->base == 0);
    vaddr_t need_align = ar->align;
    size_t need_size = ar->size;

    struct rbt_node *node = rbt_min(&ar_tree);
    vaddr_t gap_base = ADDRESS_RANGE_KERNEL_START;

    while (node) {
        struct address_range *gap_ar =
            rbt_entry(node, struct address_range, rbt_node_internal);

        vaddr_t chosen;
        if (gap_fits(need_align, need_size, gap_base, gap_ar->base, &chosen)) {
            ar->base = chosen;
            rbt_insert(&ar_tree, &ar->rbt_node_internal);
            return;
        }

        gap_base = ar_end(gap_ar);
        node = rbt_next(node);
    }

    vaddr_t chosen;
    if (gap_fits(need_align, need_size, gap_base, ADDRESS_RANGE_KERNEL_END,
                 &chosen)) {
        ar->base = chosen;
        rbt_insert(&ar_tree, &ar->rbt_node_internal);
        return;
    }

    panic("No suitable gap for dynamic address range");
}

/* The idea behind this is as follows:
 *
 * We have a linker section full of struct address_range. For each
 * of these, we either
 *
 * 1. Add it to the address range tree, if it already has a base and size.
 *    a. Validate that it doesn't cross over any existing range
 *
 * 2. Find a spot to allocate it, and provide it with that base and size
 */
void address_ranges_init() {
    rbt_init(&ar_tree, ar_get_data, ar_cmp);

    for (struct address_range *ar = __skernel_address_ranges;
         ar < __ekernel_address_ranges; ar++) {
        if (!(ar->flags & ADDRESS_RANGE_DYNAMIC))
            add_static_address_range(ar);
    }

    for (struct address_range *ar = __skernel_address_ranges;
         ar < __ekernel_address_ranges; ar++) {
        if (ar->flags & ADDRESS_RANGE_DYNAMIC)
            add_dynamic_address_range(ar);
    }

    address_ranges_print();
}

static void format_size(char *buf, size_t bufsz, size_t bytes) {
    if (bytes >= (1ULL << 30)) {
        size_t whole = bytes >> 30;
        size_t frac = ((bytes & ((1ULL << 30) - 1)) * 100) >> 30;
        if (frac == 0) {
            snprintf(buf, bufsz, "%zu GiB", whole);
        } else {
            snprintf(buf, bufsz, "%zu.%02zu GiB", whole, frac);
        }
    } else if (bytes >= (1ULL << 20)) {
        size_t whole = bytes >> 20;
        size_t frac = ((bytes & ((1ULL << 20) - 1)) * 100) >> 20;
        if (frac == 0) {
            snprintf(buf, bufsz, "%zu MiB", whole);
        } else {
            snprintf(buf, bufsz, "%zu.%02zu MiB", whole, frac);
        }
    } else if (bytes >= (1ULL << 10)) {
        size_t whole = bytes >> 10;
        size_t frac = ((bytes & ((1ULL << 10) - 1)) * 100) >> 10;
        if (frac == 0) {
            snprintf(buf, bufsz, "%zu KiB", whole);
        } else {
            snprintf(buf, bufsz, "%zu.%02zu KiB", whole, frac);
        }
    } else {
        snprintf(buf, bufsz, "%zu B", bytes);
    }
}

#define AR_COL_ADDR 18
#define AR_LINE "─────────────────────────"
#define AR_SEP_TOP "0x%llx ┬───┬─────────────────────────"
#define AR_SEP_BOTTOM "0x%llx ┴───┴─────────────────────────"

static void ar_print_gap(vaddr_t gap_size, vaddr_t end) {
    char gapbuf[32];
    format_size(gapbuf, sizeof(gapbuf), gap_size);
    printf("0x%016llx ┼───┼" AR_LINE "\n", end);
    printf("                   │ O │\n");
    printf("                   │ O │ gap: %-13s\n", gapbuf);
    printf("                   │ O │\n");
}

void address_ranges_print() {
    size_t count = __ekernel_address_ranges - __skernel_address_ranges;
    ar_info("%zu address ranges:", count);

    struct address_range *ranges[count];
    size_t i = 0;
    struct rbt_node *rn;
    rbt_for_each(rn, &ar_tree) {
        ranges[i++] = container_of(rn, struct address_range, rbt_node_internal);
    }

    for (size_t a = 0; a < i; a++) {
        for (size_t b = a + 1; b < i; b++) {
            if (ranges[b]->base > ranges[a]->base) {
                struct address_range *tmp = ranges[a];
                ranges[a] = ranges[b];
                ranges[b] = tmp;
            }
        }
    }

    printf("\n");
    printf("%-*s       %s\n", AR_COL_ADDR, "  address", "region");

    if (i > 0) {
        vaddr_t top_end = ranges[0]->base + ranges[0]->size;
        if ((vaddr_t) ADDRESS_RANGE_KERNEL_END > top_end)
            ar_print_gap((vaddr_t) ADDRESS_RANGE_KERNEL_END - top_end,
                         ADDRESS_RANGE_KERNEL_END);
    }

    for (size_t k = 0; k < i; k++) {
        struct address_range *ar = ranges[k];
        vaddr_t end = ar->base + ar->size;
        char szbuf[32];
        format_size(szbuf, sizeof(szbuf), ar->size);

        size_t name_len = strlen(ar->name);

        printf("0x%016llx ┼───┼" AR_LINE "\n", end);
        printf("%-*s │ X │ %s: %-*s\n", AR_COL_ADDR, "", ar->name,
               AR_COL_ADDR - name_len - 2, szbuf);

        if (k + 1 < i) {
            vaddr_t next_end = ranges[k + 1]->base + ranges[k + 1]->size;
            if (ar->base > next_end)
                ar_print_gap(ar->base - next_end, ar->base);
        }
    }

    if (i > 0) {
        vaddr_t bot_base = ranges[i - 1]->base;
        if (bot_base > (vaddr_t) ADDRESS_RANGE_KERNEL_START)
            ar_print_gap(bot_base - (vaddr_t) ADDRESS_RANGE_KERNEL_START,
                         bot_base);
    }

    printf(AR_SEP_BOTTOM "\n\n", ADDRESS_RANGE_KERNEL_START);
}

struct address_range *address_range_for_addr(vaddr_t vaddr) {
    for (struct address_range *ar = __skernel_address_ranges;
         ar < __ekernel_address_ranges; ar++) {
        if (vaddr >= ar->base && vaddr <= ar_end(ar))
            return ar;
    }

    return NULL;
}
