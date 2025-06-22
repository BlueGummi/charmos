#include <elf.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>

#define ELF_MAGIC 0x464C457F // "\x7FELF"

#define USER_STACK_TOP 0x7FFFFFF000

uint64_t load_user_elf(void *elf_data) {
    struct elf64_ehdr *ehdr = (struct elf64_ehdr *) elf_data;

    if (ehdr->ident.magic != ELF_MAGIC || ehdr->ident.class != 2) {
        k_printf("Invalid ELF64\n");
        return 0;
    }

    struct elf64_phdr *phdrs =
        (struct elf64_phdr *) ((uint8_t *) elf_data + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        struct elf64_phdr *ph = &phdrs[i];
        if (ph->type != 1)
            continue;

        uint64_t va_start = PAGE_ALIGN_DOWN(ph->vaddr);
        uint64_t va_end = PAGE_ALIGN_UP(ph->vaddr + ph->memsz);
        uint64_t offset = ph->offset;

        for (uint64_t va = va_start; va < va_end; va += 0x1000) {
            uint64_t phys = (uint64_t) pmm_alloc_page(false);
            vmm_map_page(va, phys,
                         PAGING_PRESENT | PAGING_USER_ALLOWED | PAGING_WRITE);
        }

        memcpy((void *) ph->vaddr, (uint8_t *) elf_data + offset, ph->filesz);

        memset((void *) (ph->vaddr + ph->filesz), 0, ph->memsz - ph->filesz);
    }

    for (int i = 0; i < 4; i++) {
        uint64_t phys = (uint64_t) pmm_alloc_page(false);
        vmm_map_page((USER_STACK_TOP - i * 0x1000), phys,
                     PAGING_PRESENT | PAGING_USER_ALLOWED | PAGING_WRITE);
    }

    return ehdr->entry;
}
