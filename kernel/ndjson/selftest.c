#include <ndjson.h>

#include "internal.h"

NDJSON_DECLARE(selftest_types, NDJSON_SECTION_SELFTEST, "types", 1,
               NDJSON_U64(u), NDJSON_I64(i), NDJSON_I64(i_min), NDJSON_BOOL(b),
               NDJSON_STR(s), NDJSON_HEX(h));

NDJSON_DECLARE(selftest_omitted, NDJSON_SECTION_SELFTEST, "omitted", 1,
               NDJSON_U64(u), NDJSON_I64(i), NDJSON_BOOL(b), NDJSON_STR(s),
               NDJSON_HEX(h));

NDJSON_DECLARE(selftest_escapes, NDJSON_SECTION_SELFTEST, "escapes", 1,
               NDJSON_STR(quote), NDJSON_STR(backslash), NDJSON_STR(control),
               NDJSON_STR(ansi), NDJSON_STR(high));

NDJSON_DECLARE(selftest_truncation, NDJSON_SECTION_SELFTEST, "truncation", 1,
               NDJSON_STR(long_string), NDJSON_U64(sent));

static char selftest_long[NDJSON_STR_MAX + 64];

void ndjson_selftest(void) {
    ndjson_emit(selftest_types, .u = UINT64_MAX, .i = -42, .i_min = INT64_MIN,
                .b = true, .s = "hello", .h = 0xffffffff80051de7);

    ndjson_emit(selftest_omitted);

    ndjson_emit(selftest_escapes, .quote = "a \"quoted\" word",
                .backslash = "C:\\path\\to", .control = "line\nbreak\ttab",
                .ansi = "\033[31mred\033[0m", .high = "caf\xc3\xa9");

    for (size_t i = 0; i < sizeof(selftest_long) - 1; i++)
        selftest_long[i] = (char) ('a' + (i % 26));
    selftest_long[sizeof(selftest_long) - 1] = '\0';

    ndjson_emit(selftest_truncation, .long_string = selftest_long,
                .sent = sizeof(selftest_long) - 1);
}
