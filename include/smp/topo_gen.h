/* @title: Topology generation */
#pragma once
#include <smp/pertopo.h>

struct topo_gen_descriptor {
    struct pertopo_descriptor *descs[TOPOLOGY_LEVEL_MAX];
};

#define TOPO_GEN_CTOR(type_, name_, level_, ctor_)                             \
    static inline void __topo_ctor_##level_##_##name_(typeof(type_) *inst,     \
                                                      size_t node_id) {        \
        void (*const __typed_ctor)(typeof(type_) *, enum topology_level,       \
                                   size_t) = (ctor_);                          \
        if (__typed_ctor != NULL)                                              \
            __typed_ctor((typeof(type_) *) inst, level_, node_id);             \
    }

#define TOPO_DEFINE_3(type_, name_, ctor_)                                     \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_SMT, ctor_);                    \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_CORE, ctor_);                   \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_NUMA, ctor_);                   \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_LLC, ctor_);                    \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_PACKAGE, ctor_);                \
    TOPO_GEN_CTOR(type_, name_, TOPOLOGY_LEVEL_MACHINE, ctor_);                \
    PERTOPO_DEFINE(type_, name_##_SMT, TOPOLOGY_LEVEL_SMT,                     \
                   __topo_ctor_TOPOLOGY_LEVEL_SMT_##name_);                    \
    PERTOPO_DEFINE(type_, name_##_CORE, TOPOLOGY_LEVEL_CORE,                   \
                   __topo_ctor_TOPOLOGY_LEVEL_CORE_##name_);                   \
    PERTOPO_DEFINE(type_, name_##_NUMA, TOPOLOGY_LEVEL_NUMA,                   \
                   __topo_ctor_TOPOLOGY_LEVEL_NUMA_##name_);                   \
    PERTOPO_DEFINE(type_, name_##_LLC, TOPOLOGY_LEVEL_LLC,                     \
                   __topo_ctor_TOPOLOGY_LEVEL_LLC_##name_);                    \
    PERTOPO_DEFINE(type_, name_##_PACKAGE, TOPOLOGY_LEVEL_PACKAGE,             \
                   __topo_ctor_TOPOLOGY_LEVEL_PACKAGE_##name_);                \
    PERTOPO_DEFINE(type_, name_##_MACHINE, TOPOLOGY_LEVEL_MACHINE,             \
                   __topo_ctor_TOPOLOGY_LEVEL_MACHINE_##name_);                \
    static typeof(type_) __topo_gen_##name_ cc_unused;                         \
    static struct topo_gen_descriptor __topo_gen_desc_##name_ cc_unused = {    \
        .descs = {                                                             \
            [TOPOLOGY_LEVEL_SMT] = &__pertopo_desc_##name_##_SMT,              \
            [TOPOLOGY_LEVEL_CORE] = &__pertopo_desc_##name_##_CORE,            \
            [TOPOLOGY_LEVEL_NUMA] = &__pertopo_desc_##name_##_NUMA,            \
            [TOPOLOGY_LEVEL_LLC] = &__pertopo_desc_##name_##_LLC,              \
            [TOPOLOGY_LEVEL_PACKAGE] = &__pertopo_desc_##name_##_PACKAGE,      \
            [TOPOLOGY_LEVEL_MACHINE] = &__pertopo_desc_##name_##_MACHINE,      \
        }};

#define TOPO_DEFINE_2(type_, name_) TOPO_DEFINE_3(type_, name_, NULL)
#define TOPO_DEFINE(...) PP_CALL(TOPO_DEFINE, __VA_ARGS__)

#define topo_gen_for_each_internal_2(var, name)                                \
    for (enum topology_level __topo_gen_level = 0;                             \
         __topo_gen_level < TOPOLOGY_LEVEL_MAX; __topo_gen_level++)            \
        for (size_t __topo_gen_idx = 0;                                        \
             __topo_gen_idx < global.topology.count[__topo_gen_level];         \
             __topo_gen_idx++)                                                 \
            for (var = (typeof(var)) __topo_gen_desc_##name                    \
                           .descs[__topo_gen_level]                            \
                           ->pertopo_ptrs[__topo_gen_idx];                     \
                 var != NULL; var = NULL)

#define topo_gen_for_each_internal_3(var, level, name)                         \
    for (level = 0; level < TOPOLOGY_LEVEL_MAX; level++)                       \
        for (size_t __topo_gen_idx = 0;                                        \
             __topo_gen_idx < global.topology.count[level]; __topo_gen_idx++)  \
            for (var = (typeof(var)) __topo_gen_desc_##name.descs[level]       \
                           ->pertopo_ptrs[__topo_gen_idx];                     \
                 var != NULL; var = NULL)

#define topo_gen_for_each_internal_4(var, level, node_id, name)                \
    for (level = 0; level < TOPOLOGY_LEVEL_MAX; level++)                       \
        for (node_id = 0; node_id < global.topology.count[level]; node_id++)   \
            for (var = (typeof(var)) __topo_gen_desc_##name.descs[level]       \
                           ->pertopo_ptrs[node_id];                            \
                 var != NULL; var = NULL)

#define topo_gen_for_each(...) PP_CALL(topo_gen_for_each_internal, __VA_ARGS__)

#define topo_gen_for_each_at_3(var, name, level)                               \
    for (size_t __topo_gen_idx = 0;                                            \
         __topo_gen_idx < global.topology.count[level]; __topo_gen_idx++)      \
        for (var = (typeof(var)) __topo_gen_desc_##name.descs[level]           \
                       ->pertopo_ptrs[__topo_gen_idx];                         \
             var != NULL; var = NULL)

#define topo_gen_for_each_at_4(var, node_id, name, level)                      \
    for (node_id = 0; node_id < global.topology.count[level]; node_id++)       \
        for (var = (typeof(var)) __topo_gen_desc_##name.descs[level]           \
                       ->pertopo_ptrs[node_id];                                \
             var != NULL; var = NULL)

#define topo_gen_for_each_at(...)                                              \
    PP_CALL(topo_gen_for_each_at_internal, __VA_ARGS__)
