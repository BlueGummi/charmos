option(QEMU_KVM "Enable KVM acceleration" OFF)
option(QEMU_IOMMU "Attach intel-iommu device with intremap" ON)
option(QEMU_NUMA "Configure 4-node NUMA topology" ON)
option(QEMU_USB "Attach xHCI controller with USB kbd/mouse" ON)
option(QEMU_NVME "Attach NVMe drive backed by disk.img" ON)
option(QEMU_DEBUG_EXIT "Attach isa-debug-exit to the tests target" ON)
option(QEMU_NDJSON "Attach a second serial device for the NDJSON" ON)
option(QEMU_GDB_WAIT "Halt at startup waiting for gdb (-S)" OFF)
option(QEMU_TRACE "Trace events into trace.log" ON)
option(QEMU_LOAD_ACPI "Load custom ACPI tables from build/acpi/" OFF)

set(QEMU_MEM_SIZE
    "8G"
    CACHE STRING "QEMU guest memory size")
set(QEMU_SMP_TOPO
    "sockets=2,cores=2,threads=2"
    CACHE STRING "QEMU -smp topology string")

add_custom_target(
    iso
    DEPENDS kernel
    COMMAND ${CMAKE_COMMAND} -E rm -rf iso_root
    COMMAND ${CMAKE_COMMAND} -E make_directory iso_root/boot
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel> iso_root/boot/
    COMMAND ${CMAKE_COMMAND} -E make_directory iso_root/boot/limine
    COMMAND
        ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/kernel/limine.conf
        -DOUT=${CMAKE_BINARY_DIR}/iso_root/boot/limine/limine.conf -P ${CMAKE_SOURCE_DIR}/cmake/gen_limine_conf.cmake
    COMMAND ${CMAKE_COMMAND} -E make_directory iso_root/EFI/BOOT
    COMMAND
        ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/limine/limine-bios.sys
        ${CMAKE_SOURCE_DIR}/limine/limine-bios-cd.bin ${CMAKE_SOURCE_DIR}/limine/limine-uefi-cd.bin
        iso_root/boot/limine/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/limine/BOOTX64.EFI iso_root/EFI/BOOT/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/limine/BOOTIA32.EFI iso_root/EFI/BOOT/
    COMMAND
        xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table
        -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image
        --protective-msdos-label iso_root -o ${IMAGE_NAME}.iso > /dev/null 2>&1
    COMMAND make -C ${CMAKE_SOURCE_DIR}/limine
    COMMAND ${CMAKE_SOURCE_DIR}/limine/limine bios-install ${IMAGE_NAME}.iso
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building bootable ISO: ${IMAGE_NAME}.iso")

set(QEMU_FLAGS
    -cdrom
    ${IMAGE_NAME}.iso
    -boot
    d
    -m
    ${QEMU_MEM_SIZE}
    -smp
    ${QEMU_SMP_TOPO}
    -M
    q35
    -qmp
    unix:/tmp/qmp.sock,server,nowait
    -monitor
    none)

if (QEMU_KVM)
    list(APPEND QEMU_FLAGS -enable-kvm -cpu host)
endif ()

if (QEMU_GDB_WAIT)
    list(APPEND QEMU_FLAGS -S)
endif ()

if (QEMU_NUMA)
    list(
        APPEND
        QEMU_FLAGS
        -object
        memory-backend-ram,size=2G,id=mem0
        -object
        memory-backend-ram,size=2G,id=mem1
        -object
        memory-backend-ram,size=2G,id=mem2
        -object
        memory-backend-ram,size=2G,id=mem3
        -numa
        node,cpus=0-1,nodeid=0,memdev=mem0
        -numa
        node,cpus=2-3,nodeid=1,memdev=mem1
        -numa
        node,cpus=4-5,nodeid=2,memdev=mem2
        -numa
        node,cpus=6-7,nodeid=3,memdev=mem3
        -numa
        dist,src=0,dst=1,val=15
        -numa
        dist,src=1,dst=0,val=15
        -numa
        dist,src=0,dst=2,val=20
        -numa
        dist,src=2,dst=0,val=20
        -numa
        dist,src=0,dst=3,val=30
        -numa
        dist,src=3,dst=0,val=30
        -numa
        dist,src=1,dst=2,val=25
        -numa
        dist,src=2,dst=1,val=25
        -numa
        dist,src=1,dst=3,val=35
        -numa
        dist,src=3,dst=1,val=35
        -numa
        dist,src=2,dst=3,val=15
        -numa
        dist,src=3,dst=2,val=15)
endif ()

if (QEMU_USB)
    list(
        APPEND
        QEMU_FLAGS
        -device
        qemu-xhci,id=xhci
        -device
        usb-kbd,bus=xhci.0,port=1,id=usbkbd
        -device
        usb-mouse,bus=xhci.0,port=2,id=usbmouse)
endif ()

if (QEMU_IOMMU)
    list(APPEND QEMU_FLAGS -device intel-iommu,intremap=on)
endif ()

if (QEMU_NVME)
    list(APPEND QEMU_FLAGS -drive id=nvme0,file=disk.img,format=raw,if=none -device nvme,serial=boom,drive=nvme0)
endif ()

set(QEMU_DEBUG_EXIT_FLAGS)
if (QEMU_DEBUG_EXIT)
    set(QEMU_DEBUG_EXIT_FLAGS -device isa-debug-exit,iobase=0xf4,iosize=0x04)
endif ()

# The console is serial0 and the machine channel is serial1
set(NDJSON_LOG "${CMAKE_BINARY_DIR}/ndjson.log")
set(QEMU_NDJSON_FLAGS)
if (QEMU_NDJSON)
    set(QEMU_NDJSON_FLAGS -serial file:${NDJSON_LOG})
endif ()

if (QEMU_TRACE)
    list(APPEND QEMU_FLAGS -d "trace:*xhci*" -trace file=trace.log)
endif ()

if (QEMU_LOAD_ACPI)
    set(ACPI_TABLES
        apic
        dmar
        dsdt
        ecdt
        facp
        facs
        hpet
        mcfg
        sbst
        ssdt1
        ssdt2
        ssdt3
        ssdt5
        ssdt6
        ssdt7
        ssdt8
        ssdt9
        ssdt10
        ssdt11)
    foreach (tbl ${ACPI_TABLES})
        list(APPEND QEMU_FLAGS -acpitable file=acpi/${tbl}.dat)
    endforeach ()
endif ()

set(DISK_PRISTINE "${CMAKE_BINARY_DIR}/d.img")
set(DISK_RUNTIME "${CMAKE_BINARY_DIR}/disk.img")
set(DISK_SIZE_MB
    "8"
    CACHE STRING "Disk image size in megabytes")

find_program(
    MKE2FS_BIN
    NAMES mke2fs
    PATHS /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin
    PATH_SUFFIXES sbin)
if (NOT MKE2FS_BIN)
    message(WARNING "mke2fs not found - disk image generation will fail. "
                    "Install e2fsprogs (brew install e2fsprogs).")
    set(MKE2FS_BIN "mke2fs")
endif ()

add_custom_command(
    OUTPUT ${DISK_PRISTINE}
    COMMAND dd if=/dev/zero of=${DISK_PRISTINE} bs=1M count=${DISK_SIZE_MB} status=none
    COMMAND ${MKE2FS_BIN} -t ext2 -q ${DISK_PRISTINE}
    COMMENT "Creating pristine ${DISK_SIZE_MB}MB ext2 image"
    VERBATIM)
add_custom_target(pristine-disk DEPENDS ${DISK_PRISTINE})

function (register_run_target tgt)
    set(extra_args ${ARGN})
    set(debug_exit 0)
    if ("DEBUG_EXIT" IN_LIST extra_args)
        list(REMOVE_ITEM extra_args DEBUG_EXIT)
        set(debug_exit 1)
        list(APPEND extra_args ${QEMU_DEBUG_EXIT_FLAGS})
    endif ()

    if ("NDJSON" IN_LIST extra_args)
        list(REMOVE_ITEM extra_args NDJSON)
        list(APPEND extra_args ${QEMU_NDJSON_FLAGS})
    endif ()

    add_custom_target(
        ${tgt}
        DEPENDS iso pristine-disk
        COMMAND ${CMAKE_COMMAND} -E copy ${DISK_PRISTINE} ${DISK_RUNTIME}
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_qemu.sh ${CMAKE_BINARY_DIR}/output.log ${debug_exit} qemu-system-${ARCH}
                ${QEMU_FLAGS} ${extra_args}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL)
endfunction ()

register_run_target(run NDJSON -serial stdio -no-shutdown -no-reboot)
register_run_target(headless NDJSON -nographic -serial mon:stdio -no-shutdown -no-reboot)
register_run_target(tests DEBUG_EXIT NDJSON -nographic -serial mon:stdio -no-reboot)
register_run_target(debug NDJSON -s -S -serial stdio -no-shutdown -no-reboot)
