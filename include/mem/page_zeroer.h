/* @title: Page Zeroer Subsystem */
#pragma once
#include <math/ewma.h>
#include <math/fixed.h>
#include <mem/buddy.h>
#include <thread/daemon.h>
#include <thread/workqueue.h>
#include <types/types.h>

/* Notes on the page zeroer architecture:
 *
 * We have a structure called `daemon`, in `daemon.h`, and our
 * page zeroer mechanism is essentially built off of how that works.
 *
 * The basic idea of `daemon`: you have 1 background thread and N
 * timesharing threads, and ONE background work and ONE timesharing work
 * that you can wake the threads to go and perform
 *
 * For the more low level aspects: we make one page zeroer per domain,
 * with one timesharing thread per 4 (TODO: subject to change) CPUs in
 * that domain. The idea is that pages are marked as zeroed at the buddy_page
 * level, and "zero pages push at front, non-zero push at tail" for buddy
 * allocator freelists.
 *
 * During coalescing, the allocator will need to check the full block to
 * determine if THE WHOLE THING is zeroed out, otherwise it will bail
 *
 * TODO: I'm thinking we could perhaps perform that part smarter? maybe
 * we can have a system where the zero pages at order > 0 are sorted,
 * but that would probably call for major expansion in struct page
 *
 * Anywho, we have a structure (TODO:) that manages the quotas
 * for the page zeroer to meet. This likely won't be global,
 * but I can try having some degree of a global structure on
 * a "summary" of the per-domain page zeroers.
 *
 * The background thread performs aging on this global structure,
 * as it runs whenever all other threads on that CPU are zzzzzz,
 * and it can also kick awake the timesharing threads if needed
 *
 * But the idea is that upon interacting with the page zeroer
 * interfaces, the quotas will be checked and the relevant
 * timesharing threads will be booted awake to do some work
 *
 * There is also a path in which the page zeroer can be instructed
 * to perform a specific task, and this involves things like the slowpath
 * in a zero alloc'd hugepage, in which case the timesharing threads
 * are instructed to zero out that hugepage
 *
 */

struct page_zeroer_demand {
    struct ewma rate;             /* consumed blocks per epoch */
    struct ewma mad;              /* mean abs deviation of `rate` */
    atomic_size_t consumed_epoch; /* fast-path inc, bg drain */
    atomic_size_t stockout_epoch; /* sync-zero fallbacks */
};

struct page_zeroer_rate {
    size_t batch_blocks;     /* blocks TS thread zeros per wake */
    fx32_32_t safety_factor; /* multiplier on demand for min_blocks */
    fx32_32_t integral;      /* PI integral state */
    fx32_32_t idle_scale;    /* [0, 1] until scheduler idle features come */
};

struct page_zeroer_watermark {
    size_t min_min_blocks;
    size_t max_max_blocks;

    size_t min_blocks; /* These are dynamic */
    size_t max_blocks;
};

/* TODO: once we get scheduler idleness integration, add it here */
struct page_zeroer_quota {
    struct page_zeroer_watermark wm;
    struct page_zeroer_rate rate;
    struct page_zeroer_demand demand;
    time_ns_t zero_block_time_ns;
    atomic_size_t num_zero_blocks;
};

struct page_zeroer {
    struct page_zeroer_quota quotas[BUDDY_MAX_ORDER];
    struct daemon *daemon;
};
