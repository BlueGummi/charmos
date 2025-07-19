#include <limine.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* TODO: Use a boot_stage rather than these things */
void wakeup();
void mp_wakeup_processors(struct limine_mp_response *mpr);
void mp_complete_init();
void mp_setup_bsp(uint64_t core_count);
extern bool mp_ready;
#pragma once
