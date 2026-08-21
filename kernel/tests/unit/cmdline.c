#include "../../cmdline/internal.h"
#include "../test_internal.h"
#include <cmdline.h>

#ifdef TEST_CMDLINE
TEST_GROUP_DECLARE(cmdline);

TEST_DECLARE_UNIT(cmdline_xmacro_descriptors, .group = TEST_GROUP(cmdline)) {
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_BOOL), "bool") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_INT), "int") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_UINT), "uint") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_FX), "fx") == 0);
    TEST_ASSERT(
        strcmp(cmdline_type_to_str(CMDLINE_TYPE_DURATION), "duration") == 0);
    TEST_ASSERT(
        strcmp(cmdline_type_to_str(CMDLINE_TYPE_DATA_SIZE), "data_size") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_RANGE), "range") == 0);
    TEST_ASSERT(
        strcmp(cmdline_type_to_str(CMDLINE_TYPE_CPU_MASK), "cpu_mask") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_MAC), "mac") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_STRING), "string") ==
                0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_LIST), "list") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_ERR), "err") == 0);
    TEST_ASSERT(strcmp(cmdline_type_to_str(CMDLINE_TYPE_NONE), "none") == 0);

    TEST_ASSERT(
        strcmp(cmdline_expr_type_to_str(CMDLINE_TYPE_BOOL), "<on/off>") == 0);
    TEST_ASSERT(
        strcmp(cmdline_expr_type_to_str(CMDLINE_TYPE_DURATION), "<time>") == 0);
    TEST_ASSERT(strcmp(cmdline_expr_type_to_str(CMDLINE_TYPE_DATA_SIZE),
                       "<size>") == 0);
    TEST_ASSERT(strcmp(cmdline_expr_type_to_str(CMDLINE_TYPE_FX), "<float>") ==
                0);

    TEST_ASSERT(strcmp(cmdline_type_raw_hint(CMDLINE_TYPE_BOOL), "on/off") ==
                0);
    TEST_ASSERT(strcmp(cmdline_type_raw_hint(CMDLINE_TYPE_DURATION), "time") ==
                0);
    TEST_ASSERT(strcmp(cmdline_type_raw_hint(CMDLINE_TYPE_DATA_SIZE), "size") ==
                0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cmdline_polymorphic_parsing, .group = TEST_GROUP(cmdline)) {
    uint64_t mask = 0; /* 0 means unconstrained */

    struct cmdline_value vb = cmdline_parse_value_for("true", mask);
    TEST_ASSERT(vb.type == CMDLINE_TYPE_BOOL && vb.b == true);

    struct cmdline_value vu = cmdline_parse_value_for("12345", mask);
    TEST_ASSERT(vu.type == CMDLINE_TYPE_UINT && vu.u64 == 12345);

    struct cmdline_value vi = cmdline_parse_value_for("-999", mask);
    TEST_ASSERT(vi.type == CMDLINE_TYPE_INT && vi.i64 == -999);

    struct cmdline_value vfx = cmdline_parse_value_for("0.75", mask);
    TEST_ASSERT(vfx.type == CMDLINE_TYPE_FX && vfx.fx == FX(0.75));

    struct cmdline_value vdur = cmdline_parse_value_for("250ms", mask);
    TEST_ASSERT(vdur.type == CMDLINE_TYPE_DURATION &&
                vdur.duration == MS_TO_NS(250));

    struct cmdline_value vsz = cmdline_parse_value_for("64MiB", mask);
    TEST_ASSERT(vsz.type == CMDLINE_TYPE_DATA_SIZE &&
                vsz.u64 == 64ULL * 1024 * 1024);

    struct cmdline_value vstr = cmdline_parse_value_for("hello_world", mask);
    TEST_ASSERT(vstr.type == CMDLINE_TYPE_STRING &&
                strcmp((char *) vstr.data, "hello_world") == 0);

    uint64_t bool_only_mask = CMDLINE_TYPES(CMDLINE_TYPE_BOOL);
    struct cmdline_value v_err =
        cmdline_parse_value_for("12345", bool_only_mask);
    TEST_ASSERT(v_err.type == CMDLINE_TYPE_ERR);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cmdline_extraction_helpers, .group = TEST_GROUP(cmdline)) {
    struct cmdline_value vu = {.type = CMDLINE_TYPE_UINT,
                               .u64 = 0x123456789ABCDEF0ULL};
    uint64_t u64_val = 0;
    TEST_ASSERT(cmdline_extract_u64(&vu, &u64_val) == ERR_OK &&
                u64_val == 0x123456789ABCDEF0ULL);

    uint32_t u32_val = 0;
    struct cmdline_value vu32 = {.type = CMDLINE_TYPE_UINT, .u64 = 0x12345678};
    TEST_ASSERT(cmdline_extract_u32(&vu32, &u32_val) == ERR_OK &&
                u32_val == 0x12345678);

    struct cmdline_value vi = {.type = CMDLINE_TYPE_INT, .i64 = -42};
    int64_t i64_val = 0;
    TEST_ASSERT(cmdline_extract_i64(&vi, &i64_val) == ERR_OK && i64_val == -42);

    struct cmdline_value vb = {.type = CMDLINE_TYPE_BOOL, .b = true};
    bool b_val = false;
    TEST_ASSERT(cmdline_extract_bool(&vb, &b_val) == ERR_OK && b_val == true);

    struct cmdline_value vfx = {.type = CMDLINE_TYPE_FX, .fx = FX(0.5)};
    fx32_32_t fx_val = 0;
    TEST_ASSERT(cmdline_extract_fx(&vfx, &fx_val) == ERR_OK &&
                fx_val == FX(0.5));

    struct cmdline_value vdur = {.type = CMDLINE_TYPE_DURATION,
                                 .duration = SECONDS_TO_NS(2)};
    time_ns_t dur_val = 0;
    TEST_ASSERT(cmdline_extract_duration(&vdur, &dur_val) == ERR_OK &&
                dur_val == SECONDS_TO_NS(2));

    const char *str_val = NULL;
    struct cmdline_value vstr = {.type = CMDLINE_TYPE_STRING,
                                 .data = "my_string"};
    TEST_ASSERT(cmdline_extract_const_string(&vstr, &str_val) == ERR_OK &&
                strcmp(str_val, "my_string") == 0);

    uint64_t gen_u64 = 0;
    int64_t gen_i64 = 0;
    bool gen_b = false;
    const char *gen_str = NULL;

    TEST_ASSERT(CMDLINE_EXTRACT(&vu, gen_u64) == ERR_OK &&
                gen_u64 == 0x123456789ABCDEF0ULL);
    TEST_ASSERT(CMDLINE_EXTRACT(&vi, gen_i64) == ERR_OK && gen_i64 == -42);
    TEST_ASSERT(CMDLINE_EXTRACT(&vb, gen_b) == ERR_OK && gen_b == true);
    TEST_ASSERT(CMDLINE_EXTRACT(&vstr, gen_str) == ERR_OK &&
                strcmp(gen_str, "my_string") == 0);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cmdline_choices_and_mappings, .group = TEST_GROUP(cmdline)) {
    const char *const choices[] = CMDLINE_CHOICES("alpha", "beta", "gamma");
    TEST_ASSERT(cmdline_has_choice(choices, "alpha"));
    TEST_ASSERT(cmdline_has_choice(choices, "beta"));
    TEST_ASSERT(cmdline_has_choice(choices, "gamma"));
    TEST_ASSERT(!cmdline_has_choice(choices, "delta"));
    TEST_ASSERT(!cmdline_has_choice(choices, ""));

    const struct cmdline_map *mappings =
        CMDLINE_MAPPINGS(CMDLINE_MAP("fast", 100), CMDLINE_MAP("slow", 10));
    uint64_t map_val = 0;
    TEST_ASSERT(cmdline_lookup_mapping(mappings, "fast", &map_val) &&
                map_val == 100);
    TEST_ASSERT(cmdline_lookup_mapping(mappings, "slow", &map_val) &&
                map_val == 10);
    TEST_ASSERT(!cmdline_lookup_mapping(mappings, "medium", &map_val));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cmdline_flags_table, .group = TEST_GROUP(cmdline)) {
    const struct cmdline_flag *flags =
        CMDLINE_FLAGS({"read", BIT(0)}, {"write", BIT(1)}, {"exec", BIT(2)});
    uint64_t mask = 0;
    const char *err_tok = NULL;
    size_t err_len = 0;

    TEST_ASSERT(
        cmdline_parse_flags(flags, "read,write", &mask, &err_tok, &err_len) &&
        mask == 0x3);
    TEST_ASSERT(cmdline_parse_flags(flags, "exec", &mask, &err_tok, &err_len) &&
                mask == 0x4);
    TEST_ASSERT(cmdline_parse_flags(flags, "read,exec,write", &mask, &err_tok,
                                    &err_len) &&
                mask == 0x7);
    TEST_ASSERT(!cmdline_parse_flags(flags, "read,invalid_flag", &mask,
                                     &err_tok, &err_len));

    return TEST_SUCCESS;
}

CMDLINE_CHILD_DEFINE(test_root, group_opt_in);
CMDLINE_CHILD_DEFINE(watchdog, master, heartbeat_interval);

TEST_DECLARE_UNIT(cmdline_runtime_query, .group = TEST_GROUP(cmdline)) {
    struct cmdline_entry *e_root = cmdline_lookup("root");
    TEST_ASSERT(e_root != NULL);
    TEST_ASSERT(strcmp(e_root->name, "root") == 0);

    struct cmdline_entry *e_none = cmdline_lookup("non_existent_key_123");
    TEST_ASSERT(e_none == NULL);

    uint64_t fallback_u64 = CMDLINE_GET("non_existent_key_123", uint64_t, 9999);
    TEST_ASSERT(fallback_u64 == 9999);

    uint64_t out_val = 0;
    bool found = cmdline_read_or("non_existent_key_123", out_val, 42);
    TEST_ASSERT(!found);
    TEST_ASSERT(out_val == 42);

    bool opt_in = CMDLINE_CHILD_VALUE(test_root, group_opt_in);
    TEST_ASSERT(opt_in == false);

    struct cmdline_entry *e_hb1 =
        CMDLINE_CHILD(watchdog, master, heartbeat_interval);
    struct cmdline_entry *e_hb2 =
        CMDLINE_CHILD(CMDLINE_NODE(watchdog, master), heartbeat_interval);
    TEST_ASSERT(e_hb1 != NULL && e_hb1 == e_hb2);
    TEST_ASSERT(strcmp(e_hb1->name, "heartbeat_interval") == 0);

    /* TODO: Move these two a separate test */
    uint64_t reg = 0;
    reg = BIT_SET_FIELD(reg, 0x5, 4, 7);
    TEST_ASSERT(BIT_GET_FIELD(reg, 4, 7) == 0x5);

    uint64_t full_mask = BIT_MASK(0, 63);
    TEST_ASSERT(full_mask == UINT64_MAX);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cmdline_list_parsing, .group = TEST_GROUP(cmdline)) {
    struct cmdline_value vlist = cmdline_parse_list("10,20,30", 0);
    TEST_ASSERT(vlist.type == CMDLINE_TYPE_LIST);

    struct cmdline_list list = {0};
    TEST_ASSERT(cmdline_extract_list(&vlist, &list) == ERR_OK);
    TEST_ASSERT(list.count == 3);
    TEST_ASSERT(list.items[0].type == CMDLINE_TYPE_UINT &&
                list.items[0].u64 == 10);
    TEST_ASSERT(list.items[1].type == CMDLINE_TYPE_UINT &&
                list.items[1].u64 == 20);
    TEST_ASSERT(list.items[2].type == CMDLINE_TYPE_UINT &&
                list.items[2].u64 == 30);

    return TEST_SUCCESS;
}
#endif
