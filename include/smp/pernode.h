/* @title: Per-Node dynamic objects */
#pragma once
#include <compiler.h>
#include <global.h>
#include <linker/symbols.h>
#include <mem/numa.h>
#include <stddef.h>
#include <stdint.h>

struct pernode_descriptor;
typedef void (*pernode_descriptor_constructor)(void *, size_t);

struct pernode_descriptor {
    const char *name;
    size_t size;
    size_t align;
    void **pernode_ptrs;
    pernode_descriptor_constructor constructor;
    atomic_bool ready;
};

LINKER_SECTION_DEFINE(struct pernode_descriptor, pernode_desc);

#define PERNODE_DECLARE(__n, __type, __ctor)                                   \
    static typeof(__type) __pernode_##__n __unused;                            \
    static struct pernode_descriptor __pernode_desc_##__n;                     \
    static void __pernode_ctor_##__n(void *inst, size_t node) {                \
        void (*const __typed_ctor)(typeof(__type) *, size_t) = (__ctor);       \
        if (__typed_ctor != NULL)                                              \
            __typed_ctor((typeof(__type) *) inst, node);                       \
        if (node == global.node_count - 1)                                     \
            atomic_store(&__pernode_desc_##__n.ready, true);                   \
    }                                                                          \
    static LINKER_SECTION_OBJECT(struct pernode_descriptor, pernode_desc)      \
        __pernode_desc_##__n = {                                               \
            .name = #__n,                                                      \
            .size = sizeof(typeof(__type)),                                    \
            .align = _Alignof(typeof(__type)),                                 \
            .pernode_ptrs = NULL,                                              \
            .constructor = __pernode_ctor_##__n,                               \
            .ready = false,                                                    \
    };                                                                         \
    static struct pernode_descriptor *const __pernode_desc_ref_##__n           \
        __unused = &__pernode_desc_##__n

#define PERNODE_EXPORT_AS(sym_name, name)                                      \
    extern struct pernode_descriptor __pernode_desc_sym_##sym_name             \
        __attribute__((alias("__pernode_desc_" #name), used))

#define PERNODE_EXPORT(name) PERNODE_EXPORT_AS(name, name)

#define PERNODE_DEFINE_AS(name, sym_name, type)                                \
    extern struct pernode_descriptor __pernode_desc_sym_##sym_name;            \
    static typeof(type) __pernode_##name __unused;                             \
    static struct pernode_descriptor *const __pernode_desc_ref_##name          \
        __unused = &__pernode_desc_sym_##sym_name

#define PERNODE_DEFINE(name, type) PERNODE_DEFINE_AS(name, name, type)

void pernode_obj_init(void);

#define PERNODE(name) &(__pernode_##name)
#define PERNODE_READY(name) (atomic_load(&(__pernode_desc_ref_##name)->ready))

#define PERNODE_PTR_FOR_NODE(name, d)                                          \
    ({                                                                         \
        (void) kassert_oops(PERNODE_READY(name));                              \
        ((typeof(__pernode_##name) *) (__pernode_desc_ref_##name)              \
             ->pernode_ptrs[d]);                                               \
    })

#define PERNODE_READ_FOR_NODE(name, d)                                         \
    (*((typeof(__pernode_##name) *) PERNODE_PTR_FOR_NODE(name, d)))

#define PERNODE_PTR(name) PERNODE_PTR_FOR_NODE(name, smp_core()->numa_node)
#define PERNODE_READ(name) (*((typeof(__pernode_##name) *) PERNODE_PTR(name)))

#define PERNODE_WRITE(name, val) (PERNODE_READ(name) = (val))

#define pernode_for_each_internal(name, var, node)                             \
    for (node_id_t node = 0; node < global.node_count; node++)                 \
        for (var = PERNODE_PTR_FOR_NODE(name, node); var != NULL; var = NULL)

#define pernode_for_each_internal_3(name, var, node)                           \
    pernode_for_each_internal(name, var, node)
#define pernode_for_each_internal_2(name, var)                                 \
    pernode_for_each_internal(name, var, __node)

#define pernode_for_each(...)                                                  \
    _DISPATCH(pernode_for_each_internal, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
