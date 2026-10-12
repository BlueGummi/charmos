/* @title: Per-CPU dynamic objects */
#pragma once
#include <compiler/core.h>
#include <global.h>
#include <linker/symbols.h>
#include <smp/core.h>
#include <stddef.h>
#include <stdint.h>

struct percpu_descriptor;
typedef void (*percpu_descriptor_constructor)(void *, size_t);

struct percpu_descriptor {
    const char *name;
    size_t size;
    size_t align;
    void **percpu_ptrs;
    percpu_descriptor_constructor constructor;
    atomic_bool ready;
};

LINKER_SECTION_EXTERN(struct percpu_descriptor, percpu_desc);

#define PERCPU_DEFINE_3(type_, name_, ctor_)                                   \
    static typeof(type_) __percpu_##name_ cc_fn_unused;                        \
    static struct percpu_descriptor __percpu_desc_##name_;                     \
    static void __percpu_ctor_##name_(void *inst, size_t cpu) {                \
        void (*const __typed_ctor)(typeof(type_) *, size_t) = (ctor_);         \
        if (__typed_ctor != NULL)                                              \
            __typed_ctor((typeof(type_) *) inst, cpu);                         \
        if (cpu == global.core_count - 1)                                      \
            atomic_store(&__percpu_desc_##name_.ready, true);                  \
    }                                                                          \
    static LINKER_SECTION_OBJECT(struct percpu_descriptor, percpu_desc)        \
        __percpu_desc_##name_ = {                                              \
            .name = #name_,                                                    \
            .size = sizeof(typeof(type_)),                                     \
            .align = _Alignof(typeof(type_)),                                  \
            .percpu_ptrs = NULL,                                               \
            .constructor = __percpu_ctor_##name_,                              \
            .ready = false,                                                    \
    };                                                                         \
    static struct percpu_descriptor *const __percpu_desc_ref_##name_           \
        cc_fn_unused = &__percpu_desc_##name_

#define PERCPU_DEFINE_2(type_, name_) PERCPU_DEFINE_3(type_, name_, NULL)
#define PERCPU_DEFINE(...) PP_CALL(PERCPU_DEFINE, __VA_ARGS__)

#define PERCPU_EXPORT_AS(sym_name, name)                                       \
    extern struct percpu_descriptor __percpu_desc_sym_##sym_name               \
        cc_alias(__percpu_desc_##name) cc_used

#define PERCPU_EXPORT(name) PERCPU_EXPORT_AS(name, name)

#define PERCPU_EXTERN_AS(type, name, sym_name)                                 \
    extern struct percpu_descriptor __percpu_desc_sym_##sym_name;              \
    static typeof(type) __percpu_##name cc_fn_unused;                          \
    static struct percpu_descriptor *const __percpu_desc_ref_##name            \
        cc_fn_unused = &__percpu_desc_sym_##sym_name

#define PERCPU_EXTERN(type, name) PERCPU_EXTERN_AS(type, name, name)

#define PERCPU(name) &(__percpu_##name)
#define PERCPU_READY(name) (atomic_load(&(__percpu_desc_ref_##name)->ready))

#define PERCPU_PTR_FOR(name, cpu)                                              \
    ({                                                                         \
        (void) debug_enforce(PERCPU_READY(name));                              \
        ((typeof(__percpu_##name) *) (__percpu_desc_ref_##name)                \
             ->percpu_ptrs[cpu]);                                              \
    })

#define PERCPU_READ_FOR(name, cpu)                                             \
    (*((typeof(__percpu_##name) *) PERCPU_PTR_FOR(name, cpu)))

#define PERCPU_PTR(clr, name) PERCPU_PTR_FOR(name, smp_id(clr))
#define PERCPU_READ(clr, name)                                                 \
    (*((typeof(__percpu_##name) *) PERCPU_PTR(clr, name)))

#define PERCPU_WRITE(clr, name, val) (PERCPU_READ(clr, name) = (val))

#define percpu_for_each_internal_3(var, cpu, name)                             \
    for (cpu = 0; cpu < global.core_count; cpu++)                              \
        for (var = PERCPU_PTR_FOR(name, cpu); var != NULL; var = NULL)

#define percpu_for_each_internal_2(var, name)                                  \
    for (cpu_id_t __percpu_idx = 0; __percpu_idx < global.core_count;          \
         __percpu_idx++)                                                       \
        for (var = PERCPU_PTR_FOR(name, __percpu_idx); var != NULL; var = NULL)

#define percpu_for_each(...) PP_CALL(percpu_for_each_internal, __VA_ARGS__)

void percpu_obj_init(void);

void **percpu_alloc_array(size_t size);
