#include <stdint.h>

void cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd) {
    __asm__ volatile("cpuid"
                     : "=a"(abcd[0]), "=b"(abcd[1]), "=c"(abcd[2]),
                       "=d"(abcd[3])
                     : "a"(eax), "c"(ecx));
}

uint64_t read_cr4() {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

void write_cr4(uint64_t cr4) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

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

    // Enable SMAP (bit 21), SMEP (bit 20), and UMIP (bit 11)
    cr4 |= (1 << 21) | (1 << 20) | (1 << 11);

    write_cr4(cr4);
}
