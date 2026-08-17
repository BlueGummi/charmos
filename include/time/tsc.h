/* @title: TSC */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

struct clock *tsc_clock_init(freq_hz_t freq_hz);
freq_hz_t tsc_calibrate_hpet(void);
bool tsc_sync_check_bsp(cpu_id_t ap_cpu);
void tsc_sync_check_ap(cpu_id_t self);
void tsc_sync_check_all_aps(void);
void tsc_mailboxes_init(void);
bool tsc_should_use_tsc(void);
