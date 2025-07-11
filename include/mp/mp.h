#include <limine.h>
#include <mp/core.h>
#include <sch/thread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

extern uint64_t cr3;
extern struct spinlock wakeup_lock;
extern atomic_char cr3_ready;
extern bool mp_ready;
extern atomic_uint_fast64_t current_cpu;
void wakeup();
void mp_wakeup_processors(struct limine_mp_response *mpr);
void mp_inform_of_cr3();
extern struct core **global_cores;
#pragma once
