/* @title: Nightmare perturbation */
#pragma once
#include <test/perturb.h>

const struct test_perturb_desc *nightmare_perturb_lookup(const char *name);

void nightmare_perturb_stutter(const struct test_perturb_env *env,
                               struct test_conc_worker *worker);
