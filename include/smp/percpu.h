/* @title: Per-CPU dynamic objects */
#pragma once
#include <compiler.h>
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
    bool ready;
};

LINKER_SECTION_DEFINE(struct percpu_descriptor, percpu_desc);

#define PERCPU_DECLARE(__n, __type, __ctor)                                    \
    extern typeof(__type) __percpu_##__n;                                      \
    extern struct percpu_descriptor __percpu_desc_##__n;                       \
    static void __percpu_ctor_##__n(void *inst, size_t cpu) {                  \
        __percpu_desc_##__n.ready = true;                                      \
        if ((__ctor) != NULL)                                                  \
            ((void (*)(typeof(__type) *, size_t)) __ctor)(                     \
                (typeof(__type) *) inst, cpu);                                 \
    }                                                                          \
    LINKER_SECTION_OBJECT(struct percpu_descriptor, percpu_desc)               \
    __percpu_desc_##__n = {                                                    \
        .name = #__n,                                                          \
        .size = sizeof(typeof(__type)),                                        \
        .align = _Alignof(typeof(__type)),                                     \
        .percpu_ptrs = NULL,                                                   \
        .constructor = __percpu_ctor_##__n,                                    \
        .ready = false,                                                        \
    };                                                                         \
    typeof(__type) __percpu_##__n

void percpu_obj_init(void);

#define PERCPU(name) &(__percpu_##name)
#define PERCPU_READY(name) __percpu_desc_##name.ready
#define PERCPU_PTR_FOR_CPU(name, cpu)                                          \
    ((typeof(__percpu_##name) *) __percpu_desc_##name.percpu_ptrs[cpu])
#define PERCPU_READ_FOR_CPU(name, cpu)                                         \
    (*((typeof(__percpu_##name) *) PERCPU_PTR_FOR_CPU(name, cpu)))

#define PERCPU_PTR(name) PERCPU_PTR_FOR_CPU(name, smp_core_id())
#define PERCPU_READ(name) (*((typeof(__percpu_##name) *) PERCPU_PTR(name)))

#define PERCPU_WRITE(name, val) (PERCPU_READ(name) = (val))

#define percpu_for_each_internal(name, var, cpu)                               \
    for (cpu_id_t cpu = 0; cpu < global.core_count; cpu++)                     \
        for (var = PERCPU_PTR_FOR_CPU(name, cpu); var != NULL; var = NULL)

#define percpu_for_each_internal_3(name, var, cpu)                             \
    percpu_for_each_internal(name, var, cpu)
#define percpu_for_each_internal_2(name, var)                                  \
    percpu_for_each_internal(name, var, __cpu)

#define percpu_for_each(...)                                                   \
    _DISPATCH(percpu_for_each_internal, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
