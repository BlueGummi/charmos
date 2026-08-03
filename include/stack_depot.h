/* @title: Stack Depot */
#pragma once
#include <dbg.h>
#include <math/hash.h>
#include <mem/alloc.h>
#include <structures/list.h>
#include <sync/spinlock.h>
#include <types/refcount.h>

/* This is a single item in the hash table, and the idea here is that
 * when the current stack matches one of these, we return the existing one */
struct stack_depot_record {
    struct list_head hash_list;
    refcount_t refcount;
    size_t num_entries;
    uint32_t hash;

    /* TODO: Different const for this */
    uintptr_t entries[STACK_TRACE_MAX_DEPTH];
};

struct stack_depot_record_chain {
    struct list_head list;
    struct spinlock lock;
};

#define STACK_DEPOT_HASH_SIZE 2048

/* TODO: Someday we'll implement fixed_size_range support for this */
#define STACK_DEPOT_ALLOW_ALLOC_FLAG ALLOC_FLAG_AVAIL_BIT(0)

struct stack_depot_globals {
    uint32_t starting_seed;
    struct stack_depot_record_chain chains[STACK_DEPOT_HASH_SIZE];
    _Atomic size_t num_records;
};

extern struct stack_depot_globals stack_depot_global;

static inline uint32_t stack_depot_hash(uintptr_t *entries,
                                        size_t num_entries) {
    size_t entries_to_trace = num_entries > STACK_TRACE_MAX_DEPTH
                                  ? STACK_TRACE_MAX_DEPTH
                                  : num_entries;

    size_t len = entries_to_trace * 8;

    return hash_murmur3_32(entries, len, stack_depot_global.starting_seed);
}

void stack_depot_init();
stack_handle_t stack_depot_save(uintptr_t *entries, size_t num_entries,
                                enum alloc_flags flags);
struct stack_depot_record *stack_depot_get_record(stack_handle_t key);
size_t stack_depot_read(stack_handle_t key, uintptr_t *entries);
void stack_depot_print(stack_handle_t key);
void stack_depot_put(stack_handle_t key);
stack_handle_t stack_depot_save_current();

static inline size_t stack_depot_get_record_count() {
    return atomic_load(&stack_depot_global.num_records);
}
