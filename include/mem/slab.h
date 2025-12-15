/* @title: Slab allocator */
#include <compiler.h>
#include <stddef.h>
#pragma once

/* provides the ability for different subsystems to be able to make a constant
 * slab size so frequently allocated objects can waste a little less memory */

struct slab_size_constant {
    const char *name;
    size_t size;
} __linker_aligned;

extern struct slab_size_constant __skernel_slab_sizes[];
extern struct slab_size_constant __ekernel_slab_sizes[];

#define SLAB_SIZE_REGISTER(n, s)                                               \
    static struct slab_size_constant slab_size_constant_##n                    \
        __attribute__((section(".kernel_slab_sizes"), used)) = {               \
            .name = #n,                                                        \
            .size = s,                                                         \
    };

void slab_allocator_init();
void slab_domain_init(void);
void slab_domains_print();
void slab_domain_init_late();
#define SLAB_OBJ_ALIGN 16u
