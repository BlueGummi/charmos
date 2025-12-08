/* @title: SMP initialization */
#include <limine.h>
#include <stdatomic.h>
#include <stdbool.h>

void smp_wakeup();
void smp_wakeup_processors(struct limine_mp_response *mpr);
void smp_complete_init();
void smp_setup_bsp();
void smp_wait_for_others_to_idle();
void topology_init(void);
void smp_disable_all_ticks();
void smp_enable_all_ticks();
#pragma once
