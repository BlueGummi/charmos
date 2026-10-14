/* Implements `movealloc`.
 *
 * this feature allows us to take a given virtual pointer, and copy
 * its pages over to pages in the correct domain, and change the
 * page tables to point to these correct pages. this is a form
 * of "mini page migration" that we use to move over initial
 * allocations to the right node. this lives entirely separate
 * from our real page migration */
#pragma once
#include <compiler/core.h>
#include <container_of.h>
#include <linker/symbols.h>
#include <mem/vmm.h>
#include <structures/list.h>

/* this will always panic upon alloc failure - only to be used in init code! */
void movealloc_full(domain_id_t domain, void *ptr, struct vmm_request rq);

#define movealloc(d, p, ...)                                                   \
    movealloc_full((d), (p), vmm_request_with_defaults(__VA_ARGS__))

typedef void (*movealloc_callback)(void *a, void *b);

struct movealloc_callback_node {
    movealloc_callback callback;
    void *a, *b;
    struct list_node list;
};

struct movealloc_callback_chain {
    struct list_head list;
};

#define movealloc_callback_node_from_list_node(ln)                             \
    (container_of(ln, struct movealloc_callback_node, list))

#define MOVEALLOC_REGISTER_CALL(name, callback, param1, param2)                \
    static LINKER_SECTION_OBJECT(struct movealloc_callback_node,               \
                                 movealloc_callbacks)                          \
        movealloc_##name = {callback, param1, param2, .list = {0}};

void movealloc_exec_all(void);
