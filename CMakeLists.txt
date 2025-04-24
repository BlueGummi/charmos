cmake_minimum_required(VERSION 3.16)
project(OS C ASM_NASM)

set(CMAKE_C_STANDARD 11)
set(ARCH "x86_64")
set(IMAGE_NAME "os-${ARCH}")
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -pipe -Wall -Wextra -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-red-zone -fno-stack-protector -fno-stack-check -fno-PIC -ffunction-sections -fdata-sections -g -m64 -mcmodel=kernel -std=gnu11 -mgeneral-regs-only -DLIMINE_API_REVISION=2 -DUACPI_BAREBONES_MODE -Wno-address-of-packed-member -target x86_64-unknown-linux-gnu")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--build-id=none -nostdlib -static -z max-page-size=0x1000 -Wl,--gc-sections -T ../../kernel/linker-${ARCH}.ld")

enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-F dwarf -g -Wall -f elf64")

add_subdirectory(kernel)

add_custom_target(iso
    DEPENDS kernel 
    COMMAND rm -rf iso_root
    COMMAND mkdir -p iso_root/boot
    COMMAND cp kernel/kernel iso_root/boot/
    COMMAND mkdir -p iso_root/boot/limine
    COMMAND cp ../limine.conf iso_root/boot/limine/
    COMMAND mkdir -p iso_root/EFI/BOOT
    COMMAND cp ../limine/limine-bios.sys ../limine/limine-bios-cd.bin ../limine/limine-uefi-cd.bin iso_root/boot/limine/
    COMMAND cp ../limine/BOOTX64.EFI iso_root/EFI/BOOT/
    COMMAND cp ../limine/BOOTIA32.EFI iso_root/EFI/BOOT/
    COMMAND xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin
                    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus
                    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin
                    -efi-boot-part --efi-boot-image --protective-msdos-label
                    iso_root -o ${IMAGE_NAME}.iso
    COMMAND cd ../limine && make && cd -
    COMMAND ../limine/limine bios-install ${IMAGE_NAME}.iso
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(run
    DEPENDS iso
    COMMAND qemu-system-${ARCH} -M q35 -cdrom ${IMAGE_NAME}.iso -boot d -m 2G -machine smm=off
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(clean-full
    COMMAND rm -rf iso_root ${IMAGE_NAME}.iso ${IMAGE_NAME}.hdd kernel-deps limine ovmf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_include_directories(
    kernel
    PRIVATE
    include
)
