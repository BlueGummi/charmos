/* @title: Slab allocator */
#pragma once
#include <compiler/core.h>
#include <linker/symbols.h>
#include <stdbool.h>
#include <stddef.h>
#include <structures/list.h>

struct slab_elcm_candidate {
    size_t pages;
    size_t bitmap_size_bytes;
    size_t obj_count;
};

/* provides the ability for different subsystems to be able to make a constant
 * slab size so frequently allocated objects can waste a little less memory */
struct slab_size_constant {
    const char *name;
    size_t size;
    size_t align;
    struct list_node list;
    struct {
        struct slab_elcm_candidate cand;
    } internal;
};

#define SLAB_SIZE_REGISTER(n, s, a)                                            \
    LINKER_SECTION_OBJECT(struct slab_size_constant, slab_sizes)               \
    slab_size_constant_##n = {                                                 \
        .name = #n,                                                            \
        .size = s,                                                             \
        .align = a,                                                            \
        .list = LIST_NODE_INIT(slab_size_constant_##n.list),                   \
        .internal = {{0}},                                                     \
    }

/* convenience wrapper */
#define SLAB_SIZE_REGISTER_FOR_STRUCT(sname, al)                               \
    SLAB_SIZE_REGISTER(sname, sizeof(struct sname), al)

#define SLAB_OBJ_ALIGN_DEFAULT 8u
#define SLAB_FREE_POISON_BYTE 0x67

void slab_allocator_init();
void slab_domain_init(void);
void slab_domains_print();
void slab_domain_init_late();

LINKER_SECTION_EXTERN(struct slab_size_constant, slab_sizes);
