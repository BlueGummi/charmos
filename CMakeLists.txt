cmake_minimum_required(VERSION 3.16)
project(charmOS C ASM_NASM)

set(CMAKE_C_STANDARD 11)
set(ARCH "x86_64")
set(IMAGE_NAME "charmos-${ARCH}")

set(PROFILING_FLAGS
    PROFILING_SCHED
    PROFILING_VFS
)

set(TEST_FLAGS
    TEST_SCHED
    TEST_TMPFS
    TEST_EXT2
    TEST_MEM
    TEST_MINHEAP
    TEST_RCU
    TEST_RWLOCK
    TEST_MUTEX
    TEST_TIMER_DEFER
    TEST_FS
    TEST_APC
    TEST_BIO
    TEST_BIO_SCHED
)

set(TEST_NIGHTMARE_FLAGS
    TEST_NIGHTMARE_LOCKS
    TEST_NIGHTMARE_WAKE
)

function(define_flag_group GROUP_NAME ENABLE_ALL DEFAULT_ALL FLAGS)
    option(${ENABLE_ALL} "Enable all ${GROUP_NAME} flags" ${DEFAULT_ALL})

    foreach(flag ${FLAGS})
        option(${flag} "Enable ${flag}" OFF)
    endforeach()

    if(${ENABLE_ALL})
        foreach(flag ${FLAGS})
            set(${flag} ON CACHE BOOL "" FORCE)
        endforeach()
    endif()

    # Track group enabled internally
    set(_LOCAL_GROUP_ENABLED OFF)

    foreach(flag ${FLAGS})
        if(${flag})
            set(_LOCAL_GROUP_ENABLED ON)
            add_compile_definitions(${flag})
        endif()
    endforeach()

    # Expose to parent
    set(${GROUP_NAME}_ENABLED ${_LOCAL_GROUP_ENABLED} PARENT_SCOPE)

    # Emit GROUP_ALL
    if(${ENABLE_ALL})
        add_compile_definitions(${ENABLE_ALL})
    endif()

    # Emit GROUP_ENABLED
    if(_LOCAL_GROUP_ENABLED)
        add_compile_definitions(${GROUP_NAME}_ENABLED)
    endif()
endfunction()


define_flag_group(
    PROFILING
    PROFILING_ALL
    OFF
    "${PROFILING_FLAGS}"
)

define_flag_group(
    TEST
    TEST_ALL
    ON
    "${TEST_FLAGS}"
)

define_flag_group(
    TEST_NIGHTMARE
    TEST_NIGHTMARE_ALL
    ON
    "${TEST_NIGHTMARE_FLAGS}"
)

set(KERNEL_COMMON_FLAGS "-O3 -pipe -Wall -Wextra -Wpointer-sign -fno-optimize-sibling-calls -Wenum-compare -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-3dnow -mno-red-zone -fno-stack-protector -fno-omit-frame-pointer -fno-stack-check -fno-PIC -ffunction-sections -fdata-sections  -m64 -mcmodel=kernel -fstack-usage -mgeneral-regs-only -DLIMINE_API_REVISION=2 -Wno-address-of-packed-member -Wno-unused-command-line-argument -ggdb")

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
    DEPENDS ${CMAKE_SOURCE_DIR}/syms/fullsyms.c
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
    #-enable-kvm
    #-cpu host
    -m 8G
    -smp sockets=2,cores=2,threads=2
    -object memory-backend-ram,size=2G,id=mem0
    -object memory-backend-ram,size=2G,id=mem1
    -object memory-backend-ram,size=2G,id=mem2
    -object memory-backend-ram,size=2G,id=mem3

    -numa node,cpus=0-1,nodeid=0,memdev=mem0
    -numa node,cpus=2-3,nodeid=1,memdev=mem1
    -numa node,cpus=4-5,nodeid=2,memdev=mem2
    -numa node,cpus=6-7,nodeid=3,memdev=mem3
    -numa dist,src=0,dst=1,val=15
    -numa dist,src=1,dst=0,val=15
    -numa dist,src=0,dst=2,val=20
    -numa dist,src=2,dst=0,val=20
    -numa dist,src=0,dst=3,val=30
    -numa dist,src=3,dst=0,val=30
    -numa dist,src=1,dst=2,val=25
    -numa dist,src=2,dst=1,val=25
    -numa dist,src=1,dst=3,val=35
    -numa dist,src=3,dst=1,val=35
    -numa dist,src=2,dst=3,val=15
    -numa dist,src=3,dst=2,val=15

    -s
    #-S
    -monitor telnet:127.0.0.1:5555,server,nowait

    -device qemu-xhci,id=xhci
    -device usb-kbd,bus=xhci.0,port=1
    -device usb-mouse,bus=xhci.0,port=2,id=usbmouse

    -M q35

    -drive id=nvme0,file=disk.img,format=raw,if=none
    -device nvme,serial=boom,drive=disk0,drive=nvme0#,num_queues=4
    -device isa-debug-exit,iobase=0xf4,iosize=0x04

    #-drive id=fatdisk,file=fat32.img,format=raw,if=none
    # -device nvme,serial=yay,drive=fatdisk
     #-netdev bridge,id=n1,br=virbr0
     #-device e1000,netdev=n1

     #-device piix3-ide,id=ide
     #-drive id=fatdisk,file=disk.img,format=raw,if=none
     #-device ide-hd,drive=fatdisk,bus=ide.0

     #-drive id=fatdisk,file=disk.img,format=raw,if=none
     #-device ahci,id=ahci0
     #-device ide-hd,drive=fatdisk,bus=ahci0.3


     -d trace:*xhci*
     -trace file=trace.log
)


add_custom_target(run
    DEPENDS iso
    COMMAND clear
    COMMAND qemu-system-${ARCH} ${QEMU_COMMON_FLAGS}
         -serial stdio
         -no-shutdown
         -no-reboot
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(headless
    DEPENDS iso
    COMMAND clear
    COMMAND qemu-system-${ARCH} ${QEMU_COMMON_FLAGS}
        -nographic
        -no-shutdown
        -no-reboot
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(tests
    DEPENDS iso
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

add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/syms/fullsyms.c
    COMMAND nm ${CMAKE_BINARY_DIR}/kernel/kernel | awk -f
            ${CMAKE_SOURCE_DIR}/scripts/syms.awk >
            ${CMAKE_SOURCE_DIR}/syms/fullsyms.c
    DEPENDS kernel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(generate_syms ALL
    DEPENDS ${CMAKE_SOURCE_DIR}/syms/fullsyms.c
)

