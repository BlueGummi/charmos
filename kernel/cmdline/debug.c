#include <ndjson.h>

#include "internal.h"

static bool cmdline_test_exit_flag = false;
CMDLINE_DECLARE_VAR(
    cmdline_test_exit, cmdline_test_exit_flag,
    .desc = "Exit QEMU immediately after command-line parsing for unit testing",
    .flags = CMDLINE_ENTRY_HIDDEN);

void cmdline_debug_hook(void) {
#ifdef DEBUG_CMDLINE
    size_t static_count =
        (size_t) (__ekernel_cmdline_entries - __skernel_cmdline_entries);
    size_t schema_count =
        (size_t) (__ekernel_cmdline_schemas - __skernel_cmdline_schemas);

    log_msg(LOG_INFO, "%zu static entries", static_count);
    log_msg(LOG_INFO, "%zu schema namespaces", schema_count);

    for (struct cmdline_schema *s = __skernel_cmdline_schemas;
         s < __ekernel_cmdline_schemas; s++) {
        log_msg(LOG_INFO, "Schema [%s]: %zu properties (hint: %s)", s->prefix,
                s->prop_count, s->path_hint ? s->path_hint : "<path>");
        for (size_t i = 0; i < s->prop_count; i++) {
            const struct cmdline_schema_prop *p = &s->props[i];
            log_msg(LOG_INFO, "  prop: %s (offset: %zu, type: %s)", p->name,
                    p->offset, type_enum_str(p->c_type));
        }
    }
#endif

    if (cmdline_test_exit_flag) {
        log_msg(LOG_INFO, "cmdline_test_exit active: exiting QEMU now");
        ndjson_bye(QEMU_EXIT_OK, "cmdline test exit");
        qemu_exit(QEMU_EXIT_OK);
    }
}
