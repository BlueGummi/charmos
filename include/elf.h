/* @title: ELF */
#include <stdint.h>
#pragma once

struct elf64_ident {
    uint32_t magic;
    uint8_t class;
    uint8_t data;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t pad[7];
} __attribute__((packed));

struct elf64_ehdr {
    struct elf64_ident ident;
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

/* returns entry point */
uint64_t elf_load(const void *elf_data);

__attribute__((noreturn)) void
enter_userspace(uintptr_t entry_point, uintptr_t user_stack_top,
                uint16_t user_cs, uint16_t user_ss, uintptr_t user_pml4_phys);

void syscall_setup(void *syscall_entry);

uintptr_t map_user_stack(uintptr_t user_pml4_phys);

void elf_map(uintptr_t user_pml4_phys, void *elf_data);

#define PT_LOAD 1
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
