#include <limine.h>
#include <stdatomic.h>
#include <stdbool.h>

void wakeup();
void mp_wakeup_processors(struct limine_mp_response *mpr);
void mp_complete_init();
void mp_setup_bsp();
#pragma once
