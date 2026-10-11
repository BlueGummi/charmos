#include <boot/gdt.h>
#include <boot/tss.h>
#include <console/panic.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/must.h>
#include <mem/page.h>
#include <stdint.h>
#include <string.h>

#define GDT_ENTRIES 7

void gdt_set_tss(struct gdt_entry_tss *tss_desc, uint64_t base,
                 uint32_t limit) {
    tss_desc->limit_low = limit & 0xFFFF;
    tss_desc->base_low = base & 0xFFFF;
    tss_desc->base_middle = (base >> 16) & 0xFF;
    tss_desc->access = 0x89; // Present, type = 64-bit TSS (available)
    tss_desc->granularity = (limit >> 16) & 0x0F;
    tss_desc->granularity |= 0x00; // No granularity for TSS
    tss_desc->base_high = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved = 0;
}

void gdt_set_gate(struct gdt_entry *gdt, int num, uint64_t base, uint32_t limit,
                  uint8_t access, uint8_t gran) {
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access = access;
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].base_high = (base >> 24) & 0xFF;
}

static void gdt_lgdt(struct gdt_entry *gdt, uint64_t entry_count) {
    struct gdt_ptr gp = {
        .limit = (sizeof(struct gdt_entry) * entry_count) - 1,
        .base = (uint64_t) gdt,
    };
    asm volatile("lgdt %0" : : "m"(gp));
}

void reload_segment_registers(uint16_t cs_selector, uint16_t ds_selector) {
    asm volatile("pushq %0\n\t"
                 "leaq 1f(%%rip), %%rax\n\t"
                 "pushq %%rax\n\t"
                 "lretq\n\t"
                 "1:\n\t"
                 "movw %w1, %%ax\n\t"
                 "movw %%ax, %%ds\n\t"
                 "movw %%ax, %%es\n\t"
                 "movw %%ax, %%fs\n\t"
                 "movw %%ax, %%gs\n\t"
                 "movw %%ax, %%ss\n\t"
                 :
                 : "r"((uint64_t) cs_selector), "r"(ds_selector)
                 : "rax", "memory");
}

void gdt_init(struct gdt_entry *gdt, struct tss *tss) {
    gdt_set_gate(gdt, 0, 0, 0, 0, 0); // Null

    gdt_set_gate(gdt, 1, 0, 0xFFFFFFFF, ACCESS_CODE_RING0, GRAN_CODE);
    gdt_set_gate(gdt, 2, 0, 0xFFFFFFFF, ACCESS_DATA_RING0, GRAN_DATA);
    gdt_set_gate(gdt, 5, 0, 0xFFFFFFFF, ACCESS_CODE_RING3, GRAN_CODE);
    gdt_set_gate(gdt, 6, 0, 0xFFFFFFFF, ACCESS_DATA_RING3, GRAN_DATA);

    // TSS (occupies entries 3 and 4)
    gdt_set_tss((struct gdt_entry_tss *) &gdt[3], (uint64_t) tss,
                sizeof(struct tss) - 1);

    tss->io_map_base = sizeof(struct tss);

    gdt_lgdt(gdt, GDT_ENTRIES);
    tss->ist1 = (uint64_t) must(valloc_aligned(8 * PAGE_SIZE, PAGE_SIZE));
    tss->rsp0 = (uint64_t) must(valloc_aligned(8 * PAGE_SIZE, PAGE_SIZE));

    /* stacks grow down */
    tss->ist1 += 8 * PAGE_SIZE;
    tss->rsp0 += 8 * PAGE_SIZE;

    reload_segment_registers(GDT_KERNEL_CODE, GDT_KERNEL_DATA);

    asm volatile("ltr %w0" : : "r"(0x18)); // TSS selector
}

void gdt_load(void) {
    struct gdt_entry *gdt =
        valloc_aligned(sizeof(struct gdt_entry) * GDT_ENTRIES, 64, ALLOC_ZERO);
    struct tss *tss = valloc_aligned(sizeof(struct tss), 64, ALLOC_ZERO);
    if (!gdt || !tss)
        panic("GDT INIT NOT OK!!!");

    gdt_init(gdt, tss);

    return;
}
