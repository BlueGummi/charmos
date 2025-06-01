#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

bool nvme_read_sector(struct generic_disk *disk, uint32_t lba,
                      uint8_t *buffer) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    if (!buffer_phys)
        return false;

    void *virt = vmm_map_phys(buffer_phys, 4096);
    memset(virt, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = 0x02; // READ
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = lba;
    cmd.cdw11 = 0;
    cmd.cdw12 = 0; // 0 = read 1 block (0-based)

    if (nvme_submit_io_cmd(nvme, &cmd) != 0)
        return false;

    memcpy(buffer, virt, 512);
    return true;
}

bool nvme_write_sector(struct generic_disk *disk, uint32_t lba,
                       const uint8_t *buffer) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    if (!buffer_phys)
        return false;

    void *virt = vmm_map_phys(buffer_phys, 4096);
    memcpy(virt, buffer, 512);

    struct nvme_command cmd = {0};
    cmd.opc = 0x01; // WRITE
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = lba;
    cmd.cdw11 = 0;
    cmd.cdw12 = 0; // 0 = write 1 block

    if (nvme_submit_io_cmd(nvme, &cmd) != 0)
        return false;

    return true;
}
