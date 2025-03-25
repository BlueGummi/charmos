#include <stdint.h>
#include <system/rsdt.h>

struct FADT {
    struct ACPI_SDTHeader header;
    uint32_t firmware_ctrl; // physical address of FACS
    uint32_t DSDT;          // physical address of DSDT
    uint8_t reserved;
    uint8_t preferred_PM_profile;
    uint16_t SCI_INT;
    uint32_t SMI_CMD;
    uint8_t ACPI_ENABLE;
    uint8_t ACPI_DISABLE;
    uint8_t S4BIOS_REQ;
    uint8_t PSTATE_CNT;
    uint32_t PM1a_EVT_BLK;
    uint32_t PM1b_EVT_BLK;
    uint32_t PM1a_CNT_BLK;
    uint32_t PM1b_CNT_BLK;
    uint32_t PM2_CNT_BLK;
    uint32_t PM_TMR_BLK;
    uint32_t GPE0_BLK;
    uint32_t GPE1_BLK;
    uint8_t PM1_EVT_LEN;
    uint8_t PM1_CNT_LEN;
    uint8_t PM2_CNT_LEN;
    uint8_t PM_TMR_LEN;
    uint8_t GPE0_BLK_LEN;
    uint8_t GPE1_BLK_LEN;
    uint8_t GPE1_BASE;
    uint8_t CSTATE_CNT;
    uint16_t P_LVL2_LAT;
    uint16_t P_LVL3_LAT;
    uint16_t FLUSH_SIZE;
    uint16_t FLUSH_STRIDE;
    uint8_t DUTY_OFFSET;
    uint8_t DUTY_WIDTH;
    uint8_t DAY_ALRM;
    uint8_t MON_ALRM;
    uint8_t CENTURY;
    uint16_t IAPC_BOOT_ARCH;
    uint8_t reserved_2;
    uint32_t flags;
    uint64_t reset_reg;
    uint8_t reset_value;
    uint8_t reserved_3[3];
    uint64_t X_FirmwareCtrl; // 64-bit version of FirmwareCtrl
    uint64_t X_DSDT;         // 64-bit version of DSDT
    // More fields can exist depending on the revision
} __attribute__((packed));
