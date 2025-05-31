#include <asm.h>
#include <stdint.h>

void enable_smap_smep_umip() {
    uint32_t abcd[4];

    cpuid(0x7, 0x0, abcd);
    if (!(abcd[1] & (1 << 20))) {
        // SMAP not supported
        return;
    }
    if (!(abcd[1] & (1 << 7))) {
        // SMEP not supported
        return;
    }
    if (!(abcd[2] & (1 << 2))) {
        // UMIP not supported
        return;
    }

    uint64_t cr4 = read_cr4();

    cr4 |= (1 << 21) | (1 << 20) | (1 << 11);

    write_cr4(cr4);
}
