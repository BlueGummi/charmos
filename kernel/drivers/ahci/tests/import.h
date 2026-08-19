#pragma once
#include <drivers/ahci.h>
#include <stdint.h>
#include <test/export.h>

TEST_IMPORT(void, ahci_set_lba_cmd, struct ahci_fis_reg_h2d *fis, uint64_t lba,
            uint16_t sector_count);
