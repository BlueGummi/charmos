#include <cmdline.h>
#include <nightmare/perturb.h>
#include <string.h>

static struct nightmare_perturb_config perturb_configs[] = {
    {.name = "migrator"},     {.name = "waker"},   {.name = "apc_spammer"},
    {.name = "idle_forcer"},  {.name = "stutter"}, {.name = "alloc_pressure"},
    {.name = "inject_armer"},
};

static void *perturb_resolve(const char *path, size_t path_len) {
    static const char prefix[] = "perturb.";
    if (path_len <= sizeof(prefix) - 1 ||
        strncmp(path, prefix, sizeof(prefix) - 1) != 0)
        return NULL;

    path += sizeof(prefix) - 1;
    path_len -= sizeof(prefix) - 1;
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strlen(perturb_configs[i].name) == path_len &&
            strncmp(perturb_configs[i].name, path, path_len) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

CMDLINE_SCHEMA_DECLARE(
    nightmare_perturb, "nightmare", "perturb.<svc>",
    "Nightmare perturbation options", perturb_resolve,
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, enabled),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, interval_us,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, period_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, gap_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION)),
    CMDLINE_SCHEMA_PROP(struct nightmare_perturb_config, aggression,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_FX),
                        .range = RANGE(0, FX_ONE)));

const struct nightmare_perturb_config *
nightmare_perturb_config_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(perturb_configs) / sizeof(perturb_configs[0]);
         i++) {
        if (strcmp(perturb_configs[i].name, name) == 0)
            return &perturb_configs[i];
    }
    return NULL;
}

const struct nightmare_perturb_desc *
nightmare_perturb_lookup(const char *name) {
    (void) name;
    return NULL;
}
