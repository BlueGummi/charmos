#include "mem/tests/test_internal.h"
#include "mem/vas_internal.h"
#include <asm.h>
#include <compiler/intrinsic.h>
TEST_GROUP_DEFINE(vas);

#define TEST_VAS_BASE 0x700000000000ULL

TEST_DEFINE_UNIT(vas, death_wrong_size, .enabled = TEST_STATE_DISABLED) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 4 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);

    vaddr_t addr = vas_alloc(vas, 2 * PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(addr, 0);

    vas_free(vas, addr, PAGE_SIZE);
    return TEST_FAIL("wrong-size free was accepted");
}

TEST_DEFINE_UNIT(vas, death_double_free, .enabled = TEST_STATE_DISABLED) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 4 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);

    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(addr, 0);
    vas_free(vas, addr, PAGE_SIZE);
    vas_free(vas, addr, PAGE_SIZE);

    return TEST_FAIL("double free was accepted");
}

TEST_DEFINE_UNIT(vas, death_out_of_range, .enabled = TEST_STATE_DISABLED) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 4 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);
    vas_free(vas, TEST_VAS_BASE - 1, PAGE_SIZE);
    return TEST_FAIL("out-of-range free was accepted");
}

static bool arena_valid(struct vas_arena *arena, vaddr_t base, vaddr_t limit,
                        size_t *live, size_t *cached, size_t *enrolled) {
    size_t free_bytes = 0, free_tags = 0, binned_tags = 0;
    vaddr_t end = base;
    struct vas_segment *prev = NULL;
    struct rbt_node *node = rbt_first(&arena->tree);
    struct list_head *pos;
    list_for_each(pos, &arena->all_segs) {
        struct vas_segment *seg = list_entry(pos, struct vas_segment, seg_node);
        if (!seg->length || seg->start < end || seg->start >= limit ||
            seg->length > limit - seg->start || node != &seg->node)
            return false;
        enum vas_segment_type type = atomic_load_rlx(&seg->type);
        enum vas_segment_type prev_type =
            prev ? atomic_load_rlx(&prev->type) : VAS_SEG_BUSY;
        if (prev && prev_type == VAS_SEG_FREE && type == VAS_SEG_FREE &&
            prev->span_start == seg->span_start && end == seg->start)
            return false;
        end = seg->start + seg->length;
        if (type == VAS_SEG_FREE) {
            free_bytes += seg->length;
            free_tags++;
        } else if (type == VAS_SEG_BUSY) {
            *live += seg->length;
        } else if (type == VAS_SEG_CACHED) {
            *cached += seg->length;
            if (!seg->mag_slot)
                return false;
        }
        if (seg->mag_slot) {
            uintptr_t state =
                type == VAS_SEG_BUSY ? VAS_MAG_LIVE : VAS_MAG_CACHED;
            if ((type != VAS_SEG_BUSY && type != VAS_SEG_CACHED) ||
                seg->mag_slot->segment != seg ||
                atomic_load(&seg->mag_slot->token) != (seg->start | state))
                return false;
            *enrolled += seg->length;
        }
        if (type != VAS_SEG_FREE && !list_empty(&seg->bin_node))
            return false;
        prev = seg;
        node = rbt_next(node);
    }
    if (node || free_bytes != arena->total_free)
        return false;
    uint64_t mask = 0;
    for (uint32_t bin = 0; bin < VAS_BIN_COUNT; bin++) {
        list_for_each(pos, &arena->free_bins[bin]) {
            struct vas_segment *seg =
                list_entry(pos, struct vas_segment, bin_node);
            if (atomic_load_rlx(&seg->type) != VAS_SEG_FREE ||
                (63U - ci_clzll(seg->length)) != bin ||
                rbt_search(&arena->tree, seg->start) != &seg->node)
                return false;
            mask |= BIT(bin);
            if (++binned_tags > free_tags)
                return false;
        }
    }
    const size_t sizes[] = {PAGE_SIZE, 5 * PAGE_SIZE, 17 * PAGE_SIZE, PAGE_2MB};
    for (int cls = 0; cls < VAS_MAG_CLASSES; cls++) {
        for (int i = 0; i < VAS_MAG_CAPACITY; i++) {
            struct vas_mag_slot *slot = &arena->magazines[cls].slots[i];
            uintptr_t token = atomic_load(&slot->token);
            if (!token)
                continue;
            if ((token & VAS_MAG_STATE_MASK) == VAS_MAG_CLAIMED ||
                !slot->segment || slot->segment->mag_slot != slot ||
                slot->segment->length != sizes[cls] ||
                rbt_search(&arena->tree, token & ~VAS_MAG_STATE_MASK) !=
                    &slot->segment->node)
                return false;
        }
    }
    return mask == arena->bin_mask && binned_tags == free_tags;
}

static bool vas_valid(struct vas *vas, size_t expected_live) {
    size_t live = 0, free_bytes = vas->global.total_free;
    size_t cached = 0, enrolled = 0;
    if (!arena_valid(&vas->global, vas->base, vas->limit, &live, &cached,
                     &enrolled))
        return false;
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++) {
        struct vas_arena *arena = &vas->local[cpu];
        if (!arena_valid(arena, vas->base, vas->limit, &live, &cached,
                         &enrolled))
            return false;
        free_bytes += arena->total_free;
        struct list_head *pos;
        list_for_each(pos, &arena->all_segs) {
            struct vas_segment *seg =
                list_entry(pos, struct vas_segment, seg_node);
            size_t index = (seg->start - vas->map_base) >> VAS_CHUNK_SHIFT;
            struct rbt_node *parent =
                rbt_search(&vas->global.tree, seg->span_start);
            if (!parent || atomic_load(&vas->chunk_owner[index]) != cpu ||
                seg->start < seg->span_start ||
                seg->start - seg->span_start + seg->length > VAS_CHUNK_SIZE ||
                atomic_load_rlx(
                    &rbt_entry(parent, struct vas_segment, node)->type) !=
                    VAS_SEG_IMPORTED)
                return false;
        }
    }
    return live == expected_live &&
           live + cached + free_bytes == vas->limit - vas->base &&
           enrolled == atomic_load(&vas->mag_reserved_bytes) &&
           enrolled <= VAS_MAG_BYTE_LIMIT;
}

TEST_DEFINE_UNIT(vas, magazine_hits_preserve_query_and_exact_free)
TSA_NO_ANALYSIS {
    const size_t sizes[] = {PAGE_SIZE, 5 * PAGE_SIZE, 17 * PAGE_SIZE, PAGE_2MB};
    for (uint32_t cls = 0; cls < VAS_MAG_CLASSES; cls++) {
        struct vas *vas =
            vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
        TEST_ASSERT_NONNULL(vas);
        enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
        struct vas_arena *arena = &vas->local[smp_id(TOPC_IRQL)];
        vaddr_t addr = vas_alloc(vas, sizes[cls], PAGE_SIZE);
        TEST_ASSERT_NE(addr, 0);
        TEST_ASSERT(vas_valid(vas, sizes[cls]));
        TEST_ASSERT_FALSE(vas_destroy(vas));
        vas_free(vas, addr, sizes[cls]);
        TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, addr));
        TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, addr + sizes[cls] - 1));
        TEST_ASSERT(vas_valid(vas, 0));
        arena->tag_alloc_budget = 0;
        vas->global.tag_alloc_budget = 0;
        uint64_t hits = arena->mag_alloc_hits;
        uint64_t frees = arena->mag_free_hits;
        /* Holding the arena lock makes accidental slow-path entry detectable
         * by lock checking (and the test watchdog in ordinary builds). */
        enum irql locked = spin_lock(&arena->lock);
        bool reused = true;
        for (uint32_t i = 0; i < 100; i++) {
            vaddr_t next = vas_alloc(vas, sizes[cls], PAGE_SIZE);
            reused &= next == addr;
            vas_free(vas, next, sizes[cls]);
        }
        spin_unlock(&arena->lock, locked);
        TEST_ASSERT(reused);
        TEST_ASSERT_EQ(arena->mag_alloc_hits - hits, 100);
        TEST_ASSERT_EQ(arena->mag_free_hits - frees, 100);
        TEST_ASSERT(vas_valid(vas, 0));
        TEST_ASSERT(vas_destroy(vas));
        irql_lower(old);
    }
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, magazine_alignment_overflow_and_drain)
TSA_NO_ANALYSIS {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    vaddr_t first = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    vaddr_t unaligned = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(unaligned & (PAGE_2MB - 1), 0);
    vas_free(vas, unaligned, PAGE_SIZE);
    vaddr_t aligned = vas_alloc(vas, PAGE_SIZE, PAGE_2MB);
    TEST_ASSERT_NE(aligned, 0);
    TEST_ASSERT_NE(aligned, first);
    TEST_ASSERT_EQ(aligned & (PAGE_2MB - 1), 0);
    vas_free(vas, first, PAGE_SIZE);
    vas_free(vas, aligned, PAGE_SIZE);
    vaddr_t addresses[VAS_MAG_CAPACITY + 5];
    for (uint32_t i = 0; i < TEST_ARRAY_LEN(addresses); i++) {
        addresses[i] = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
        TEST_ASSERT_NE(addresses[i], 0);
    }
    TEST_ASSERT(
        vas_valid(vas, sizeof(addresses) / sizeof(*addresses) * PAGE_SIZE));
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes),
                   VAS_MAG_CAPACITY * PAGE_SIZE);
    for (uint32_t i = 0; i < TEST_ARRAY_LEN(addresses); i++)
        vas_free(vas, addresses[i], PAGE_SIZE);
    TEST_ASSERT(vas_valid(vas, 0));
    /* A giant request must drain cached reservations, not falsely exhaust. */
    TEST_ASSERT_EQ(vas_alloc(vas, VAS_CHUNK_SIZE, VAS_CHUNK_SIZE),
                   TEST_VAS_BASE);
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), 0);
    TEST_ASSERT(vas_valid(vas, VAS_CHUNK_SIZE));
    vas_free(vas, TEST_VAS_BASE, VAS_CHUNK_SIZE);
    vas_reclaim(vas);
    TEST_ASSERT_EQ(vas->global.total_free, VAS_CHUNK_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    irql_lower(old);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, magazine_refill_failure_keeps_primary_allocation)
TSA_NO_ANALYSIS {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    /* Import tag + the primary allocation's suffix, no speculative suffix. */
    vas->local[smp_id(TOPC_IRQL)].tag_alloc_budget = 2;
    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_EQ(addr, TEST_VAS_BASE);
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), PAGE_SIZE);
    TEST_ASSERT(vas_valid(vas, PAGE_SIZE));
    vas_free(vas, addr, PAGE_SIZE);
    TEST_ASSERT_EQ(vas_alloc(vas, PAGE_SIZE, PAGE_SIZE), addr);
    vas_free(vas, addr, PAGE_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    irql_lower(old);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, magazine_claim_pins_tag_during_drain)
TSA_NO_ANALYSIS {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(addr, 0);
    struct vas_arena *arena = &vas->local[smp_id(TOPC_IRQL)];
    struct vas_segment *seg =
        rbt_entry(rbt_search(&arena->tree, addr), struct vas_segment, node);
    struct vas_mag_slot *slot = seg->mag_slot;
    TEST_ASSERT_NONNULL(slot);
    vas_free(vas, addr, PAGE_SIZE);

    uintptr_t token = vas_token_make(addr, VAS_MAG_CACHED);
    TEST_ASSERT(atomic_cas_strong(&slot->token, &token,
                                  vas_token_make(addr, VAS_MAG_CLAIMED),
                                  mo_acquire, mo_rlx));
    vas_reclaim(vas);
    TEST_ASSERT_EQ(atomic_load(&vas->mag_reserved_bytes), PAGE_SIZE);
    TEST_ASSERT_EQ(rbt_search(&arena->tree, addr), &seg->node);
    TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, addr));
    atomic_store(&seg->type, VAS_SEG_BUSY);
    atomic_store(&slot->token, vas_token_make(addr, VAS_MAG_LIVE));
    TEST_ASSERT(vas_vaddr_is_allocated(vas, addr + PAGE_SIZE - 1));
    TEST_ASSERT(vas_valid(vas, PAGE_SIZE));
    vas_free(vas, addr, PAGE_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    irql_lower(old);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, magazine_benchmark, .enabled = TEST_STATE_DISABLED,
                 .print_logs = true)
TSA_NO_ANALYSIS {
    const size_t sizes[] = {PAGE_SIZE, 5 * PAGE_SIZE, 17 * PAGE_SIZE, PAGE_2MB};
    const uint32_t iterations = 10000;
    for (uint32_t cls = 0; cls < TEST_ARRAY_LEN(sizes); cls++) {
        for (uint32_t cached = 0; cached < 2; cached++) {
            struct vas *vas =
                vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
            TEST_ASSERT_NONNULL(vas);
            vas->magazines_disabled = !cached;
            enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
            struct vas_arena *arena = &vas->local[smp_id(TOPC_IRQL)];
            size_t align = cls == 3 ? PAGE_2MB : PAGE_SIZE;
            vaddr_t warm = vas_alloc(vas, sizes[cls], align);
            TEST_ASSERT_NE(warm, 0);
            vas_free(vas, warm, sizes[cls]);
            uint64_t hits = arena->mag_alloc_hits;
            uint64_t start = rdtsc_ordered();
            for (uint32_t i = 0; i < iterations; i++) {
                vaddr_t addr = vas_alloc(vas, sizes[cls], align);
                TEST_ASSERT_NE(addr, 0);
                vas_free(vas, addr, sizes[cls]);
            }
            uint64_t ticks = rdtsc_ordered() - start;
            TEST_ASSERT_EQ(arena->mag_alloc_hits - hits,
                           cached ? iterations : 0);
            TEST_ASSERT_EQ(arena->requests[cls], iterations + 1);
            TEST_ASSERT_EQ(arena->alignments[cls == 3 ? 21 : 12],
                           iterations + 1);
            size_t retained = atomic_load(&vas->mag_reserved_bytes);
            TEST_ASSERT(vas_valid(vas, 0));
            TEST_ASSERT(vas_destroy(vas));
            irql_lower(old);
            test_info("size=%zu align=%zu cached=%u ticks/pair=%llu "
                      "retained=%zu hits=%u/%u",
                      sizes[cls], align, cached, ticks / iterations, retained,
                      cached ? iterations : 0, iterations);
        }
    }
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, death_cached_double_free, .enabled = TEST_STATE_DISABLED)
TSA_NO_ANALYSIS {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(addr, 0);
    vas_free(vas, addr, PAGE_SIZE);
    vas_free(vas, addr, PAGE_SIZE);
    irql_lower(old);
    return TEST_FAIL("cached double free was accepted");
}

TEST_DEFINE_UNIT(vas, death_magazine_wrong_size,
                 .enabled = TEST_STATE_DISABLED) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    vaddr_t addr = vas_alloc(vas, 5 * PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(addr, 0);
    vas_free(vas, addr, PAGE_SIZE);
    return TEST_FAIL("magazine wrong-size free was accepted");
}

TEST_DEFINE_UNIT(vas, byte_sizes_and_exact_bin_fit) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 4 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);
    vaddr_t three_pages = vas_alloc(vas, 3 * PAGE_SIZE, PAGE_SIZE);
    vaddr_t last = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_EQ(three_pages, TEST_VAS_BASE);
    TEST_ASSERT_EQ(last, TEST_VAS_BASE + 3 * PAGE_SIZE);
    vas_free(vas, three_pages, 3 * PAGE_SIZE);
    TEST_ASSERT_EQ(vas_alloc(vas, 3 * PAGE_SIZE, PAGE_SIZE), three_pages);
    vas_free(vas, three_pages, 3 * PAGE_SIZE);
    vas_free(vas, last, PAGE_SIZE);
    vaddr_t small = vas_alloc(vas, 13, 1);
    vaddr_t next = vas_alloc(vas, 37, 16);
    TEST_ASSERT_EQ(small, TEST_VAS_BASE);
    TEST_ASSERT_EQ(next, TEST_VAS_BASE + 16);
    TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, small + 13));
    TEST_ASSERT(vas_vaddr_is_allocated(vas, next + 36));
    TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, next + 37));
    TEST_ASSERT(vas_valid(vas, 50));
    vas_free(vas, small, 13);
    vas_free(vas, next, 37);
    TEST_ASSERT(vas_valid(vas, 0));
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, alignment_and_partial_edge_buckets) {
    vaddr_t base = TEST_VAS_BASE + PAGE_SIZE;
    struct vas *vas = vas_create(base, TEST_VAS_BASE + 2 * PAGE_1GB);
    TEST_ASSERT_NONNULL(vas);
    size_t aligns[] = {PAGE_SIZE, KIB(64), PAGE_2MB, PAGE_1GB};
    vaddr_t addresses[TEST_ARRAY_LEN(aligns)];
    for (size_t i = 0; i < TEST_ARRAY_LEN(aligns); i++) {
        addresses[i] = vas_alloc(vas, 3 * PAGE_SIZE, aligns[i]);
        TEST_ASSERT_NE(addresses[i], 0);
        TEST_ASSERT(IS_ALIGNED(addresses[i], aligns[i]));
        for (size_t j = 0; j < i; j++)
            TEST_ASSERT(addresses[i] + 3 * PAGE_SIZE <= addresses[j] ||
                        addresses[j] + 3 * PAGE_SIZE <= addresses[i]);
    }

    /* The unaligned prefix must remain globally usable */
    vaddr_t prefix = vas_alloc(vas, VAS_CHUNK_SIZE - PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_NE(prefix, 0);
    TEST_ASSERT(vas_valid(vas, VAS_CHUNK_SIZE + 11 * PAGE_SIZE));
    vas_free(vas, prefix, VAS_CHUNK_SIZE - PAGE_SIZE);
    for (size_t i = 0; i < TEST_ARRAY_LEN(aligns); i++)
        vas_free(vas, addresses[i], 3 * PAGE_SIZE);
    vas_reclaim(vas);
    TEST_ASSERT_EQ(vas->global.total_free, vas->limit - vas->base);
    TEST_ASSERT_EQ(atomic_load(&vas->chunk_owner[0]), VAS_OWNER_GLOBAL);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, all_coalescing_orders) {
    const uint32_t orders[][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
    for (size_t order = 0; order < TEST_ARRAY_LEN(orders); order++) {
        struct vas *vas =
            vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 3 * PAGE_SIZE);
        TEST_ASSERT_NONNULL(vas);
        vaddr_t a[3];
        for (size_t i = 0; i < 3; i++) {
            a[i] = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
            TEST_ASSERT_EQ(a[i], TEST_VAS_BASE + i * PAGE_SIZE);
        }
        /* Free remains possible with no available tag allocations. */
        vas->global.tag_alloc_budget = 0;
        for (size_t i = 0; i < 3; i++) {
            vas_free(vas, a[orders[order][i]], PAGE_SIZE);
            TEST_ASSERT(vas_valid(vas, (2 - i) * PAGE_SIZE));
        }
        TEST_ASSERT_EQ(vas_alloc(vas, 3 * PAGE_SIZE, PAGE_SIZE), TEST_VAS_BASE);
        vas_free(vas, TEST_VAS_BASE, 3 * PAGE_SIZE);
        TEST_ASSERT(vas_destroy(vas));
    }
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, invalid_requests_and_high_address_arithmetic) {
    TEST_ASSERT_NULL(vas_create(0, PAGE_SIZE));
    TEST_ASSERT_NULL(vas_create(TEST_VAS_BASE, TEST_VAS_BASE));
    TEST_ASSERT_NULL(vas_create(TEST_VAS_BASE, TEST_VAS_BASE - 1));
    vaddr_t base = UINTPTR_MAX - (4 * PAGE_SIZE - 1);
    struct vas *vas = vas_create(base, UINTPTR_MAX);
    TEST_ASSERT_NONNULL(vas);
    TEST_ASSERT_EQ(vas_alloc(vas, 0, PAGE_SIZE), 0);
    TEST_ASSERT_EQ(vas_alloc(vas, 1, 0), 0);
    TEST_ASSERT_EQ(vas_alloc(vas, 1, 3), 0);
    TEST_ASSERT_EQ(vas_alloc(vas, SIZE_MAX, PAGE_SIZE), 0);
    TEST_ASSERT_EQ(vas_alloc(vas, PAGE_SIZE, BIT(63)), 0);
    TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, base - 1));
    TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, UINTPTR_MAX));
    size_t length = UINTPTR_MAX - base;
    TEST_ASSERT_EQ(vas_alloc(vas, length, 1), base);
    TEST_ASSERT(vas_vaddr_is_allocated(vas, UINTPTR_MAX - 1));
    TEST_ASSERT_EQ(vas_alloc(vas, 1, 1), 0);
    TEST_ASSERT_FALSE(vas_destroy(vas));
    vas_free(vas, base, length);
    TEST_ASSERT(vas_valid(vas, 0));
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, metadata_failure_rolls_back_both_splits) {
    vaddr_t base = TEST_VAS_BASE + 1;
    struct vas *vas = vas_create(base, base + 4 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);
    for (ssize_t budget = 0; budget < 2; budget++) {
        vas->global.tag_alloc_budget = budget;
        TEST_ASSERT_EQ(vas_alloc(vas, PAGE_SIZE, PAGE_SIZE), 0);
        TEST_ASSERT(vas_valid(vas, 0));
        TEST_ASSERT_EQ(vas->global.total_free, 4 * PAGE_SIZE);
        TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, base));
    }
    vas->global.tag_alloc_budget = -1;
    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_EQ(addr, TEST_VAS_BASE + PAGE_SIZE);
    vas_free(vas, addr, PAGE_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, import_failure_rolls_back_ownership) {
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    for (ssize_t budget = 0; budget < 2; budget++) {
        for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++)
            vas->local[cpu].tag_alloc_budget = budget;
        TEST_ASSERT_EQ(vas_alloc(vas, PAGE_SIZE, PAGE_SIZE), 0);
        TEST_ASSERT_EQ(atomic_load(&vas->chunk_owner[0]), VAS_OWNER_GLOBAL);
        TEST_ASSERT(vas_valid(vas, 0));
    }
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++)
        vas->local[cpu].tag_alloc_budget = -1;
    vaddr_t addr = vas_alloc(vas, PAGE_SIZE, PAGE_SIZE);
    TEST_ASSERT_EQ(addr, TEST_VAS_BASE);
    vas_free(vas, addr, PAGE_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, large_fallback_reclaims_adjacent_imports) {
    struct vas *vas =
        vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 2 * VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    vaddr_t a = vas_alloc(vas, VAS_CHUNK_SIZE, VAS_CHUNK_SIZE);
    vaddr_t b = vas_alloc(vas, VAS_CHUNK_SIZE, VAS_CHUNK_SIZE);
    TEST_ASSERT_NE(a, 0);
    TEST_ASSERT_NE(b, 0);
    TEST_ASSERT_NE(a, b);
    TEST_ASSERT_EQ(vas->global.total_free, 0);
    vas_free(vas, a, VAS_CHUNK_SIZE);
    vas_free(vas, b, VAS_CHUNK_SIZE);
    TEST_ASSERT(vas_valid(vas, 0));
    vaddr_t large = vas_alloc(vas, 2 * VAS_CHUNK_SIZE, PAGE_SIZE);
    TEST_ASSERT_EQ(large, TEST_VAS_BASE);
    TEST_ASSERT_EQ(atomic_load(&vas->chunk_owner[0]), VAS_OWNER_GLOBAL);
    TEST_ASSERT_EQ(atomic_load(&vas->chunk_owner[1]), VAS_OWNER_GLOBAL);
    TEST_ASSERT(vas_vaddr_is_allocated(vas, large + VAS_CHUNK_SIZE + 7));
    TEST_ASSERT(vas_valid(vas, 2 * VAS_CHUNK_SIZE));
    vas_free(vas, large, 2 * VAS_CHUNK_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, bootstrap_storage_and_repeated_teardown) {
    for (size_t i = 0; i < 8; i++) {
        struct vas *vas =
            i & 1 ? vas_bootstrap(TEST_VAS_BASE, TEST_VAS_BASE + (8ULL << 40))
                  : vas_create(TEST_VAS_BASE, TEST_VAS_BASE + PAGE_1GB);
        TEST_ASSERT_NONNULL(vas);
        if (i & 1)
            TEST_ASSERT_GT(vas->bootstrap_pages, 1);
        vaddr_t addr = vas_alloc(vas, PAGE_2MB, PAGE_2MB);
        TEST_ASSERT_NE(addr, 0);
        vas_free(vas, addr, PAGE_2MB);
        TEST_ASSERT(vas_valid(vas, 0));
        TEST_ASSERT(vas_destroy(vas));
    }
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, fragmented_small_arena_has_no_false_exhaustion) {
    enum { SLOTS = 32, BYTES = 2048, OPERATIONS = 1000 };
    uint8_t occupied[BYTES] = {0};
    struct {
        vaddr_t addr;
        size_t size;
    } slots[SLOTS] = {0};
    struct vas *vas = vas_create(TEST_VAS_BASE, TEST_VAS_BASE + BYTES);
    TEST_ASSERT_NONNULL(vas);
    uint64_t random = 0xfee12345;
    for (size_t op = 0; op < OPERATIONS; op++) {
        size_t slot = prng_splitmix64_next(&random) % SLOTS;
        if (slots[slot].addr) {
            size_t offset = slots[slot].addr - TEST_VAS_BASE;
            memset(occupied + offset, 0, slots[slot].size);
            vas_free(vas, slots[slot].addr, slots[slot].size);
            slots[slot].addr = 0;
        } else {
            size_t size = 1 + prng_splitmix64_next(&random) % 127;
            size_t align = BIT(prng_splitmix64_next(&random) % 8);
            bool fits = false;
            for (size_t offset = 0; offset + size <= BYTES; offset += align) {
                size_t n = 0;
                while (n < size && !occupied[offset + n])
                    n++;
                if (n == size) {
                    fits = true;
                    break;
                }
            }
            vaddr_t addr = vas_alloc(vas, size, align);
            TEST_ASSERT_EQ(addr != 0, fits);
            if (addr) {
                TEST_ASSERT_EQ(addr & (align - 1), 0);
                size_t offset = addr - TEST_VAS_BASE;
                TEST_ASSERT(offset + size <= BYTES);
                for (size_t n = 0; n < size; n++) {
                    TEST_ASSERT_EQ(occupied[offset + n], 0);
                    occupied[offset + n] = 1;
                }
                slots[slot].addr = addr;
                slots[slot].size = size;
            }
        }
    }
    for (size_t i = 0; i < SLOTS; i++)
        if (slots[i].addr)
            vas_free(vas, slots[i].addr, slots[i].size);
    TEST_ASSERT(vas_valid(vas, 0));
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, partial_bucket_and_alignment_rejection) {
    vaddr_t base = TEST_VAS_BASE + PAGE_SIZE;
    struct vas *vas = vas_create(base, base + 2 * PAGE_SIZE);
    TEST_ASSERT_NONNULL(vas);
    TEST_ASSERT_EQ(vas_alloc(vas, 2 * PAGE_SIZE, 2 * PAGE_SIZE), 0);
    TEST_ASSERT_EQ(vas_alloc(vas, 2 * PAGE_SIZE, PAGE_SIZE), base);
    TEST_ASSERT_EQ(atomic_load(&vas->chunk_owner[0]), VAS_OWNER_GLOBAL);
    vas_free(vas, base, 2 * PAGE_SIZE);
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(vas, seeded_churn_against_interval_oracle) {
    enum { SLOTS = 64, OPERATIONS = 3000 };
    struct {
        vaddr_t addr;
        size_t size;
    } slots[SLOTS] = {0};
    struct vas *vas =
        vas_create(TEST_VAS_BASE, TEST_VAS_BASE + 8 * VAS_CHUNK_SIZE);
    TEST_ASSERT_NONNULL(vas);
    uint64_t seed = ctx->seed ? ctx->seed : 0x5641532026ULL;
    uint64_t random = seed;
    size_t live = 0;
    for (size_t op = 0; op < OPERATIONS; op++) {
        size_t slot = prng_splitmix64_next(&random) % SLOTS;
        if (slots[slot].addr) {
            vas_free(vas, slots[slot].addr, slots[slot].size);
            TEST_ASSERT_FALSE(vas_vaddr_is_allocated(vas, slots[slot].addr));
            live -= slots[slot].size;
            slots[slot].addr = 0;
        } else {
            size_t size = 1 + prng_splitmix64_next(&random) % MIB(16);
            size_t align = BIT(prng_splitmix64_next(&random) % 25);
            vaddr_t addr = vas_alloc(vas, size, align);
            if (addr) {
                TEST_ASSERT(IS_ALIGNED(addr, align));
                TEST_ASSERT(addr >= vas->base && size <= vas->limit - addr);
                for (size_t j = 0; j < SLOTS; j++)
                    if (slots[j].addr)
                        TEST_ASSERT_MSG(addr + size <= slots[j].addr ||
                                            slots[j].addr + slots[j].size <=
                                                addr,
                                        "overlap seed=%llu op=%zu", seed, op);
                slots[slot].addr = addr;
                slots[slot].size = size;
                live += size;
                TEST_ASSERT(vas_vaddr_is_allocated(vas, addr + size - 1));
            }
        }
        if (op % 29 == 0)
            vas_reclaim(vas);
        TEST_ASSERT_MSG(vas_valid(vas, live), "seed=%llu op=%zu", seed, op);
    }
    for (size_t i = 0; i < SLOTS; i++)
        if (slots[i].addr)
            vas_free(vas, slots[i].addr, slots[i].size);
    TEST_ASSERT(vas_valid(vas, 0));
    TEST_ASSERT(vas_destroy(vas));
    return TEST_SUCCESS;
}
