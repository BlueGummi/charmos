#include <asm.h>
#include <console/printf.h>
#include <elf.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define ELF_MAGIC 0x464C457F // "\x7FELF"

#define USER_STACK_TOP 0x7FFFFFF000
#define USER_STACK_SIZE (16 * PAGE_SIZE)

uint64_t elf_load(const void *elf_data) {
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

        for (uint64_t va = va_start; va < va_end; va += 0x1000) {
            uint64_t phys = (uint64_t) pmm_alloc_page(false);
            vmm_map_page(va, phys,
                         PAGING_PRESENT | PAGING_USER_ALLOWED | PAGING_WRITE);
        }
    }

    return ehdr->entry;
}

void elf_map(uintptr_t user_pml4_phys, void *elf_data) {
    struct elf64_ehdr *ehdr = (struct elf64_ehdr *) elf_data;
    struct elf64_phdr *phdrs =
        (struct elf64_phdr *) ((uint8_t *) elf_data + ehdr->phoff);

    for (int i = 0; i < ehdr->phnum; i++) {
        struct elf64_phdr *ph = &phdrs[i];
        if (ph->type != PT_LOAD)
            continue;

        uintptr_t seg_vaddr_start = PAGE_ALIGN_DOWN(ph->vaddr);
        uintptr_t seg_vaddr_end = PAGE_ALIGN_UP(ph->vaddr + ph->memsz);
        uintptr_t file_start = ph->offset;
        uintptr_t file_end = ph->offset + ph->filesz;

        uint64_t flags = PAGING_USER_ALLOWED | PAGING_PRESENT;
        if (ph->flags & PF_W)
            flags |= PAGING_WRITE;
        if (!(ph->flags & PF_X))
            flags |= PAGING_XD;

        for (uintptr_t vaddr = seg_vaddr_start; vaddr < seg_vaddr_end;
             vaddr += PAGE_SIZE) {

            uintptr_t phys = (uintptr_t) pmm_alloc_page(false);
            if (!phys)
                k_panic("Failed to allocate page for user ELF segment\n");

            void *phys_mapped = vmm_map_phys(phys, PAGE_SIZE);
            memset(phys_mapped, 0, PAGE_SIZE);

            uintptr_t offset_in_seg = vaddr - seg_vaddr_start;
            uintptr_t file_offset_in_seg =
                offset_in_seg + (ph->vaddr - seg_vaddr_start);
            uintptr_t file_pos = file_start + file_offset_in_seg;

            uintptr_t page_offset = 0;
            if (vaddr == seg_vaddr_start)
                page_offset = ph->vaddr & (PAGE_SIZE - 1);

            size_t to_copy = 0;
            if (file_pos < file_end) {
                size_t bytes_remaining = file_end - file_pos;
                to_copy = PAGE_SIZE - page_offset;
                if (bytes_remaining < to_copy)
                    to_copy = bytes_remaining;

                memcpy((uint8_t *) phys_mapped + page_offset,
                       (uint8_t *) elf_data + file_pos, to_copy);
            }

            vmm_map_page_user(user_pml4_phys, vaddr, phys, flags);
        }
    }
}

uintptr_t map_user_stack(uintptr_t user_pml4_phys) {
    uintptr_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;

    for (uintptr_t v = stack_bottom; v < USER_STACK_TOP; v += PAGE_SIZE) {
        uintptr_t phys = (uintptr_t) pmm_alloc_page(false);
        if (!phys)
            k_panic("Failed to alloc user stack\n");

        vmm_map_page_user(user_pml4_phys, v, phys,
                          PAGING_WRITE | PAGING_USER_ALLOWED | PAGING_PRESENT);
    }

    return USER_STACK_TOP - 0x2000;
}

void syscall_setup(void *syscall_entry) {
    uint64_t efer = rdmsr(0xC0000080);
    efer |= (1 << 0); // SCE: Enable syscall/sysret
    wrmsr(0xC0000080, efer);

    wrmsr(0xC0000082, (uint64_t) syscall_entry);

    wrmsr(0xC0000084, (1 << 9));

    uint64_t star = ((uint64_t) 0x08 << 32) | ((uint64_t) 0x28 << 48);
    wrmsr(0xC0000081, star);
}

__attribute__((noreturn)) void
enter_userspace(uintptr_t entry_point, uintptr_t user_stack_top,
                uint16_t user_cs, uint16_t user_ss, uintptr_t user_pml4_phys) {
    asm volatile("mov %0, %%cr3" : : "r"(user_pml4_phys) : "memory");

    uint64_t rflags;
    asm volatile("pushfq; popq %0" : "=r"(rflags));
    rflags |= (1 << 9);

    asm volatile("cli\n"
                 "pushq %0\n" // SS
                 "pushq %1\n" // RSP
                 "pushq %2\n" // RFLAGS
                 "pushq %3\n" // CS
                 "pushq %4\n" // RIP
                 "iretq\n"
                 :
                 : "r"((uint64_t) user_ss), "r"(user_stack_top), "r"(rflags),
                   "r"((uint64_t) user_cs), "r"(entry_point)
                 : "memory");

    __builtin_unreachable();
}
