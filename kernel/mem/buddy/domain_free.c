#include <misc/align.h>
#include <mp/domain.h>

#include "internal.h"

static bool try_push_page_onto_arena(struct domain_arena *arena,
                                     paddr_t address) {
    struct buddy_page *buddy = buddy_page_for_addr(address);
    return domain_arena_push(arena, buddy);
}

static bool try_push_page_onto_domain_arenas(struct domain_buddy *domain,
                                             paddr_t address) {
    struct domain_arena *arena;
    struct buddy_page *this_page = buddy_page_for_addr(address);

    domain_for_each_arena(domain, arena) {
        if (domain_arena_push(arena, this_page))
            return true;
    }

    return false;
}

/* First we try and add to our arena cache. If that fails,
 * we try the next one, until an arena cache is able to
 * hold the freed page. If none of these succeed, we give up and just free it */
static void free_from_local_domain_buddy(struct domain_buddy *local,
                                         paddr_t address, size_t page_count) {
    if (page_count > 1)
        return free_from_buddy_internal(local, address, page_count);

    struct domain_arena *local_arena = domain_arena_on_this_core();

    /* Try local core first */
    if (try_push_page_onto_arena(local_arena, address))
        return;

    /* Try putting the page on other cores */
    if (try_push_page_onto_domain_arenas(local, address))
        return;

    /* No success? Let's flush it from the buddy */
    free_from_buddy_internal(local, address, page_count);
}

/* First, we try and enqueue it onto the remote freequeue.
 *
 * If this fails, and we have a multi-page free, then we flush it to
 * their buddy. If this fails, and we have a single-page free, then
 * we first try to push the page onto the remote arenas.
 *
 * If that fails, (on single-page frees), then we flush it to their buddy
 */
static void free_from_remote_domain_buddy(struct domain_buddy *remote,
                                          paddr_t address, size_t page_count) {
    struct domain_free_queue *remote_freequeue = remote->free_queue;

    if (domain_free_queue_enqueue(remote_freequeue, address, page_count))
        return;

    if (page_count > 1)
        return free_from_buddy_internal(remote, address, page_count);

    if (try_push_page_onto_domain_arenas(remote, address))
        return;

    free_from_buddy_internal(remote, address, page_count);
}

static inline struct domain_arena *
domain_first_arena(struct domain_buddy *domain) {
    return domain->arenas[0];
}

static struct domain_arena *get_next_domain_arena(struct domain_buddy *domain,
                                                  size_t *current_arena_idx) {
    if (++*current_arena_idx < domain->core_count)
        return domain->arenas[*current_arena_idx];

    /* Nope, let's flush to the buddy */
    return NULL;
}

static struct domain_arena *find_non_full_arena(struct domain_buddy *domain,
                                                size_t *current_arena_idx) {
    while (true) {
        struct domain_arena *ret =
            get_next_domain_arena(domain, current_arena_idx);
        if (!ret)
            return NULL;

        if (ret->num_pages < ret->capacity)
            return ret;
    }
}

static size_t compute_min_elements_to_free(struct domain_buddy *domain,
                                           struct domain_free_queue *queue) {

    size_t total_slots_available = 0;
    struct domain_arena *curr;

    domain_for_each_arena(domain, curr) {
        total_slots_available += curr->capacity - curr->num_pages;
    }

    size_t target = queue->num_elements / 2;
    if (target > total_slots_available)
        target = total_slots_available;

    return target > 0 ? target : 1;
}

static void flush_free_queue_internal(struct domain_buddy *domain,
                                      struct domain_free_queue *queue,
                                      size_t minimum_to_free) {
    /* current_arena becomes NULL once we have filled all arenas */
    struct domain_arena *current_arena = domain_first_arena(domain);
    size_t current_arena_idx = 0;
    for (size_t i = 0; i < minimum_to_free; i++) {
        size_t addr = 0, page_count = 0;
        domain_free_queue_dequeue(queue, &addr, &page_count);

        /* We are done. Free queue is empty */
        if (addr == 0)
            return;

        if (page_count > 1 || current_arena == NULL) {
            free_from_buddy_internal(domain, addr, page_count);
            continue;
        }

        struct buddy_page *this = buddy_page_for_addr(addr);
        if (domain_arena_push(current_arena, this)) /* Pushed it onto arena */
            continue;

        /* Arena was full. Let's move onto the next one */
        current_arena = find_non_full_arena(domain, &current_arena_idx);
        if (!current_arena) {
            free_from_buddy_internal(domain, addr, page_count);
        } else {
            domain_arena_push(current_arena, this);
        }
    }
}

/* Flushing our free queue goes as follows:
 *
 * First, we figure out how many elements we should AT LEAST
 * free. We free until that is met, or if we have
 * freed everything in the freequeue.
 *
 * done = freed_minimum || freequeue_empty
 *
 * As we flush the freequeue, we set a target arena to fill.
 *
 * If we are flushing single pages, we try and enqueue onto
 * this arena. If the arena is full, then we move onto the
 * next arena. If all arenas are full, then we actually
 * start flushing to the main buddy.
 *
 * For frees of more than one page, we don't bother with
 * the arenas, and we just flush to the main buddy.
 */
void domain_flush_free_queue(struct domain_buddy *domain,
                             struct domain_free_queue *queue) {
    if (is_free_in_progress(queue))
        return;

    mark_free_in_progress(queue, true);
    size_t minimum_to_free = compute_min_elements_to_free(domain, queue);

    flush_free_queue_internal(domain, queue, minimum_to_free);

    mark_free_in_progress(queue, false);
}

void domain_free(paddr_t address, size_t page_count) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    struct domain_buddy *target = domain_buddy_for_addr(address);
    struct domain_free_queue *free_queue = domain_free_queue_on_this_core();

    atomic_fetch_sub(&target->pages_used, page_count);

    domain_stat_free(target);

    if (local == target) {
        free_from_local_domain_buddy(target, address, page_count);
    } else {
        free_from_remote_domain_buddy(target, address, page_count);
    }

    domain_enqueue_flush_worker(&local->worker);
    domain_flush_free_queue(local, free_queue);
}

void domain_flush_thread() {
    struct domain_flush_worker *worker = &domain_buddy_on_this_core()->worker;
    while (!worker->stop) {
        semaphore_wait(&worker->sema);
        struct domain_free_queue *fq = worker->domain->free_queue;
        domain_flush_free_queue(worker->domain, fq);
        atomic_store(&worker->enqueued, false);
    }
}

void domain_enqueue_flush_worker(struct domain_flush_worker *worker) {
    bool already = atomic_exchange(&worker->enqueued, true);
    if (!already)
        semaphore_post(&worker->sema);
}
