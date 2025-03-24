#include <stddef.h>
#include <stdint.h>
#include <system/memfuncs.h>
#include <system/printf.h>

#define AML_S5_SIGNATURE "\x5F\x53\x35\x5F" // "_S5_"

uint16_t find_s5_in_dsdt(uint8_t *dsdt, size_t length) {
    for (size_t i = 0; i < length - 4; i++) {
        if (memcmp(&dsdt[i], AML_S5_SIGNATURE, 4) == 0) {
            // Found "_S5_"
            uint8_t *s5_ptr = &dsdt[i + 4];

            if (s5_ptr[0] == 0x12) {
                uint8_t package_length = s5_ptr[1];
                if (package_length >= 2) {
                    uint8_t slp_typa = s5_ptr[2];
                    return (uint16_t) slp_typa;
                }
            }
        }
    }

    return 0xFFFF;
}
