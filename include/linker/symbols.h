#pragma once

/* defines linker sections for commonly accessed regions,
 * .text, .bss, .rodata, etc... */
#include <compiler.h>
#include <stdint.h>

extern uint64_t __stext, __etext;
extern uint64_t __srodata, __erodata;
extern uint64_t __sdata, __edata;
extern uint64_t __sbss, __ebss;
extern uint64_t __slimine_requests, __elimine_requests;
extern uint64_t __kernel_virt_end;

#define LINKER_SECTION_ATTRIBUTE(n)                                            \
    __attribute__((section(".kernel_" #n), used))

#define LINKER_SECTION_OBJECT(type, section)                                   \
    LINKER_SECTION_ATTRIBUTE(section) type

#define LINKER_SECTION_DEFINE(type, name)                                      \
    extern type __skernel_##name[];                                            \
    extern type __ekernel_##name[];

#define linker_section_for_each_object(obj, name)                              \
    for (obj = __skernel_##name; obj < __ekernel_##name; obj++)
