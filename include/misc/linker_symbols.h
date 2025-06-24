#pragma once
#include <stdint.h>

extern uint64_t __stext, __etext;
extern uint64_t __srodata, __erodata;
extern uint64_t __sdata, __edata;
extern uint64_t __sbss, __ebss;
extern uint64_t __slimine_requests, __elimine_requests;
extern uint64_t __kernel_virt_end;
extern struct kernel_test __skernel_tests;
extern struct kernel_test __ekernel_tests;
