/* @title: Per-Topology level dynamic objects */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <global.h>
#include <linker/symbols.h>
#include <smp/core.h>
#include <smp/topology.h>
#include <stddef.h>
#include <stdint.h>

/* Core per-topology macros */
struct pertopo_descriptor;
typedef void (*pertopo_descriptor_constructor)(void *, size_t);

struct pertopo_descriptor {
    enum topology_level level;
    const char *name;
    size_t size;
    size_t align;
    void **pertopo_ptrs;
    pertopo_descriptor_constructor constructor;
    atomic_bool ready;
};

LINKER_SECTION_EXTERN(struct pertopo_descriptor, pertopo_desc);

#define PERTOPO_DEFINE_4(type_, name_, level_, ctor_)                          \
    static typeof(type_) __pertopo_##name_ cc_fn_unused;                       \
    static struct pertopo_descriptor __pertopo_desc_##name_;                   \
    static void __pertopo_ctor_##name_(void *inst, size_t node_id) {           \
        void (*const __typed_ctor)(typeof(type_) *, size_t) = (ctor_);         \
        if (__typed_ctor != NULL)                                              \
            __typed_ctor((typeof(type_) *) inst, node_id);                     \
        if (node_id == (size_t) (global.topology.count[level_] - 1))           \
            atomic_store(&__pertopo_desc_##name_.ready, true);                 \
    }                                                                          \
    static LINKER_SECTION_OBJECT(struct pertopo_descriptor, pertopo_desc)      \
        __pertopo_desc_##name_ = {                                             \
            .name = #name_,                                                    \
            .size = sizeof(typeof(type_)),                                     \
            .align = _Alignof(typeof(type_)),                                  \
            .pertopo_ptrs = NULL,                                              \
            .level = level_,                                                   \
            .constructor = __pertopo_ctor_##name_,                             \
            .ready = false,                                                    \
    };                                                                         \
    static struct pertopo_descriptor *const __pertopo_desc_ref_##name_         \
        cc_fn_unused = &__pertopo_desc_##name_

#define PERTOPO_DEFINE_3(type_, name_, level_)                                 \
    PERTOPO_DEFINE_4(type_, name_, level_, NULL)
#define PERTOPO_DEFINE(...) PP_CALL(PERTOPO_DEFINE, __VA_ARGS__)

#define PERTOPO_EXPORT_AS(sym_name, name)                                      \
    extern struct pertopo_descriptor __pertopo_desc_sym_##sym_name             \
        cc_alias(__pertopo_desc_##name) cc_used

#define PERTOPO_EXPORT(name) PERTOPO_EXPORT_AS(name, name)

#define PERTOPO_EXTERN_AS(type, name, sym_name)                                \
    extern struct pertopo_descriptor __pertopo_desc_sym_##sym_name;            \
    static typeof(type) __pertopo_##name cc_fn_unused;                         \
    static struct pertopo_descriptor *const __pertopo_desc_ref_##name          \
        cc_fn_unused = &__pertopo_desc_sym_##sym_name

#define PERTOPO_EXTERN(type, name) PERTOPO_EXTERN_AS(type, name, name)

#define PERTOPO(name) &(__pertopo_##name)
#define PERTOPO_READY(name) (atomic_load(&(__pertopo_desc_ref_##name)->ready))

#define PERTOPO_PTR_FOR(name, node)                                            \
    ({                                                                         \
        (void) debug_enforce(PERTOPO_READY(name));                             \
        ((typeof(__pertopo_##name) *) (__pertopo_desc_ref_##name)              \
             ->pertopo_ptrs[node]);                                            \
    })

#define PERTOPO_READ_FOR(name, node)                                           \
    (*((typeof(__pertopo_##name) *) PERTOPO_PTR_FOR(name, node)))

#define PERTOPO_PTR(clr, name)                                                 \
    PERTOPO_PTR_FOR(name, pertopo_node_local(&__pertopo_desc_##name, clr))
#define PERTOPO_READ(clr, name)                                                \
    (*((typeof(__pertopo_##name) *) PERTOPO_PTR(clr, name)))

#define PERTOPO_WRITE(clr, name, val) (PERTOPO_READ(clr, name) = (val))

#define pertopo_for_each_internal_3(var, node_id, name)                        \
    for (node_id = 0;                                                          \
         node_id < global.topology.count[__pertopo_desc_##name.level];         \
         node_id++)                                                            \
        for (var = PERTOPO_PTR_FOR(name, node_id); var != NULL; var = NULL)

#define pertopo_for_each_internal_2(var, name)                                 \
    for (size_t __pertopo_idx = 0;                                             \
         __pertopo_idx < global.topology.count[__pertopo_desc_##name.level];   \
         __pertopo_idx++)                                                      \
        for (var = PERTOPO_PTR_FOR(name, __pertopo_idx); var != NULL;          \
             var = NULL)

#define pertopo_for_each(...) PP_CALL(pertopo_for_each_internal, __VA_ARGS__)

size_t pertopo_node_local(struct pertopo_descriptor *desc,
                          enum topology_caller c);
void pertopo_obj_init(void);
