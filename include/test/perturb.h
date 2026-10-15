/* @title: Test perturbers */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <test/conc.h>
#include <types/types.h>

/* Background noise to create IPIs/DPCs and other stuff */
#define TEST_PERTURB_VICTIM_BIAS 4

/* No victim CPU given */
#define TEST_PERTURB_VICTIM_ANY (-1)

struct test_perturb_env {
    struct test_conc *conc;
    size_t subject_count;
    int64_t victim_cpu; /* TEST_PERTURB_VICTIM_ANY or a CPU id */

    void (*finding)(const char *kind, const char *msg);
};

typedef void (*test_perturb_fn)(const struct test_perturb_env *env,
                                struct test_conc_worker *self);

struct test_perturb_desc {
    const char *name;
    test_perturb_fn fn;
};

/* 0 = default */
struct test_perturb_config {
    const char *name;
    time_ns_t interval_us;
    time_ns_t period_ms;
    time_ns_t gap_ms;
};

const struct test_perturb_desc *test_perturb_lookup(const char *name);

struct test_perturb_config *test_perturb_config_lookup(const char *name);
struct test_perturb_config *test_perturb_config_lookup_n(const char *name,
                                                         size_t len);

void test_perturb_migrator(const struct test_perturb_env *env,
                           struct test_conc_worker *self);
void test_perturb_waker(const struct test_perturb_env *env,
                        struct test_conc_worker *self);
void test_perturb_apc_spammer(const struct test_perturb_env *env,
                              struct test_conc_worker *self);
void test_perturb_alloc_pressure(const struct test_perturb_env *env,
                                 struct test_conc_worker *self);
void test_perturb_inject_armer(const struct test_perturb_env *env,
                               struct test_conc_worker *self);
void test_perturb_ipi_storm(const struct test_perturb_env *env,
                            struct test_conc_worker *self);
void test_perturb_dpc_storm(const struct test_perturb_env *env,
                            struct test_conc_worker *self);
