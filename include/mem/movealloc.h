/* Implements `movealloc`.
 *
 * this feature allows us to take a given virtual pointer, and copy
 * its pages over to pages in the correct domain, and change the
 * page tables to point to these correct pages. this is a form
 * of "mini page migration" that we use to move over initial
 * allocations to the right node. this lives entirely separate
 * from our real page migration */
#pragma once
#include <containerof.h>
#include <structures/list.h>

/* this will always panic upon alloc failure - only to be used in init code! */
void movealloc(size_t domain, void *ptr);

typedef void (*movealloc_callback)(void *a, void *b);

struct movealloc_callback_node {
    movealloc_callback callback;
    void *a, *b;
    struct list_head list;
} __attribute__((aligned(64)));

struct movealloc_callback_chain {
    struct list_head list;
};

#define movealloc_callback_node_from_list_node(ln)                             \
    (container_of(ln, struct movealloc_callback_node, list))

#define REGISTER_MOVEALLOC_CALLBACK(name, callback, a, b)                      \
    static struct movealloc_callback_node movealloc_##name                     \
        __attribute__((section(".kernel_movealloc_callbacks"), used)) = {      \
            callback, a, b, .list = {0}};

void movealloc_exec_all(void);
