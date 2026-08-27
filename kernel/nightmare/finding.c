#include "internal.h"

#include <console/printf.h>
#include <math/hash.h>
#include <nightmare/record.h>
#include <stdarg.h>

#ifdef TEST_NIGHTMARE_ENABLED
static uint64_t fnv1a_string(uint64_t hash, const char *str) {
    const char *value = str ? str : "";
    return hash_fnv1a_64_update(hash, value, strlen(value));
}

void nightmare_finding_at(const struct nightmare_finding_site *site,
                          uint64_t discriminator, const char *fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    uint64_t signature = HASH_FNV1A_64_OFFSET_BASIS;
    signature = fnv1a_string(signature, nightmare_runtime.ctx.nm
                                            ? nightmare_runtime.ctx.nm->name
                                            : "unknown");
    signature = fnv1a_string(signature, site->kind);
    signature = fnv1a_string(signature, site->file);
    signature =
        hash_fnv1a_64_update(signature, &site->line, sizeof(site->line));
    signature =
        hash_fnv1a_64_update(signature, &discriminator, sizeof(discriminator));

    char sig[24];
    char location[192];
    snprintf(sig, (int) sizeof(sig), "%016lx", signature);
    snprintf(location, (int) sizeof(location), "%s:%u", site->file, site->line);

    nightmare_record_finding(&(struct nightmare_finding_record){
        .kind = site->kind,
        .tier =
            site->tier == NIGHTMARE_TIER_CONFIDENT ? "confident" : "ambiguous",
        .sig = sig,
        .site = location,
        .msg = msg,
    });
    atomic_fetch_add_explicit(&nightmare_runtime.finding_count, 1,
                              memory_order_relaxed);
}

void nightmare_request_external_fail(const char *kind, uint64_t discriminator,
                                     const char *fmt, ...) {
    if (!atomic_load_explicit(&nightmare_runtime.active, memory_order_acquire))
        return;

    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    nightmare_finding_at(
        &(const struct nightmare_finding_site){
            .kind = kind,
            .tier = NIGHTMARE_TIER_CONFIDENT,
            .file = __RELFILE__,
            .line = __LINE__,
        },
        discriminator, "%s", msg);

    nightmare_publish_stop(NM_STOP_FAIL);
}
#else
void nightmare_request_external_fail(const char *kind, uint64_t discriminator,
                                     const char *fmt, ...) {
    unused(kind, discriminator, fmt);
}
#endif
