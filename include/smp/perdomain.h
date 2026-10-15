/* @title: Per-Domain dynamic objects */
#pragma once
#include <compiler/core.h>
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

LINKER_SECTION_EXTERN(struct perdomain_descriptor, perdomain_desc);

#define PERDOMAIN_DEFINE_3(type_, name_, ctor_)                                \
    static typeof(type_) __perdomain_##name_ cc_unused;                        \
    static struct perdomain_descriptor __perdomain_desc_##name_;               \
    static void __perdomain_ctor_##name_(void *inst, size_t domain) {          \
        void (*const __typed_ctor)(typeof(type_) *, size_t) = (ctor_);         \
        if (__typed_ctor != NULL)                                              \
            __typed_ctor((typeof(type_) *) inst, domain);                      \
        if (domain == global.domain_count - 1)                                 \
            atomic_store(&__perdomain_desc_##name_.ready, true);               \
    }                                                                          \
    static LINKER_SECTION_OBJECT(struct perdomain_descriptor, perdomain_desc)  \
        __perdomain_desc_##name_ = {                                           \
            .name = #name_,                                                    \
            .size = sizeof(typeof(type_)),                                     \
            .align = _Alignof(typeof(type_)),                                  \
            .perdomain_ptrs = NULL,                                            \
            .constructor = __perdomain_ctor_##name_,                           \
            .ready = false,                                                    \
    };                                                                         \
    static struct perdomain_descriptor *const __perdomain_desc_ref_##name_     \
        cc_unused = &__perdomain_desc_##name_

#define PERDOMAIN_DEFINE_2(type_, name_) PERDOMAIN_DEFINE_3(type_, name_, NULL)
#define PERDOMAIN_DEFINE(...) PP_CALL(PERDOMAIN_DEFINE, __VA_ARGS__)

#define PERDOMAIN_EXPORT_AS(sym_name, name)                                    \
    extern struct perdomain_descriptor __perdomain_desc_sym_##sym_name         \
        cc_alias(__perdomain_desc_##name) cc_used

#define PERDOMAIN_EXPORT(name) PERDOMAIN_EXPORT_AS(name, name)

#define PERDOMAIN_EXTERN_AS(type, name, sym_name)                              \
    extern struct perdomain_descriptor __perdomain_desc_sym_##sym_name;        \
    static typeof(type) __perdomain_##name cc_unused;                          \
    static struct perdomain_descriptor *const __perdomain_desc_ref_##name      \
        cc_unused = &__perdomain_desc_sym_##sym_name

#define PERDOMAIN_EXTERN(type, name) PERDOMAIN_EXTERN_AS(type, name, name)

#define PERDOMAIN(name) &(__perdomain_##name)
#define PERDOMAIN_READY(name)                                                  \
    (atomic_load(&(__perdomain_desc_ref_##name)->ready))

#define PERDOMAIN_PTR_FOR(name, d)                                             \
    ({                                                                         \
        (void) debug_enforce(PERDOMAIN_READY(name));                           \
        ((typeof(__perdomain_##name) *) (__perdomain_desc_ref_##name)          \
             ->perdomain_ptrs[d]);                                             \
    })

#define PERDOMAIN_READ_FOR(name, d)                                            \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR_FOR(name, d)))

#define PERDOMAIN_PTR(clr, name) PERDOMAIN_PTR_FOR(name, domain_local_id(clr))
#define PERDOMAIN_READ(clr, name)                                              \
    (*((typeof(__perdomain_##name) *) PERDOMAIN_PTR(clr, name)))

#define PERDOMAIN_WRITE(clr, name, val) (PERDOMAIN_READ(clr, name) = (val))

#define perdomain_for_each_internal_3(var, domain, name)                       \
    for (domain = 0; domain < global.domain_count; domain++)                   \
        for (var = PERDOMAIN_PTR_FOR(name, domain); var != NULL; var = NULL)

#define perdomain_for_each_internal_2(var, name)                               \
    for (domain_id_t __perdomain_idx = 0;                                      \
         __perdomain_idx < global.domain_count; __perdomain_idx++)             \
        for (var = PERDOMAIN_PTR_FOR(name, __perdomain_idx); var != NULL;      \
             var = NULL)

#define perdomain_for_each(...)                                                \
    PP_CALL(perdomain_for_each_internal, __VA_ARGS__)

void perdomain_obj_init(void);
void **perdomain_alloc_array(size_t size);
