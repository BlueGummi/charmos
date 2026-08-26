#include "internal.h"

#include <console/printf.h>
#include <nightmare/record.h>
#include <stdarg.h>

#ifdef TEST_NIGHTMARE_ENABLED
static uint64_t fnv1a_bytes(uint64_t hash, const void *data, size_t len) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t fnv1a_string(uint64_t hash, const char *str) {
    return fnv1a_bytes(hash, str ? str : "", strlen(str ? str : ""));
}

void nightmare_finding_at(const struct nightmare_finding_site *site,
                          uint64_t discriminator, const char *fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, (int) sizeof(msg), fmt, args);
    va_end(args);

    uint64_t signature = UINT64_C(14695981039346656037);
    signature = fnv1a_string(signature, nightmare_runtime.ctx.nm
                                            ? nightmare_runtime.ctx.nm->name
                                            : "unknown");
    signature = fnv1a_string(signature, site->kind);
    signature = fnv1a_string(signature, site->file);
    signature = fnv1a_bytes(signature, &site->line, sizeof(site->line));
    signature = fnv1a_bytes(signature, &discriminator, sizeof(discriminator));

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
#endif
