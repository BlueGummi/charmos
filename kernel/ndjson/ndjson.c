#include <bootstage.h>
#include <global.h>
#include <ndjson.h>
#include <smp/core.h>
#include <string.h>
#include <time/time.h>

#include "internal.h"

/* first point where GS:base and clock can be read without fault */
#define NDJSON_CONTEXT_STAGE BOOTSTAGE_EARLY_DEVICES

static void ndjson_put(const char *s) {
    ndjson_carrier_write(s, strlen(s));
}

static void ndjson_put_u64(uint64_t v) {
    char buf[20];
    size_t n = 0;

    do {
        buf[n++] = (char) ('0' + (v % 10));
        v /= 10;
    } while (v);

    while (n)
        ndjson_carrier_putc(buf[--n]);
}

static void ndjson_put_i64(int64_t v) {
    if (v < 0) {
        ndjson_carrier_putc('-');
        ndjson_put_u64((uint64_t) -(v + 1) + 1); /* INT64_MIN survives this */
        return;
    }

    ndjson_put_u64((uint64_t) v);
}

static void ndjson_put_hex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    size_t n = 0;

    do {
        buf[n++] = digits[v & 0xf];
        v >>= 4;
    } while (v);

    ndjson_put("\"0x");
    while (n)
        ndjson_carrier_putc(buf[--n]);
    ndjson_carrier_putc('"');
}

static void ndjson_put_escaped_byte(uint8_t c) {
    static const char digits[] = "0123456789abcdef";

    ndjson_put("\\u00");
    ndjson_carrier_putc(digits[c >> 4]);
    ndjson_carrier_putc(digits[c & 0xf]);
}

/* Returns true when a string was cut short, and escapes bytes */
static bool ndjson_put_string(const char *s) {
    if (!s) {
        ndjson_put("null");
        return false;
    }

    ndjson_carrier_putc('"');

    size_t i = 0;
    for (; s[i]; i++) {
        uint8_t c = (uint8_t) s[i];

        if (i == NDJSON_STR_MAX)
            break;

        switch (c) {
        case '"': ndjson_put("\\\""); continue;
        case '\\': ndjson_put("\\\\"); continue;
        case '\n': ndjson_put("\\n"); continue;
        case '\r': ndjson_put("\\r"); continue;
        case '\t': ndjson_put("\\t"); continue;
        case '\b': ndjson_put("\\b"); continue;
        case '\f': ndjson_put("\\f"); continue;
        default: break;
        }

        if (c < 0x20 || c > 0x7e) {
            ndjson_put_escaped_byte(c);
            continue;
        }

        ndjson_carrier_putc((char) c);
    }

    ndjson_carrier_putc('"');
    return s[i] != '\0';
}

/* Separator goes with the key */
static bool ndjson_put_first_key(const char *name) {
    bool trunc = ndjson_put_string(name);
    ndjson_carrier_putc(':');
    return trunc;
}

static bool ndjson_put_key(const char *name) {
    ndjson_carrier_putc(',');
    return ndjson_put_first_key(name);
}

static bool ndjson_put_field(const struct ndjson_field *f, const void *args) {
    const void *p = (const char *) args + f->offset;
    bool trunc = ndjson_put_key(f->name);

    switch (f->type) {
    case NDJSON_TYPE_U64: ndjson_put_u64(*(const uint64_t *) p); break;
    case NDJSON_TYPE_I64: ndjson_put_i64(*(const int64_t *) p); break;
    case NDJSON_TYPE_HEX: ndjson_put_hex(*(const uint64_t *) p); break;
    case NDJSON_TYPE_BOOL:
        ndjson_put(*(const bool *) p ? "true" : "false");
        break;
    case NDJSON_TYPE_STR:
        trunc |= ndjson_put_string(*(const char *const *) p);
        break;
    }

    return trunc;
}

static void ndjson_put_context(void) {
    bool ready = global.current_bootstage >= NDJSON_CONTEXT_STAGE;

    ndjson_put_key(NDJSON_KEY_TIME);
    if (ready)
        ndjson_put_u64(time_get_ms());
    else
        ndjson_put("null");

    ndjson_put_key(NDJSON_KEY_CPU);
    if (ready)
        ndjson_put_u64(smp_core_id());
    else
        ndjson_put("null");
}

void ndjson_emit_impl(const struct ndjson_record *rec, const void *args) {
    bool irqs_were_on;

    if (!ndjson_carrier_begin(&irqs_were_on))
        return;

    bool trunc = false;

    ndjson_carrier_putc('{');
    trunc |= ndjson_put_first_key(NDJSON_KEY_DOMAIN);
    trunc |= ndjson_put_string(rec->domain);

    trunc |= ndjson_put_key(NDJSON_KEY_KIND);
    trunc |= ndjson_put_string(rec->kind);

    trunc |= ndjson_put_key(NDJSON_KEY_VERSION);
    ndjson_put_u64(rec->version);

    ndjson_put_context();

    for (uint16_t i = 0; i < rec->nfields; i++)
        trunc |= ndjson_put_field(&rec->fields[i], args);

    if (trunc) {
        ndjson_put_key(NDJSON_KEY_TRUNCATED);
        ndjson_put("true");
    }

    ndjson_put("}\n");

    ndjson_carrier_end(irqs_were_on);
}
