/* @title: Per-Domain dynamic objects */
#pragma once
#include <compiler.h>
#include <global.h>
#include <linker/symbols.h>
#include <smp/domain.h>
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
    atomic_bool ready;
};

LINKER_SECTION_DEFINE(struct perdomain_descriptor, perdomain_desc);

#define PERDOMAIN_DECLARE(__n, __type, __ctor)                                 \
    extern typeof(__type) __perdomain_##__n;                                   \
    extern struct perdomain_descriptor __perdomain_desc_##__n;                 \
    static void __perdomain_ctor_##__n(void *inst, size_t domain) {            \
        if ((__ctor) != NULL)                                                  \
            ((void (*)(typeof(__type) *, size_t)) __ctor)(                     \
                (typeof(__type) *) inst, domain);                              \
        if (domain == global.domain_count - 1)                                 \
            atomic_store(&__perdomain_desc_##__n.ready, true);                 \
    }                                                                          \
    LINKER_SECTION_OBJECT(struct perdomain_descriptor, perdomain_desc)         \
    __perdomain_desc_##__n = {                                                 \
        .name = #__n,                                                          \
        .size = sizeof(typeof(__type)),                                        \
        .align = _Alignof(typeof(__type)),                                     \
        .perdomain_ptrs = NULL,                                                \
        .constructor = __perdomain_ctor_##__n,                                 \
        .ready = false,                                                        \
    };                                                                         \
    typeof(__type) __perdomain_##__n

void perdomain_obj_init(void);

#define PERDOMAIN(name) &(__perdomain_##name)
#define PERDOMAIN_READY(name) (atomic_load(&__perdomain_desc_##name.ready))
#define PERDOMAIN_PTR_FOR_DOMAIN(name, d)                                      \
    ((typeof(__perdomain_##name) *) __perdomain_desc_##name.perdomain_ptrs[d])
#define PERDOMAIN_READ_FOR_DOMAIN(name, d)                                     \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR_FOR_DOMAIN(name, d)))

#define PERDOMAIN_PTR(name) PERDOMAIN_PTR_FOR_DOMAIN(name, domain_local_id())
#define PERDOMAIN_READ(name)                                                   \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR(name)))

#define PERDOMAIN_WRITE(name, val) (PERDOMAIN_READ(name) = (val))

#define perdomain_for_each_internal(name, var, domain)                         \
    for (domain_id_t domain = 0; domain < global.domain_count; domain++)       \
        for (var = PERDOMAIN_PTR_FOR_DOMAIN(name, domain); var != NULL;        \
             var = NULL)

#define perdomain_for_each_internal_3(name, var, domain)                       \
    perdomain_for_each_internal(name, var, domain)
#define perdomain_for_each_internal_2(name, var)                               \
    perdomain_for_each_internal(name, var, __domain)

#define perdomain_for_each(...)                                                \
    _DISPATCH(perdomain_for_each_internal, PP_NARG(__VA_ARGS__))(__VA_ARGS__)
