#include <kassert.h>
#include <ndjson.h>
#include <string.h>

NDJSON_DECLARE(ndjson_schema, NDJSON_SECTION_NDJSON, NDJSON_KIND_SCHEMA, 1,
               NDJSON_STR(section), NDJSON_STR(kind), NDJSON_U64(rec_version),
               NDJSON_U64(nfields), NDJSON_U64(index), NDJSON_STR(field),
               NDJSON_STR(type));

static const char *ndjson_type_str(enum ndjson_type t) {
    switch (t) {
    case NDJSON_TYPE_U64: return NDJSON_TYPE_NAME_U64;
    case NDJSON_TYPE_I64: return NDJSON_TYPE_NAME_I64;
    case NDJSON_TYPE_BOOL: return NDJSON_TYPE_NAME_BOOL;
    case NDJSON_TYPE_STR: return NDJSON_TYPE_NAME_STR;
    case NDJSON_TYPE_HEX: return NDJSON_TYPE_NAME_HEX;
    }

    return "unknown";
}

/* (section, kind) sharing is ambiguous */
void ndjson_check_duplicates(void) {
    for (struct ndjson_record *a = __skernel_ndjson_records;
         a < __ekernel_ndjson_records; a++) {
        for (struct ndjson_record *b = a + 1; b < __ekernel_ndjson_records;
             b++) {
            if (strcmp(a->section, b->section) == 0 &&
                strcmp(a->kind, b->kind) == 0)
                panic("Duplicate ndjson record: %s/%s", a->section, a->kind);
        }
    }
}

/* One record for every field */
void ndjson_dump_schema(void) {
    struct ndjson_record *rec;

    linker_section_for_each_object(rec, ndjson_records) {
        for (uint16_t i = 0; i < rec->nfields; i++) {
            const struct ndjson_field *f = &rec->fields[i];

            ndjson_emit(ndjson_schema, .section = rec->section,
                        .kind = rec->kind, .rec_version = rec->version,
                        .nfields = rec->nfields, .index = i, .field = f->name,
                        .type = ndjson_type_str(f->type));
        }
    }
}
