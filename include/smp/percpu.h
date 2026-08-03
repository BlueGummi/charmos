/* @title: Per-CPU dynamic objects */
#pragma once
#include <compiler.h>
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
    extern __type __percpu_##__n;                                              \
    extern struct percpu_descriptor __percpu_desc_##__n;                       \
    static void __percpu_ctor_##__n(void *inst, size_t cpu) {                  \
        __percpu_desc_##__n.ready = true;                                      \
        if ((__ctor) != NULL)                                                  \
            ((void (*)(__type *, size_t)) __ctor)((__type *) inst, cpu);       \
    }                                                                          \
    LINKER_SECTION_OBJECT(struct percpu_descriptor, percpu_desc)               \
    __percpu_desc_##__n = {                                                    \
        .name = #__n,                                                          \
        .size = sizeof(__type),                                                \
        .align = _Alignof(__type),                                             \
        .percpu_ptrs = NULL,                                                   \
        .constructor = __percpu_ctor_##__n,                                    \
        .ready = false,                                                        \
    };                                                                         \
    __type __percpu_##__n

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
