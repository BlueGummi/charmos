void cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd);

uint64_t read_cr4();

void write_cr4(uint64_t cr4);

void enable_smap_smep_umip();
