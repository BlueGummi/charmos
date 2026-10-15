#include <global.h>
#include <inject.h>
#include <irq/ipi.h>
#include <irq/irq.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <string.h>
#include <structures/cpu_mask.h>
#include <test/perturb.h>
#include <thread/apc.h>
#include <thread/dpc.h>
#include <thread/thread.h>
#include <time/time.h>

#if defined(TEST_ENABLED) || defined(TEST_NIGHTMARE_ENABLED)

static struct test_perturb_config perturb_configs[] = {
    {.name = "migrator"},  {.name = "waker"},          {.name = "apc_spammer"},
    {.name = "stutter"},   {.name = "alloc_pressure"}, {.name = "inject_armer"},
    {.name = "ipi_storm"}, {.name = "dpc_storm"},
};

struct test_perturb_config *test_perturb_config_lookup_n(const char *name,
                                                         size_t len) {
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strlen(perturb_configs[i].name) == len &&
            strncmp(perturb_configs[i].name, name, len) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

struct test_perturb_config *test_perturb_config_lookup(const char *name) {
    return test_perturb_config_lookup_n(name, strlen(name));
}

static time_us_t perturb_interval_us(const char *name, time_us_t fallback) {
    const struct test_perturb_config *cfg = test_perturb_config_lookup(name);
    return (cfg && cfg->interval_us) ? NS_TO_US(cfg->interval_us) : fallback;
}

static bool perturb_continue(const struct test_perturb_env *env,
                             struct test_conc_worker *self) {
    while (!test_conc_must_stop(env->conc)) {
        if (!test_conc_must_park(env->conc))
            return true;
        test_conc_park(env->conc, self);
    }
    return false;
}

static struct thread *perturb_pick_subject(const struct test_perturb_env *env,
                                           struct test_conc_worker *self) {
    if (!env->subject_count)
        return NULL;

    size_t idx = test_rng_next(&self->rng) % env->subject_count;
    return atomic_load_acq(&env->conc->workers[idx].th);
}

static void perturb_report(const struct test_perturb_env *env, const char *kind,
                           const char *msg) {
    if (env->finding)
        env->finding(kind, msg);
}

void test_perturb_migrator(const struct test_perturb_env *env,
                           struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("migrator", 200);

    while (perturb_continue(env, self)) {
        struct thread *target = perturb_pick_subject(env, self);
        if (target && global.core_count > 1) {
            uint64_t cpu = test_rng_next(&self->rng) % global.core_count;
            thread_set_migration_target(target, (int64_t) cpu);
        }

        thread_sleep_for_us(interval_us);
    }
}

void test_perturb_waker(const struct test_perturb_env *env,
                        struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("waker", 250);

    while (perturb_continue(env, self)) {
        struct thread *target = perturb_pick_subject(env, self);
        if (target)
            thread_alert(target);

        thread_sleep_for_us(interval_us);
    }
}

static void perturb_apc_probe(void *arg) {
    cc_unused(arg);
}

void test_perturb_apc_spammer(const struct test_perturb_env *env,
                              struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("apc_spammer", 300);

    while (perturb_continue(env, self)) {
        struct thread *target = perturb_pick_subject(env, self);
        if (target && thread_getref(target)) {
            struct apc *apc = apc_create();
            if (apc) {
                apc_init(apc, perturb_apc_probe, NULL, apc_destroy_free);
                apc_enqueue(target, apc, APC_TYPE_KERNEL);
                apc_putref(apc);
            }
            thread_putref(target);
        }

        thread_sleep_for_us(interval_us);
    }
}

#define ALLOC_PRESSURE_SLOTS 64

void test_perturb_alloc_pressure(const struct test_perturb_env *env,
                                 struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("alloc_pressure", 200);

    void *slots[ALLOC_PRESSURE_SLOTS] = {0};
    size_t slot_sizes[ALLOC_PRESSURE_SLOTS] = {0};
    uint32_t slot_patterns[ALLOC_PRESSURE_SLOTS] = {0};

    while (perturb_continue(env, self)) {
        size_t slot = test_rng_next(&self->rng) % ALLOC_PRESSURE_SLOTS;
        if (slots[slot]) {
            uint32_t *p = slots[slot];
            size_t words = slot_sizes[slot] / sizeof(uint32_t);
            uint32_t expected = slot_patterns[slot];
            for (size_t w = 0; w < words; w++) {
                if (p[w] != (expected ^ (uint32_t) w)) {
                    perturb_report(
                        env, "alloc_corruption",
                        "heap memory corruption in alloc_pressure slot");
                    break;
                }
            }
            vfree(slots[slot]);
            slots[slot] = NULL;
            slot_sizes[slot] = 0; // NOLINT(*Malloc)
            slot_patterns[slot] = 0;
        } else {
            size_t size = 16 + (test_rng_next(&self->rng) % 8176);
            uint32_t pat = (uint32_t) test_rng_next(&self->rng);
            void *ptr = valloc(size, ALLOC_ZERO);
            if (ptr) {
                uint32_t *p = ptr;
                size_t words = size / sizeof(uint32_t);
                for (size_t w = 0; w < words; w++)
                    p[w] = pat ^ (uint32_t) w;
                slots[slot] = ptr;
                slot_sizes[slot] = size;
                slot_patterns[slot] = pat;
            }
        }

        thread_sleep_for_us(interval_us);
    }

    for (size_t i = 0; i < ALLOC_PRESSURE_SLOTS; i++) {
        if (slots[i]) {
            vfree(slots[i]);
            slots[i] = NULL;
        }
    }
}

void test_perturb_inject_armer(const struct test_perturb_env *env,
                               struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("inject_armer", 500);

    size_t site_count = __ekernel_inject_sites - __skernel_inject_sites;
    if (site_count == 0) {
        while (perturb_continue(env, self))
            thread_sleep_for_ms(50);
        return;
    }

    while (perturb_continue(env, self)) {
        size_t idx = test_rng_next(&self->rng) % site_count;
        struct inject_site *site = &__skernel_inject_sites[idx];
        uint32_t seed = (uint32_t) test_rng_next(&self->rng);
        uint32_t nth = 1 + (test_rng_next(&self->rng) % 10);

        inject_arm(site, seed, nth);
        thread_sleep_for_us(interval_us);
        inject_disarm(site);
    }
}

enum storm_target : uint8_t {
    STORM_TARGET_VICTIM,
    STORM_TARGET_RANDOM,
    STORM_TARGET_BROADCAST,
};

static cpu_id_t storm_pick_victim(const struct test_perturb_env *env,
                                  struct test_conc_worker *self) {
    if (env->victim_cpu >= 0 && (size_t) env->victim_cpu < global.core_count)
        return (cpu_id_t) env->victim_cpu;

    return (cpu_id_t) (test_rng_next(&self->rng) % global.core_count);
}

static enum storm_target storm_roll(struct test_conc_worker *self,
                                    cpu_id_t victim, cpu_id_t *out_cpu) {
    uint64_t roll = test_rng_next(&self->rng) % (TEST_PERTURB_VICTIM_BIAS + 2);

    if (roll < TEST_PERTURB_VICTIM_BIAS) {
        *out_cpu = victim;
        return STORM_TARGET_VICTIM;
    }

    if (roll == TEST_PERTURB_VICTIM_BIAS) {
        *out_cpu = (cpu_id_t) (test_rng_next(&self->rng) % global.core_count);
        return STORM_TARGET_RANDOM;
    }

    return STORM_TARGET_BROADCAST;
}

/* Bother with reschedule IPIs */
void test_perturb_ipi_storm(const struct test_perturb_env *env,
                            struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("ipi_storm", 100);
    cpu_id_t victim = storm_pick_victim(env, self);

    struct cpu_mask all = CPU_MASK_INIT;
    cpu_mask_set_all(&all);

    while (perturb_continue(env, self)) {
        cpu_id_t cpu;
        if (storm_roll(self, victim, &cpu) == STORM_TARGET_BROADCAST) {
            size_t i;
            for_each_cpu_id(i) {
                scheduler_mark_core_needs_resched(global.cores[i], true);
            }
            ipi_send_mask(&all, IRQ_SCHEDULER, IPI_MASK_SUPERSET_OK);
        } else {
            scheduler_force_resched(global.schedulers[cpu]);
        }

        thread_sleep_for_us(interval_us);
    }
}

/* How long storm DPC stays DISPATCH_LEVEL */
#define DPC_STORM_SPIN 256

#define DPC_STORM_POOL 64

static void dpc_storm_spin(void) {
    for (volatile size_t i = 0; i < DPC_STORM_SPIN; i++)
        cpu_pause();
}

static void dpc_storm_fn(void *a, void *b) {
    cc_unused(a, b);
    dpc_storm_spin();
}

static struct dpc dpc_storm_pool[DPC_STORM_POOL] = {
    [0 ... DPC_STORM_POOL - 1] = {.func = dpc_storm_fn},
};

void test_perturb_dpc_storm(const struct test_perturb_env *env,
                            struct test_conc_worker *self) {
    time_us_t interval_us = perturb_interval_us("dpc_storm", 200);
    cpu_id_t victim = storm_pick_victim(env, self);

    /* dpc_fanout() waits for every CPU, so this can be per-thread */
    struct dpc *fanout = vnews(fanout, global.core_count, ALLOC_ZERO);

    struct cpu_mask all = CPU_MASK_INIT;
    cpu_mask_set_all(&all);

    while (perturb_continue(env, self)) {
        cpu_id_t cpu;
        if (storm_roll(self, victim, &cpu) == STORM_TARGET_BROADCAST) {
            if (fanout)
                dpc_fanout(fanout, all, dpc_storm_spin);
        } else {
            size_t slot = test_rng_next(&self->rng) % DPC_STORM_POOL;
            dpc_enqueue_on_cpu(cpu, &dpc_storm_pool[slot]);
        }

        thread_sleep_for_us(interval_us);
    }

    vfree(fanout);
}

static const struct test_perturb_desc perturb_descs[] = {
    {.name = "migrator", .fn = test_perturb_migrator},
    {.name = "waker", .fn = test_perturb_waker},
    {.name = "apc_spammer", .fn = test_perturb_apc_spammer},
    {.name = "alloc_pressure", .fn = test_perturb_alloc_pressure},
    {.name = "inject_armer", .fn = test_perturb_inject_armer},
    {.name = "ipi_storm", .fn = test_perturb_ipi_storm},
    {.name = "dpc_storm", .fn = test_perturb_dpc_storm},
};

const struct test_perturb_desc *test_perturb_lookup(const char *name) {
    if (!name)
        return NULL;

    for (size_t i = 0; i < sizeof(perturb_descs) / sizeof(perturb_descs[0]);
         i++) {
        if (strcmp(perturb_descs[i].name, name) == 0)
            return &perturb_descs[i];
    }
    return NULL;
}

#endif /* TEST_ENABLED || TEST_NIGHTMARE_ENABLED */
