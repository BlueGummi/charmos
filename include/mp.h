#include <core.h>
#include <stdatomic.h>
#include <stdint.h>
#include <thread.h>

extern struct core **core_data;
extern uint64_t cr3;
extern struct spinlock wakeup_lock;
extern atomic_char cr3_ready;
extern atomic_uint_fast64_t current_cpu;
void wakeup();
uint64_t mp_available_core();
#pragma once
