#pragma once
#include <block/bio.h>
#include <stdint.h>
#include <test/export.h>

TEST_IMPORT(enum bio_request_status, nvme_to_bio_status, uint16_t status_word);
