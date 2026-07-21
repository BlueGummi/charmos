/* @title: setjmp */
#pragma once
#include <compiler.h>
#include <thread/thread.h>

typedef uint64_t jmp_buf[8];
__naked int setjmp(jmp_buf env);
__naked void longjmp(jmp_buf env, int val);
