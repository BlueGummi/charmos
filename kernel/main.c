#include <acpi/print.h>
#include <ahci.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/ide.h>
#include <devices/nvme.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <fs/fat32.h>
#include <fs/fat32_print.h>
#include <fs/supersector.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/dbg.h>
#include <misc/linker_symbols.h>
#include <misc/logo.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <pci/pci.h>
#include <pit.h>
#include <requests.h>
#include <rust.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/print.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <vfs/vfs.h>

struct scheduler global_sched;

void k_sch_main() {
    k_printf("idle task\n");
    while (1) {
        asm volatile("hlt");
    }
}

uint64_t a_rsdp = 0;
uint64_t tsc_freq = 0;

static uacpi_iteration_decision
match_rtc(void *user, uacpi_namespace_node *node, uacpi_u32 a) {
    (void) a;
    (void) user;
    uacpi_resources *rtc_data;

    uacpi_status ret = uacpi_get_current_resources(node, &rtc_data);
    if (uacpi_unlikely_error(ret)) {
        k_printf("unable to retrieve RTC resources: %s",
                 uacpi_status_to_string(ret));
        return UACPI_ITERATION_DECISION_NEXT_PEER;
    }
    k_printf("Len is %u\n", rtc_data->length);
    uacpi_free_resources(rtc_data);

    return UACPI_ITERATION_DECISION_CONTINUE;
}

void k_main(void) {
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);
    a_rsdp = rsdp_request.response->address;
    struct limine_mp_response *mpr = mp_request.response;

    for (uint64_t i = 0; i < mpr->cpu_count; i++) {
        struct limine_mp_info *curr_cpu = mpr->cpus[i];
        curr_cpu->goto_address = wakeup;
    }

    enable_smap_smep_umip();
    gdt_install();
    idt_install();
    init_physical_allocator(r->offset, memmap_request);
    vmm_offset_set(r->offset);
    vmm_init();
    slab_init();
    test_alloc();
    tsc_freq = measure_tsc_freq_pit();
    /*   uacpi_initialize(0);
        uacpi_namespace_load();
        uacpi_namespace_initialize();

        uacpi_namespace_for_each_child(uacpi_namespace_root(), acpi_print_ctx,
                                       UACPI_NULL, UACPI_OBJECT_DEVICE_BIT,
                                       UACPI_MAX_DEPTH_ANY, UACPI_NULL);*/

    uacpi_find_devices("PNP0B00", match_rtc, NULL);
    struct pci_device *devices;
    uint64_t count;
    pci_scan_devices(&devices, &count);
    asm volatile("sti");
    uint8_t drive_status = ide_detect_drives();

    struct ide_drive drives[4] = {0};
    struct generic_supersector supersectors[4] = {0};
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            ide_setup_drive(&drives[i * 2 + j], devices, count, i, j);
        }
    }

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int ind = i * 2 + j;
            if ((drive_status >> (3 - ind) & 1) == 0) {
                continue;
            }
            struct generic_disk *d = ide_create_generic(&drives[ind]);
            enum fs_type fst = detect_fs(d);
            switch (fst) {
            case FS_FAT32: {
                supersectors[ind].supersector = fat32_read_bpb(d);
                supersectors[ind].type = SSFS_FAT32;
                break;
            }
            case FS_EXT2: {
                supersectors[ind].type = SSFS_EXT2;
                supersectors[ind].supersector =
                    kmalloc(sizeof(struct ext2_sblock));
                ext2_read_superblock(d, 0, supersectors[ind].supersector);
                break;
            }
            case FS_EXFAT:
            case FS_EXT3:
            case FS_EXT4:
            case FS_FAT12:
            case FS_FAT16:
            case FS_ISO9660:
            case FS_NTFS:
                k_printf("Filesystem %s not yet implemented...\n",
                         detect_fstr(fst));
                break;
            case FS_UNKNOWN: k_printf("Unknown filesystem\n"); break;
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        struct generic_supersector ss = supersectors[i];
        switch (ss.type) {
        case SSFS_EXT2: {
            ext2_print_superblock(supersectors[i].supersector);
            break;
        }
        case SSFS_FAT32: {
            fat32_print_bpb(supersectors[i].supersector);
            break;
        }
        default: continue;
        }
    }

    nvme_scan_pci();
    sleep(1);
    ahci_pci_discover();

    scheduler_init(&global_sched);
    scheduler_add_thread(&global_sched, thread_create(k_sch_main));
    scheduler_start();
    while (1) {
        asm("hlt");
    }
}
