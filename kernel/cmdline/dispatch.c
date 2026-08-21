#include "internal.h"

bool cmdline_has_list_separator(const char *value) {
    bool quoted = false;
    for (const char *p = value; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        else if (*p == ',' && !quoted)
            return true;
    }
    return false;
}

static inline bool cmdline_type_is_heap_allocated(enum cmdline_type t) {
    return t == CMDLINE_TYPE_STRING || t == CMDLINE_TYPE_CPU_MASK ||
           t == CMDLINE_TYPE_RANGE || t == CMDLINE_TYPE_LIST;
}

struct cmdline_value cmdline_parse_list(const char *value, uint64_t accepted) {
    if (accepted == 0)
        accepted = UINT64_MAX;

    uint64_t item_mask = accepted & ~CMDLINE_TYPES(CMDLINE_TYPE_LIST);
    if (item_mask == 0)
        item_mask = UINT64_MAX;

    struct parse_list plist = {0};
    if (!parse_is_list(value, &plist))
        panic("cmdline: invalid list format in '%s'", value);

    struct cmdline_list *list = kmalloc_or_die(sizeof(*list));
    list->count = plist.count;
    list->items = kmalloc_or_die(plist.count * sizeof(*list->items));

    for (size_t i = 0; i < plist.count; i++) {
        list->items[i] = cmdline_parse_value_for(plist.items[i], item_mask);
        if (list->items[i].type == CMDLINE_TYPE_ERR) {
            for (size_t k = 0; k < i; k++) {
                if (cmdline_type_is_heap_allocated(list->items[k].type) &&
                    list->items[k].data)
                    kfree(list->items[k].data);
            }
            kfree(list->items);
            kfree(list);
            parse_list_free(&plist);
            return (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                          .type = CMDLINE_TYPE_ERR};
        }
    }

    parse_list_free(&plist);
    return (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                  .type = CMDLINE_TYPE_LIST,
                                  .data = list};
}

static void *allocate_parsed_value_data(const struct cmdline_type_parser *p,
                                        const char *value,
                                        enum cmdline_type *out_type) {
    if (p->type == CMDLINE_TYPE_STRING) {
        char *str_copy = NULL;
        if (p->parse(&str_copy, value) == ERR_OK)
            return str_copy;
    } else if (p->type == CMDLINE_TYPE_CPU_MASK) {
        struct cpu_mask *mask = kmalloc_or_die(sizeof(*mask));
        if (p->parse(mask, value) == ERR_OK)
            return mask;
        kfree(mask);
    } else if (p->type == CMDLINE_TYPE_RANGE) {
        struct cmdline_range *range = kmalloc_or_die(sizeof(*range));
        if (p->parse(range, value) == ERR_OK)
            return range;
        kfree(range);
    }

    *out_type = CMDLINE_TYPE_ERR;
    return NULL;
}

struct cmdline_value cmdline_parse_value_for(const char *value,
                                             uint64_t accepted) {
    struct cmdline_value val = {
        .mode = CMDLINE_MODE_POLYMORPHIC,
        .type = CMDLINE_TYPE_NONE,
        .u64 = 0,
    };

    if (!value)
        return val;

    if (accepted == 0)
        accepted = UINT64_MAX;

    if ((accepted & CMDLINE_TYPES(CMDLINE_TYPE_LIST)) &&
        cmdline_has_list_separator(value))
        return cmdline_parse_list(value, accepted);

    uint64_t candidate_mask = 0;
    for (size_t i = 0; i < cmdline_parsers_count; i++) {
        const struct cmdline_type_parser *p = &cmdline_parsers[i];
        if (p->detect(value, NULL))
            candidate_mask |= CMDLINE_TYPES(p->type);
    }

    uint64_t valid_mask = candidate_mask & accepted;
    if (valid_mask == 0)
        return (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                      .type = CMDLINE_TYPE_ERR};

    for (size_t i = 0; i < cmdline_parsers_count; i++) {
        const struct cmdline_type_parser *p = &cmdline_parsers[i];
        if (valid_mask & CMDLINE_TYPES(p->type)) {
            val.type = p->type;
            if (!p->uses_allocated_ptr) {
                if (p->parse(&val.u64, value) == ERR_OK)
                    return val;
            } else {
                val.data = allocate_parsed_value_data(p, value, &val.type);
                if (val.type != CMDLINE_TYPE_ERR)
                    return val;
            }
        }
    }

    return (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                  .type = CMDLINE_TYPE_ERR};
}

struct cmdline_value cmdline_parse_value(struct cmdline_entry *ent,
                                         const char *value) {
    return cmdline_parse_value_for(value, cmdline_entry_get_accepted_mask(ent));
}

static bool entry_matches_key(const struct cmdline_entry *e, const char *key) {
    char name[CMDLINE_ENTRY_NAME_LEN_MAX];
    cmdline_functional_name(e, name);
    return strcmp(name, key) == 0;
}

struct cmdline_entry *cmdline_lookup(const char *key) {
    if (!key)
        return NULL;

    for (struct cmdline_entry *ent = __skernel_cmdline_entries;
         ent < __ekernel_cmdline_entries; ent++) {
        if (entry_matches_key(ent, key))
            return ent;
    }

    return NULL;
}

static void cmdline_extract_value_into(void *dst, enum type_enum c_type,
                                       const struct cmdline_value *v) {
    if (!dst || !v)
        return;

    switch (c_type) {
    case TYPE_BOOL:
        *(bool *) dst = (v->type == CMDLINE_TYPE_BOOL) ? v->b : (v->u64 != 0);
        return;
    case TYPE_INT8: *(int8_t *) dst = (int8_t) v->i64; return;
    case TYPE_INT16: *(int16_t *) dst = (int16_t) v->i64; return;
    case TYPE_INT32: *(int32_t *) dst = (int32_t) v->i64; return;
    case TYPE_INT64:
        *(int64_t *) dst =
            (v->type == CMDLINE_TYPE_FX) ? (int64_t) v->fx : v->i64;
        return;
    case TYPE_UINT8: *(uint8_t *) dst = (uint8_t) v->u64; return;
    case TYPE_UINT16: *(uint16_t *) dst = (uint16_t) v->u64; return;
    case TYPE_UINT32: *(uint32_t *) dst = (uint32_t) v->u64; return;
    case TYPE_UINT64:
        *(uint64_t *) dst =
            (v->type == CMDLINE_TYPE_DURATION) ? v->duration : v->u64;
        return;
    case TYPE_POINTER:
        if (v->type == CMDLINE_TYPE_STRING)
            *(const char **) dst = (const char *) v->data;
        else if (v->type == CMDLINE_TYPE_LIST)
            *(const struct cmdline_list **) dst =
                (const struct cmdline_list *) v->data;
        else
            *(void **) dst = v->data;
        return;
    default:
        if (v->type == CMDLINE_TYPE_RANGE) {
            *(struct cmdline_range *) dst =
                *(const struct cmdline_range *) v->data;
        } else if (v->type == CMDLINE_TYPE_CPU_MASK) {
            memcpy(dst, v->data, sizeof(struct cpu_mask));
        } else {
            *(uint64_t *) dst = v->u64;
        }
        return;
    }
}

void dispatch_parse_value(struct cmdline_entry *e, const char *name,
                          const char *var, const char *val) {
    if (e->mappings) {
        uint64_t mapped_val = 0;
        if (!cmdline_lookup_mapping(e->mappings, val, &mapped_val))
            panic("cmdline entry '%s' received invalid mapping choice '%s'",
                  name, val);
        if (cmdline_value_is_typed(&e->value)) {
            cmdline_write_typed_uint(&e->value, mapped_val);
        } else {
            e->value = (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                              .type = CMDLINE_TYPE_UINT,
                                              .u64 = mapped_val};
        }
        return;
    }

    if (e->flags_table) {
        uint64_t accumulated_mask = 0;
        const char *err_tok = NULL;
        size_t err_len = 0;
        if (!cmdline_parse_flags(e->flags_table, val, &accumulated_mask,
                                 &err_tok, &err_len))
            panic("cmdline entry '%s' received invalid flag '%.*s'", name,
                  (int) err_len, err_tok);
        if (cmdline_value_is_typed(&e->value)) {
            cmdline_write_typed_uint(&e->value, accumulated_mask);
        } else {
            e->value = (struct cmdline_value){.mode = CMDLINE_MODE_POLYMORPHIC,
                                              .type = CMDLINE_TYPE_UINT,
                                              .u64 = accumulated_mask};
        }
        return;
    }

    if (cmdline_value_is_typed(&e->value)) {
        if (e->value.parse) {
            enum errno err = e->value.parse(e->value.write_to, val);
            if (err != ERR_OK)
                panic("cmdline entry '%s' failed to parse value '%s' (err: %d)",
                      name, val, err);
            return;
        }

        struct cmdline_value vtmp =
            cmdline_parse_value_for(val, cmdline_entry_get_accepted_mask(e));
        if (vtmp.type == CMDLINE_TYPE_ERR)
            panic("cmdline variable %s received incompatible value '%s'", var,
                  val);
        if (e->value.write_to)
            cmdline_extract_value_into(e->value.write_to, e->value.c_type,
                                       &vtmp);
        return;
    }

    struct cmdline_value vtmp =
        cmdline_parse_value_for(val, cmdline_entry_get_accepted_mask(e));
    if (vtmp.type == CMDLINE_TYPE_ERR)
        panic("cmdline variable %s received incompatible value '%s'", var, val);
    e->value = vtmp;
}

static void validate_scalar_range(enum type_enum c_type, const void *ptr,
                                  uint64_t low, uint64_t hi,
                                  const char *var_name, const char *val) {
    switch (c_type) {
    case TYPE_INT8: {
        int64_t v = *(const int8_t *) ptr;
        if (v < (int64_t) low || v > (int64_t) hi)
            panic("cmdline entry '%s' integer '%s' out of range", var_name,
                  val);
        break;
    }
    case TYPE_INT16: {
        int64_t v = *(const int16_t *) ptr;
        if (v < (int64_t) low || v > (int64_t) hi)
            panic("cmdline entry '%s' integer '%s' out of range", var_name,
                  val);
        break;
    }
    case TYPE_INT32: {
        int64_t v = *(const int32_t *) ptr;
        if (v < (int64_t) low || v > (int64_t) hi)
            panic("cmdline entry '%s' integer '%s' out of range", var_name,
                  val);
        break;
    }
    case TYPE_INT64: {
        int64_t v = *(const int64_t *) ptr;
        if (v < (int64_t) low || v > (int64_t) hi)
            panic("cmdline entry '%s' integer '%s' out of range", var_name,
                  val);
        break;
    }
    case TYPE_UINT8: {
        uint64_t v = *(const uint8_t *) ptr;
        if (v < low || v > hi)
            panic("cmdline entry '%s' value '%s' out of range", var_name, val);
        break;
    }
    case TYPE_UINT16: {
        uint64_t v = *(const uint16_t *) ptr;
        if (v < low || v > hi)
            panic("cmdline entry '%s' value '%s' out of range", var_name, val);
        break;
    }
    case TYPE_UINT32: {
        uint64_t v = *(const uint32_t *) ptr;
        if (v < low || v > hi)
            panic("cmdline entry '%s' value '%s' out of range", var_name, val);
        break;
    }
    case TYPE_UINT64: {
        uint64_t v = *(const uint64_t *) ptr;
        if (v < low || v > hi)
            panic("cmdline entry '%s' value '%s' out of range", var_name, val);
        break;
    }
    default: break;
    }
}

static void dispatch_validate_range(const struct cmdline_entry *e,
                                    const char *name, const char *val) {
    if (!cmdline_entry_has_range(e))
        return;

    if (e->value.mode == CMDLINE_MODE_POLYMORPHIC) {
        if (e->value.type == CMDLINE_TYPE_INT) {
            if (e->value.i64 < (int64_t) e->range.low ||
                e->value.i64 > (int64_t) e->range.hi)
                panic("cmdline entry '%s' integer '%s' out of range", name,
                      val);
        } else if (e->value.type == CMDLINE_TYPE_UINT ||
                   e->value.type == CMDLINE_TYPE_DATA_SIZE) {
            if (!RANGE_CONTAINS(e->range, e->value.u64))
                panic("cmdline entry '%s' value '%s' out of range", name, val);
        } else if (e->value.type == CMDLINE_TYPE_DURATION) {
            if (!RANGE_CONTAINS(e->range, e->value.duration))
                panic("cmdline entry '%s' duration '%s' out of range", name,
                      val);
        } else if (e->value.type == CMDLINE_TYPE_FX) {
            if (e->value.fx < (fx32_32_t) e->range.low ||
                e->value.fx > (fx32_32_t) e->range.hi)
                panic("cmdline entry '%s' value '%s' out of range", name, val);
        }
        return;
    }

    if (cmdline_value_is_typed(&e->value) && e->value.write_to) {
        if ((e->types & (1ULL << CMDLINE_TYPE_FX)) != 0) {
            fx32_32_t v = *(const fx32_32_t *) e->value.write_to;
            if (v < (fx32_32_t) e->range.low || v > (fx32_32_t) e->range.hi)
                panic("cmdline entry '%s' value '%s' out of range", name, val);
            return;
        }
        if ((e->types & (1ULL << CMDLINE_TYPE_DURATION)) != 0) {
            uint64_t v = *(const uint64_t *) e->value.write_to;
            if (!RANGE_CONTAINS(e->range, v))
                panic("cmdline entry '%s' duration '%s' out of range", name,
                      val);
            return;
        }
        validate_scalar_range(e->value.c_type, e->value.write_to, e->range.low,
                              e->range.hi, name, val);
    }
}

static void dispatch_validate_choices(const struct cmdline_entry *e,
                                      const char *name, const char *val) {
    if (e->choices && !cmdline_has_choice(e->choices, val))
        panic("cmdline entry '%s' received invalid choice '%s'", name, val);
}

static void schema_validate_range(const struct cmdline_schema_prop *p,
                                  const char *var, const char *val,
                                  const void *ptr) {
    if (!RANGE_VALID(p->range))
        return; /* Uninitialized / sentinel range */

    if ((p->types & (1ULL << CMDLINE_TYPE_FX)) != 0 ||
        p->parse == cmdline_parse_fx) {
        fx32_32_t v = *(const fx32_32_t *) ptr;
        if (v < (fx32_32_t) p->range.low || v > (fx32_32_t) p->range.hi)
            panic("cmdline entry '%s' value '%s' out of range", var, val);
        return;
    }

    if ((p->types & (1ULL << CMDLINE_TYPE_DURATION)) != 0 ||
        p->parse == cmdline_parse_duration) {
        time_ns_t v = *(const time_ns_t *) ptr;
        if (!RANGE_CONTAINS(p->range, v))
            panic("cmdline entry '%s' duration '%s' out of range", var, val);
        return;
    }

    validate_scalar_range(p->c_type, ptr, p->range.low, p->range.hi, var, val);
}

static bool schema_dispatch(const char *var, const char *val) {
    for (struct cmdline_schema *s = __skernel_cmdline_schemas;
         s < __ekernel_cmdline_schemas; s++) {
        size_t prefix_len = strlen(s->prefix);
        if (strncmp(var, s->prefix, prefix_len) != 0 || var[prefix_len] != '.')
            continue;

        const char *subpath = var + prefix_len + 1;
        const char *last_dot = strrchr(subpath, '.');
        if (!last_dot)
            continue;

        size_t instance_path_len = (size_t) (last_dot - subpath);
        const char *prop_name = last_dot + 1;

        const struct cmdline_schema_prop *matched_prop = NULL;
        for (size_t i = 0; i < s->prop_count; i++) {
            if (strcmp(s->props[i].name, prop_name) == 0) {
                matched_prop = &s->props[i];
                break;
            }
        }
        if (!matched_prop)
            continue;

        void *instance =
            s->resolve ? s->resolve(subpath, instance_path_len) : NULL;
        if (!instance)
            panic("cmdline schema '%s': unknown instance '%.*s'", s->prefix,
                  (int) instance_path_len, subpath);

        void *target_ptr = ((uint8_t *) instance) + matched_prop->offset;

        if (matched_prop->mappings) {
            uint64_t mval = 0;
            if (!cmdline_lookup_mapping(matched_prop->mappings, val, &mval))
                panic("cmdline '%s': invalid mapping '%s'", var, val);
            struct cmdline_value tmp_val = {
                .mode = CMDLINE_MODE_TYPED,
                .c_type = matched_prop->c_type,
                .write_to = target_ptr,
            };
            cmdline_write_typed_uint(&tmp_val, mval);
        } else if (matched_prop->flags_table) {
            uint64_t mask = 0;
            const char *err_tok = NULL;
            size_t err_len = 0;
            if (!cmdline_parse_flags(matched_prop->flags_table, val, &mask,
                                     &err_tok, &err_len))
                panic("cmdline '%s': invalid flag '%.*s'", var, (int) err_len,
                      err_tok);
            struct cmdline_value tmp_val = {
                .mode = CMDLINE_MODE_TYPED,
                .c_type = matched_prop->c_type,
                .write_to = target_ptr,
            };
            cmdline_write_typed_uint(&tmp_val, mask);
        } else if (matched_prop->parse) {
            enum errno err = matched_prop->parse(target_ptr, val);
            if (err != ERR_OK)
                panic("cmdline '%s': parse failed for '%s' (err %d)", var, val,
                      err);
        } else if (matched_prop->types != 0 ||
                   matched_prop->c_type != TYPE_NONE) {
            uint64_t mask = UINT64_MAX;
            if (matched_prop->types >= (1ULL << CMDLINE_TYPE_OFFSET)) {
                mask = matched_prop->types;
            } else if (matched_prop->types >= CMDLINE_TYPE_OFFSET &&
                       matched_prop->types < CMDLINE_TYPE_NONE) {
                mask = (1ULL << matched_prop->types);
            } else if (matched_prop->c_type != TYPE_NONE) {
                switch (matched_prop->c_type) {
                case TYPE_BOOL: mask = (1ULL << CMDLINE_TYPE_BOOL); break;
                case TYPE_INT8:
                case TYPE_INT16:
                case TYPE_INT32:
                case TYPE_INT64:
                    mask = (1ULL << CMDLINE_TYPE_INT) |
                           (1ULL << CMDLINE_TYPE_UINT);
                    break;
                case TYPE_UINT8:
                case TYPE_UINT16:
                case TYPE_UINT32:
                case TYPE_UINT64: mask = (1ULL << CMDLINE_TYPE_UINT); break;
                case TYPE_POINTER: mask = (1ULL << CMDLINE_TYPE_STRING); break;
                default: break;
                }
            }
            struct cmdline_value vtmp = cmdline_parse_value_for(val, mask);
            if (vtmp.type == CMDLINE_TYPE_ERR)
                panic("cmdline '%s': parse failed for '%s'", var, val);
            cmdline_extract_value_into(target_ptr, matched_prop->c_type, &vtmp);
        } else {
            panic("cmdline schema '%s': property '%s' has no parser configured",
                  s->prefix, prop_name);
        }

        if (matched_prop->choices &&
            !cmdline_has_choice(matched_prop->choices, val))
            panic("cmdline entry '%s' received invalid choice '%s'", var, val);

        schema_validate_range(matched_prop, var, val, target_ptr);
        log_msg(LOG_INFO, "command line entry '%s' set to '%s'", var, val);
        return true;
    }
    return false;
}

void cmdline_dispatch(const char *var, const char *val) {
    for (struct cmdline_entry *e = __skernel_cmdline_entries;
         e < __ekernel_cmdline_entries; e++) {
        kassert(e->name);

        if (e->flags & CMDLINE_ENTRY_SYMBOLIC)
            continue;

        if (!entry_matches_key(e, var))
            continue;

        if (e->status == CMDLINE_ENTRY_FOUND)
            panic("duplicate cmdline entry: %s", var);

        e->status = CMDLINE_ENTRY_FOUND;

        char name[CMDLINE_ENTRY_NAME_LEN_MAX];
        cmdline_functional_name(e, name);

        dispatch_parse_value(e, name, var, val);
        dispatch_validate_range(e, name, val);
        dispatch_validate_choices(e, name, val);
        log_msg(LOG_INFO, "command line entry '%s' set to '%s'", name, val);
        return;
    }

    if (schema_dispatch(var, val))
        return;

    panic("unknown command line key '%s'", var);
}
