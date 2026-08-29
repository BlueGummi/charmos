#include "internal.h"

enum errno cmdline_extract_bool(struct cmdline_value *val, bool *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->mode == CMDLINE_MODE_TYPED && val->write_to &&
        val->c_type == TYPE_BOOL) {
        *out = *(bool *) val->write_to;
        return ERR_OK;
    }

    if (val->type == CMDLINE_TYPE_BOOL) {
        *out = val->b;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_u64(struct cmdline_value *val, uint64_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->mode == CMDLINE_MODE_TYPED && val->write_to) {
        switch (val->c_type) {
        case TYPE_UINT64: *out = *(uint64_t *) val->write_to; return ERR_OK;
        case TYPE_INT64: {
            int64_t v = *(int64_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint64_t) v;
            return ERR_OK;
        }
        case TYPE_UINT32: *out = *(uint32_t *) val->write_to; return ERR_OK;
        case TYPE_INT32: {
            int32_t v = *(int32_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint64_t) v;
            return ERR_OK;
        }
        case TYPE_UINT16: *out = *(uint16_t *) val->write_to; return ERR_OK;
        case TYPE_INT16: {
            int16_t v = *(int16_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint64_t) v;
            return ERR_OK;
        }
        case TYPE_UINT8: *out = *(uint8_t *) val->write_to; return ERR_OK;
        case TYPE_INT8: {
            int8_t v = *(int8_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint64_t) v;
            return ERR_OK;
        }
        default: return ERR_INVAL;
        }
    }

    if (val->type == CMDLINE_TYPE_INT) {
        if (val->i64 < 0)
            return ERR_OVERFLOW;
        *out = (uint64_t) val->i64;
        return ERR_OK;
    }

    if (val->type == CMDLINE_TYPE_UINT || val->type == CMDLINE_TYPE_DATA_SIZE ||
        val->type == CMDLINE_TYPE_DURATION || val->type == CMDLINE_TYPE_MAC) {
        *out = val->u64;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_i64(struct cmdline_value *val, int64_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->mode == CMDLINE_MODE_TYPED && val->write_to) {
        switch (val->c_type) {
        case TYPE_INT64: *out = *(int64_t *) val->write_to; return ERR_OK;
        case TYPE_UINT64: {
            uint64_t v = *(uint64_t *) val->write_to;
            if (v > (uint64_t) INT64_MAX)
                return ERR_OVERFLOW;
            *out = (int64_t) v;
            return ERR_OK;
        }
        case TYPE_INT32: *out = *(int32_t *) val->write_to; return ERR_OK;
        case TYPE_UINT32: *out = *(uint32_t *) val->write_to; return ERR_OK;
        case TYPE_INT16: *out = *(int16_t *) val->write_to; return ERR_OK;
        case TYPE_UINT16: *out = *(uint16_t *) val->write_to; return ERR_OK;
        case TYPE_INT8: *out = *(int8_t *) val->write_to; return ERR_OK;
        case TYPE_UINT8: *out = *(uint8_t *) val->write_to; return ERR_OK;
        default: return ERR_INVAL;
        }
    }

    if (val->type == CMDLINE_TYPE_INT) {
        *out = val->i64;
        return ERR_OK;
    }

    if (val->type == CMDLINE_TYPE_UINT || val->type == CMDLINE_TYPE_DATA_SIZE ||
        val->type == CMDLINE_TYPE_DURATION) {
        if (val->u64 > (uint64_t) INT64_MAX)
            return ERR_OVERFLOW;
        *out = (int64_t) val->u64;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_u32(struct cmdline_value *val, uint32_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->mode == CMDLINE_MODE_TYPED && val->write_to) {
        switch (val->c_type) {
        case TYPE_UINT32: *out = *(uint32_t *) val->write_to; return ERR_OK;
        case TYPE_INT32: {
            int32_t v = *(int32_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
            return ERR_OK;
        }
        case TYPE_UINT64: {
            uint64_t v = *(uint64_t *) val->write_to;
            if (v > UINT32_MAX)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
            return ERR_OK;
        }
        case TYPE_INT64: {
            int64_t v = *(int64_t *) val->write_to;
            if (v < 0 || (uint64_t) v > UINT32_MAX)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
            return ERR_OK;
        }
        case TYPE_UINT16: *out = *(uint16_t *) val->write_to; return ERR_OK;
        case TYPE_INT16: {
            int16_t v = *(int16_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
            return ERR_OK;
        }
        case TYPE_UINT8: *out = *(uint8_t *) val->write_to; return ERR_OK;
        case TYPE_INT8: {
            int8_t v = *(int8_t *) val->write_to;
            if (v < 0)
                return ERR_OVERFLOW;
            *out = (uint32_t) v;
            return ERR_OK;
        }
        default: return ERR_INVAL;
        }
    }

    if (val->type == CMDLINE_TYPE_UINT || val->type == CMDLINE_TYPE_DATA_SIZE ||
        val->type == CMDLINE_TYPE_DURATION) {
        uint64_t v = val->u64;
        if (v > UINT32_MAX)
            return ERR_OVERFLOW;
        *out = (uint32_t) v;
        return ERR_OK;
    }

    if (val->type == CMDLINE_TYPE_INT) {
        int64_t v = val->i64;
        if (v < 0 || (uint64_t) v > UINT32_MAX)
            return ERR_OVERFLOW;
        *out = (uint32_t) v;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_i32(struct cmdline_value *val, int32_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->mode == CMDLINE_MODE_TYPED && val->write_to) {
        switch (val->c_type) {
        case TYPE_INT32: *out = *(int32_t *) val->write_to; return ERR_OK;
        case TYPE_UINT32: {
            uint32_t v = *(uint32_t *) val->write_to;
            if (v > (uint32_t) INT32_MAX)
                return ERR_OVERFLOW;
            *out = (int32_t) v;
            return ERR_OK;
        }
        case TYPE_INT64: {
            int64_t v = *(int64_t *) val->write_to;
            if (v < INT32_MIN || v > INT32_MAX)
                return ERR_OVERFLOW;
            *out = (int32_t) v;
            return ERR_OK;
        }
        case TYPE_UINT64: {
            uint64_t v = *(uint64_t *) val->write_to;
            if (v > (uint64_t) INT32_MAX)
                return ERR_OVERFLOW;
            *out = (int32_t) v;
            return ERR_OK;
        }
        case TYPE_INT16: *out = *(int16_t *) val->write_to; return ERR_OK;
        case TYPE_UINT16: *out = *(uint16_t *) val->write_to; return ERR_OK;
        case TYPE_INT8: *out = *(int8_t *) val->write_to; return ERR_OK;
        case TYPE_UINT8: *out = *(uint8_t *) val->write_to; return ERR_OK;
        default: return ERR_INVAL;
        }
    }

    if (val->type == CMDLINE_TYPE_INT) {
        int64_t v = val->i64;
        if (v < INT32_MIN || v > INT32_MAX)
            return ERR_OVERFLOW;
        *out = (int32_t) v;
        return ERR_OK;
    }

    if (val->type == CMDLINE_TYPE_UINT || val->type == CMDLINE_TYPE_DATA_SIZE ||
        val->type == CMDLINE_TYPE_DURATION) {
        uint64_t v = val->u64;
        if (v > (uint64_t) INT32_MAX)
            return ERR_OVERFLOW;
        *out = (int32_t) v;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_fx(struct cmdline_value *val, fx32_32_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_FX) {
        *out = val->fx;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_duration(struct cmdline_value *val, time_ns_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_DURATION) {
        *out = val->duration;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_mac(struct cmdline_value *val, uint64_t *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_MAC) {
        *out = val->u64;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_range(struct cmdline_value *val,
                                 struct cmdline_range *out) {
    if (!val || !out || val->type != CMDLINE_TYPE_RANGE || !val->data)
        return ERR_INVAL;
    *out = *(struct cmdline_range *) val->data;
    return ERR_OK;
}

enum errno cmdline_extract_cpu_mask(struct cmdline_value *val,
                                    struct cpu_mask *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_CPU_MASK && val->data) {
        *out = *(struct cpu_mask *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_string(struct cmdline_value *val, char **out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_STRING && val->data) {
        *out = (char *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_const_string(struct cmdline_value *val,
                                        const char **out) {
    return cmdline_extract_string(val, (char **) out);
}

enum errno cmdline_extract_list(struct cmdline_value *val,
                                struct cmdline_list *out) {
    if (!val || !out)
        return ERR_INVAL;

    if (val->type == CMDLINE_TYPE_LIST && val->data) {
        *out = *(struct cmdline_list *) val->data;
        return ERR_OK;
    }

    return ERR_INVAL;
}

enum errno cmdline_extract_u16(struct cmdline_value *val, uint16_t *out) {
    if (!val || !out)
        return ERR_INVAL;
    uint32_t v = 0;
    enum errno err = cmdline_extract_u32(val, &v);
    if (err != ERR_OK)
        return err;
    if (v > UINT16_MAX)
        return ERR_OVERFLOW;
    *out = (uint16_t) v;
    return ERR_OK;
}

enum errno cmdline_extract_i16(struct cmdline_value *val, int16_t *out) {
    if (!val || !out)
        return ERR_INVAL;
    int32_t v = 0;
    enum errno err = cmdline_extract_i32(val, &v);
    if (err != ERR_OK)
        return err;
    if (v < INT16_MIN || v > INT16_MAX)
        return ERR_OVERFLOW;
    *out = (int16_t) v;
    return ERR_OK;
}

enum errno cmdline_extract_u8(struct cmdline_value *val, uint8_t *out) {
    if (!val || !out)
        return ERR_INVAL;
    uint32_t v = 0;
    enum errno err = cmdline_extract_u32(val, &v);
    if (err != ERR_OK)
        return err;
    if (v > UINT8_MAX)
        return ERR_OVERFLOW;
    *out = (uint8_t) v;
    return ERR_OK;
}

enum errno cmdline_extract_i8(struct cmdline_value *val, int8_t *out) {
    if (!val || !out)
        return ERR_INVAL;
    int32_t v = 0;
    enum errno err = cmdline_extract_i32(val, &v);
    if (err != ERR_OK)
        return err;
    if (v < INT8_MIN || v > INT8_MAX)
        return ERR_OVERFLOW;
    *out = (int8_t) v;
    return ERR_OK;
}

uint64_t cmdline_entry_value_u64(const struct cmdline_entry *e) {
    if (!e)
        return 0;

    if (cmdline_value_is_typed(&e->value) && e->value.write_to) {
        switch (e->value.c_type) {
        case TYPE_BOOL: return *(const bool *) e->value.write_to;
        case TYPE_INT8: return (uint64_t) *(const int8_t *) e->value.write_to;
        case TYPE_UINT8: return *(const uint8_t *) e->value.write_to;
        case TYPE_INT16: return (uint64_t) *(const int16_t *) e->value.write_to;
        case TYPE_UINT16: return *(const uint16_t *) e->value.write_to;
        case TYPE_INT32: return (uint64_t) *(const int32_t *) e->value.write_to;
        case TYPE_UINT32: return *(const uint32_t *) e->value.write_to;
        case TYPE_INT64: return (uint64_t) *(const int64_t *) e->value.write_to;
        case TYPE_UINT64: return *(const uint64_t *) e->value.write_to;
        default: return *(const uint64_t *) e->value.write_to;
        }
    }

    if (e->types >= (1ULL << CMDLINE_TYPE_OFFSET)) {
        if (e->value.type == CMDLINE_TYPE_STRING ||
            e->value.type == CMDLINE_TYPE_LIST ||
            e->value.type == CMDLINE_TYPE_CPU_MASK ||
            e->value.type == CMDLINE_TYPE_RANGE) {
            panic("CMDLINE_VALUE called on non-scalar cmdline entry '%s'",
                  e->name);
        }
    }

    return e->value.u64;
}
