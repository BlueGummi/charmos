#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ndjson_carrier_begin(bool *irqs_were_on);
void ndjson_carrier_end(bool irqs_were_on);
void ndjson_carrier_putc(char c);
void ndjson_carrier_write(const char *s, size_t len);

void ndjson_check_duplicates(void);
void ndjson_dump_schema(void);
void ndjson_selftest(void);
