#include <mem/fixed_size_alloc.h>
#include <stack_depot.h>
#include <string.h>

struct stack_depot_globals stack_depot_global = {0};
FIXED_SIZE_RANGE_PERDOMAIN_DECLARE(
    stack_depot, .obj_size = sizeof(struct stack_depot_record),
    .obj_align = _Alignof(struct stack_depot_record));

void stack_depot_init() {
    stack_depot_global.starting_seed = 1234;
    for (int i = 0; i < STACK_DEPOT_HASH_SIZE; i++) {
        INIT_LIST_HEAD(&stack_depot_global.chains[i].list);
        spinlock_init(&stack_depot_global.chains[i].lock);
    }
}

static struct stack_depot_record *
record_chain_get(struct stack_depot_record_chain *this_chain,
                 uintptr_t *entries, size_t num_entries, bool locked) {
    enum irql irql = IRQL_PASSIVE_LEVEL;

    if (!locked)
        irql = spin_lock(&this_chain->lock);

    struct stack_depot_record *pos;
    list_for_each_entry(pos, &this_chain->list, hash_list) {
        if (pos->num_entries == num_entries) {
            if (!memcmp(pos->entries, entries,
                        num_entries * sizeof(uintptr_t))) {
                /* This MUST NOT fail. If it does, that means some invariant
                 * has been broken, because refcount == 0 records exist
                 * outside the hashtable. Basically, on put(), we spin_lock the
                 * record's hash chain, and then if refcount_dec_and_test,
                 * we remove it from the list and free */
                kassert(refcount_inc_not_zero(&pos->refcount));

                if (!locked)
                    spin_unlock(&this_chain->lock, irql);

                return pos;
            }
        }
    }

    if (!locked)
        spin_unlock(&this_chain->lock, irql);

    return NULL;
}

static stack_handle_t record_to_handle(struct stack_depot_record *rec) {
    return rec;
}

static struct stack_depot_record *record_alloc() {
    return FSR_PERDOMAIN_ALLOC(stack_depot);
}

static void record_free(struct stack_depot_record *rec) {
    FSR_PERDOMAIN_FREE(stack_depot, rec);
}

struct stack_depot_record *stack_depot_get_record(stack_handle_t key) {
    return key;
}

stack_handle_t stack_depot_save(uintptr_t *entries, size_t num_entries,
                                enum alloc_flags flags) {
    (void) flags; /* TODO: not handled for now */

    uint32_t hash = stack_depot_hash(entries, num_entries);
    struct stack_depot_record_chain *this_chain =
        &stack_depot_global.chains[hash];

    struct stack_depot_record *rec = NULL;

    if ((rec = record_chain_get(this_chain, entries, num_entries,
                                /*locked=*/false)))
        goto out;

    if (!(rec = record_alloc()))
        goto out;

    enum irql irql = spin_lock(&this_chain->lock);

    struct stack_depot_record *winner =
        record_chain_get(this_chain, entries, num_entries, /*locked=*/true);
    if (winner) {
        spin_unlock(&this_chain->lock, irql);
        record_free(rec);
        rec = winner;
        goto out;
    }

    refcount_init(&rec->refcount, 1);
    rec->hash = hash;
    rec->num_entries = num_entries;
    memcpy(rec->entries, entries, num_entries * sizeof(uintptr_t));
    list_add_tail(&rec->hash_list, &this_chain->list);
    spin_unlock(&this_chain->lock, irql);

out:

    return record_to_handle(rec);
}

size_t stack_depot_read(stack_handle_t key, uintptr_t *entries) {
    struct stack_depot_record *rec = stack_depot_get_record(key);
    if (!rec)
        return 0;

    memcpy(entries, rec->entries, rec->num_entries * sizeof(uintptr_t));
    return rec->num_entries;
}

void stack_depot_print(stack_handle_t key) {
    struct stack_depot_record *rec = stack_depot_get_record(key);
    if (!rec)
        return;

    for (size_t i = 0; i < rec->num_entries; i++) {
        printf("%p ", (void *) rec->entries[i]);
    }
    printf("\n");
}

void stack_depot_put(stack_handle_t key) {
    struct stack_depot_record *rec = key;
    struct stack_depot_record_chain *chain =
        &stack_depot_global.chains[rec->hash];
    bool free_it = false;

    enum irql irql = spin_lock(&chain->lock);

    if (refcount_dec_and_test(&rec->refcount)) {
        list_del(&rec->hash_list);
        free_it = true;
    }

    spin_unlock(&chain->lock, irql);

    if (free_it)
        record_free(rec);
}

stack_handle_t stack_depot_save_current() {
    uintptr_t entries[STACK_TRACE_MAX_DEPTH] = {0};
    size_t num_entries = stack_unwind((uint64_t) __builtin_frame_address(0),
                                      entries, STACK_TRACE_MAX_DEPTH);
    return stack_depot_save(entries, num_entries, ALLOC_FLAGS_DEFAULT);
}
