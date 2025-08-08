#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/arena.h>
#include <mem/hugepage.h>

static struct arena *init(enum arena_flags flags,
                          enum arena_allocation_type type) {
    struct arena *a = kzalloc(sizeof(struct arena));
    a->flags = flags;
    a->preferred = type;
    a->hugepages = minheap_create();

    /* No need for caller-specified HTB sizes for now */
    if (arena_has_private_htb(a))
        a->tb = hugepage_tb_init(ARENA_HTB_SIZE);

    spinlock_init(&a->lock);
    return a;
}

struct arena *arena_init(enum arena_flags flags,
                         enum arena_allocation_type type) {
    return init(flags, type);
}

struct arena *arena_init_default(void) {
    return init(ARENA_FLAGS_DEFAULT, ARENA_ALLOCATION_TYPE_DEFAULT);
}

struct arena *arena_init_with_limit(enum arena_flags flags,
                                    enum arena_allocation_type type,
                                    size_t hp_limit) {
    struct arena *ret = init(flags, type);
    ret->max_hpages = hp_limit;
    ret->flags |= ARENA_FLAGS_SET_MAX_HUGEPAGES;
    return ret;
}

void arena_insert_hugepage(struct arena *a, struct hugepage *hp) {
    minheap_insert(a->hugepages, &hp->minheap_node, hp->pages_used);
}

struct arena *arena_init_from_hugepage(struct hugepage *hp,
                                       enum arena_flags flags,
                                       enum arena_allocation_type type) {
    struct arena *ret = init(flags, type);
    arena_insert_hugepage(ret, hp);
    return ret;
}

struct arena *
arena_init_from_hugepage_with_limit(struct hugepage *hp, enum arena_flags flags,
                                    enum arena_allocation_type type,
                                    size_t hp_limit) {
    struct arena *ret = arena_init_with_limit(flags, type, hp_limit);
    arena_insert_hugepage(ret, hp);
    return ret;
}
