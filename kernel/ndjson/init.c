#include <cmdline.h>
#include <ndjson.h>
#include <stdint.h>

#include "internal.h"

#define NDJSON_PORT_DEFAULT 0x2F8

static uint64_t ndjson_port = NDJSON_PORT_DEFAULT;
static bool ndjson_enabled = true;
static bool ndjson_schema_dump = true;
static bool ndjson_selftest_enabled = false;

static CMDLINE_DECLARE(ndjson, .flags = CMDLINE_ENTRY_SYMBOLIC,
                       .desc = "NDJSON symbolic parent");

CMDLINE_CHILDREN_DECLARE(
    ndjson,
    CMDLINE_INNER_VAR(enabled, ndjson_enabled, .desc = "Emit NDJSON records"),
    CMDLINE_INNER_VAR(port, ndjson_port, .desc = "NDJSON carrier port",
                      .arg = "<io port>", .range = RANGE(0, UINT16_MAX)),
    CMDLINE_INNER_VAR(schema, ndjson_schema_dump,
                      .desc = "Dump the schema at boot"),
    CMDLINE_INNER_VAR(selftest, ndjson_selftest_enabled,
                      .desc = "Self test at boot"));

/* Early init before cmdline is parsed, so faults still have somewhere to go */
void ndjson_early_init(void) {
    ndjson_carrier_init(NDJSON_PORT_DEFAULT);
}

void ndjson_init(void) {
    if (!ndjson_enabled) {
        ndjson_carrier_disable();
        return;
    }

    if (ndjson_port != NDJSON_PORT_DEFAULT) {
        ndjson_carrier_disable();
        ndjson_carrier_init((uint16_t) ndjson_port);
    }

    ndjson_check_duplicates();

    if (ndjson_schema_dump)
        ndjson_dump_schema();

    if (ndjson_selftest_enabled)
        ndjson_selftest();
}

NDJSON_DECLARE(ndjson_bye, NDJSON_DOMAIN_NDJSON, NDJSON_KIND_BYE, 1,
               NDJSON_U64(code), NDJSON_STR(reason));

/* Signal the end of kernel execution */
void ndjson_bye(uint64_t code, const char *reason) {
    ndjson_emit(ndjson_bye, .code = code, .reason = reason);
}
