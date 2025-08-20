#include <asm.h>
#include <stdint.h>

void smap_init() {
    uint32_t a, b, c, d;

    cpuid(0x7, 0x0, &a, &b, &c, &d);
    if (!(b & (1 << 20)))
        return;

    if (!(b & (1 << 7)))
        return;

    if (!(c & (1 << 2)))
        return;

    uint64_t cr4 = read_cr4();

    cr4 |= (1 << 21) | (1 << 20) | (1 << 11);

    write_cr4(cr4);
}
