cmake_minimum_required(VERSION 3.16)

foreach (_required BUNDLE_DIR CMDLINE OUTPUT_ISO WORK_DIR)
    if (NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "repack_nightmare_bundle: ${_required} is required")
    endif ()
endforeach ()

get_filename_component(BUNDLE_DIR "${BUNDLE_DIR}" ABSOLUTE)
get_filename_component(CMDLINE "${CMDLINE}" ABSOLUTE)
get_filename_component(OUTPUT_ISO "${OUTPUT_ISO}" ABSOLUTE)
get_filename_component(WORK_DIR "${WORK_DIR}" ABSOLUTE)

set(_artifacts "${BUNDLE_DIR}/artifacts")
set(_iso_root "${WORK_DIR}/iso_root")
file(REMOVE_RECURSE "${_iso_root}")
file(MAKE_DIRECTORY "${_iso_root}/boot/limine" "${_iso_root}/EFI/BOOT")

file(COPY_FILE "${_artifacts}/kernel" "${_iso_root}/boot/kernel" ONLY_IF_DIFFERENT)
set(ENV{CMDLINE} "${CMDLINE}")
set(ENV{NIGHTMARE_TESTS} "")
set(ENV{TESTS} "")
set(ENV{EXTRA_CMDLINE} "")
execute_process(COMMAND "${CMAKE_COMMAND}" -DIN=${_artifacts}/limine.conf -DOUT=${_iso_root}/boot/limine/limine.conf -P
                        ${CMAKE_CURRENT_LIST_DIR}/gen_limine_conf.cmake COMMAND_ERROR_IS_FATAL ANY)

foreach (_asset limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin)
    file(COPY_FILE "${_artifacts}/${_asset}" "${_iso_root}/boot/limine/${_asset}" ONLY_IF_DIFFERENT)
endforeach ()
foreach (_asset BOOTX64.EFI BOOTIA32.EFI)
    file(COPY_FILE "${_artifacts}/${_asset}" "${_iso_root}/EFI/BOOT/${_asset}" ONLY_IF_DIFFERENT)
endforeach ()

execute_process(
    COMMAND
        xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table
        -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image
        --protective-msdos-label "${_iso_root}" -o "${OUTPUT_ISO}"
    OUTPUT_QUIET ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${_artifacts}/limine" bios-install "${OUTPUT_ISO}" OUTPUT_QUIET ERROR_QUIET
                                                                            COMMAND_ERROR_IS_FATAL ANY)
