/* @title: Per-Domain dynamic objects */
#pragma once
#include <compiler.h>
#include <stddef.h>
#include <stdint.h>

struct perdomain_descriptor;
typedef void (*perdomain_descriptor_constructor)(void *, size_t);

struct perdomain_descriptor {
    const char *name;
    size_t size;
    size_t align;
    void **perdomain_ptrs;
    perdomain_descriptor_constructor constructor;
} __linker_aligned;

extern struct perdomain_descriptor __skernel_perdomain_desc[];
extern struct perdomain_descriptor __ekernel_perdomain_desc[];

#define PERDOMAIN_DECLARE(__n, __type, __ctor)                                 \
    extern __type __perdomain_##__n;                                           \
    static void __perdomain_ctor_##__n(void *inst, size_t cpu) {               \
        if (__ctor)                                                            \
            __ctor((__type *) inst, cpu);                                      \
    }                                                                          \
    static volatile struct perdomain_descriptor __perdomain_desc_##__n         \
        __attribute__((section(".kernel_perdomain_desc"))) = {                 \
            .name = #__n,                                                      \
            .size = sizeof(__type),                                            \
            .align = _Alignof(__type),                                         \
            .perdomain_ptrs = NULL,                                            \
            .constructor = __perdomain_ctor_##__n,                             \
    };                                                                         \
    __type __perdomain_##__n

void perdomain_obj_init(void);

#define PERDOMAIN_PTR_FOR_DOMAIN(name, d)                                      \
    (__perdomain_desc_##name.perdomain_ptrs[d])
#define PERDOMAIN_READ_FOR_DOMAIN(name, d)                                     \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR_FOR_DOMAIN(name, d)))

#define PERDOMAIN_PTR(name) PERDOMAIN_PTR_FOR_DOMAIN(name, domain_local_id())
#define PERDOMAIN_READ(name)                                                   \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR(name)))

#define PERDOMAIN_WRITE(name, val) (PERDOMAIN_READ(name) = (val))
