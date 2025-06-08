cmake_minimum_required(VERSION 3.16)
project(charmOS C ASM_NASM)

set(CMAKE_C_STANDARD 11)
set(ARCH "x86_64")
set(IMAGE_NAME "charmos-${ARCH}")

set(KERNEL_COMMON_FLAGS "-O2 -pipe -Wall -Wextra -Wpointer-sign -Wenum-compare -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-red-zone -fno-stack-protector -fno-stack-check -fno-PIC -ffunction-sections -fdata-sections -g -m64 -mcmodel=kernel -mgeneral-regs-only -DLIMINE_API_REVISION=2 -Wno-address-of-packed-member -Wno-unused-command-line-argument -fsanitize=undefined")

set(CMAKE_C_FLAGS "${KERNEL_COMMON_FLAGS} -std=gnu11")

if(APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -target x86_64-unknown-unknown")
    set(CMAKE_LINKER "x86_64-elf-ld")
    set(CMAKE_EXE_LINKER_FLAGS
    "-nostdlib -static -Wl,--gc-sections -z max-page-size=0x1000 -T ${CMAKE_SOURCE_DIR}/kernel/linker-${ARCH}.ld"
)
else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--build-id=none -nostdlib -static -z max-page-size=0x1000 -Wl,--gc-sections -T ../../kernel/linker-${ARCH}.ld")
endif()

enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-F dwarf -g -Wall -f elf64 -wno-reloc-rel-dword")

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
                    iso_root -o ${IMAGE_NAME}.iso 2>&1 >/dev/null
    COMMAND cd ../limine && make && cd -
    COMMAND ../limine/limine bios-install ${IMAGE_NAME}.iso 2>&1 >/dev/null
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set(QEMU_COMMON_FLAGS
    -cdrom ${IMAGE_NAME}.iso
    -boot d
    -m 4G
    -cpu host
    -enable-kvm
    -smp cores=4
    -no-shutdown
    -no-reboot
    -s
    -monitor telnet:127.0.0.1:1234,server,nowait

    -device qemu-xhci,id=xhci
    -device usb-kbd,bus=xhci.0,id=usbkbd
    -device usb-mouse,bus=xhci.0,id=usbmouse

    -drive id=nvme0,file=disk.img,format=raw,if=none
    -device nvme,serial=boom,drive=disk0,drive=nvme0

    -drive id=fatdisk,file=fat32_disk.img,format=raw,if=none
    -device ide-hd,drive=fatdisk,bus=ide.0,unit=1

    -d trace:*nvme*
    -trace file=trace.log
)

add_custom_target(run
    DEPENDS iso
    COMMAND clear
    COMMAND qemu-system-${ARCH} ${QEMU_COMMON_FLAGS}
         -serial stdio
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(headless
    DEPENDS iso
    COMMAND clear
    COMMAND qemu-system-${ARCH} ${QEMU_COMMON_FLAGS}
        -nographic
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
