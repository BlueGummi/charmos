#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

bool nvme_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    uint64_t total_bytes = count * 512;
    uint64_t pages_needed = (total_bytes + 4095) / PAGE_SIZE;

    uint64_t buffer_phys = (uint64_t) pmm_alloc_pages(pages_needed, false);
    if (!buffer_phys)
        return false;

    void *virt = vmm_map_phys(buffer_phys, pages_needed * PAGE_SIZE);
    memset(virt, 0, pages_needed * PAGE_SIZE);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_READ;
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;
    if (pages_needed > 1) {
        cmd.prp2 = buffer_phys + PAGE_SIZE;
    }
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    if (nvme_submit_io_cmd(nvme, &cmd, 1) != 0) {
        vmm_unmap_region((uint64_t) virt, pages_needed * PAGE_SIZE);
        return false;
    }

    memcpy(buffer, virt, total_bytes);
    vmm_unmap_region((uint64_t) virt, pages_needed * PAGE_SIZE);
    return true;
}

bool nvme_write_sector(struct generic_disk *disk, uint64_t lba,
                       const uint8_t *buffer, uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    uint64_t total_bytes = count * 512;
    uint64_t pages_needed = (total_bytes + 4095) / PAGE_SIZE;

    uint64_t buffer_phys = (uint64_t) pmm_alloc_pages(pages_needed, false);
    if (!buffer_phys)
        return false;

    void *virt = vmm_map_phys(buffer_phys, pages_needed * PAGE_SIZE);
    memcpy(virt, buffer, total_bytes);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_WRITE;
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;
    if (pages_needed > 1) {
        cmd.prp2 = buffer_phys + PAGE_SIZE;
    }
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    if (nvme_submit_io_cmd(nvme, &cmd, 1) != 0) {
        vmm_unmap_region((uint64_t) virt, pages_needed * PAGE_SIZE);
        return false;
    }

    vmm_unmap_region((uint64_t) virt, pages_needed * PAGE_SIZE);
    return true;
}

bool nvme_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_read_sector(disk, lba, buf, chunk))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}

bool nvme_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_write_sector(disk, lba, buf, chunk))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}
