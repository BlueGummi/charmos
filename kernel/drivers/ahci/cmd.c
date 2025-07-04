#include <acpi/lapic.h>
#include <asm.h>
#include <block/bio.h>
#include <block/generic.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_PRDT_ENTRY_SIZE (4 * 1024 * 1024) // 4MB

void ahci_process_completions(struct ahci_device *dev, uint32_t port) {
    struct ahci_port *p = dev->regs[port].port;
    uint32_t completed;
    completed = ~(mmio_read_32(&p->ci) | mmio_read_32(&p->sact)) & 0xFFFFFFFF;

    for (uint32_t slot = 0; slot < 32; slot++) {
        if (!(completed & (1ULL << slot)))
            continue;

        struct ahci_request *req = dev->io_requests[port][slot];

        if (!req)
            continue;

        if (req->trigger_completion) {
            req->done = true;
            req->status = 0;

            if (req->on_complete)
                req->on_complete(req);
        }

        /* if there are waiters - this ignores if not */
        if (dev->io_waiters[port][slot]) {
            struct thread *t = dev->io_waiters[port][slot];
            t->state = READY;
            t->mlfq_level = 0;
            t->time_in_level = 0;
            scheduler_put_back(t);
            lapic_send_ipi(t->curr_core, SCHEDULER_ID);
        }

        dev->io_requests[port][slot] = NULL;
    }

    mmio_write_32(&p->is, p->is);
}

void ahci_isr_handler(void *ctx, uint8_t vector, void *rsp) {
    (void) vector, (void) rsp;

    struct ahci_device *dev = ctx;
    for (uint32_t port = 0; port < AHCI_MAX_PORTS; port++) {
        if (!dev->regs[port].port)
            continue;

        struct ahci_port *p = dev->regs[port].port;
        if (mmio_read_32(&p->is) & mmio_read_32(&p->ie)) {
            ahci_process_completions(dev, port);
            mmio_write_32(&p->is, p->is);
        }
    }
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

void ahci_send_command(struct ahci_disk *disk, struct ahci_full_port *port,
                       struct ahci_request *req) {
    uint32_t slot = req->slot;

    mmio_write_32(&port->port->is, 0xFFFFFFFF);
    disk->device->io_requests[disk->port][slot] = req;

    uint32_t ci = mmio_read_32(&port->port->ci);
    ci |= (1 << slot);
    mmio_write_32(&port->port->ci, ci);
}

uint32_t ahci_find_slot(struct ahci_port *port) {
    uint32_t slots_in_use = mmio_read_32(&port->sact) | mmio_read_32(&port->ci);

    for (int slot = 0; slot < 32; slot++) {
        if ((slots_in_use & (1U << slot)) == 0) {
            return slot;
        }
    }

    return -1;
}

void ahci_prepare_command(struct ahci_full_port *port, uint32_t slot,
                          bool write, uint8_t *buf, uint64_t size) {
    struct ahci_cmd_header *hdr = port->cmd_hdrs[slot];
    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];

    if (!hdr || !cmd_tbl || size == 0)
        return;

    uint64_t prdt_count =
        (size + MAX_PRDT_ENTRY_SIZE - 1) / MAX_PRDT_ENTRY_SIZE;

    if (prdt_count > 65535)
        return;

    hdr->cfl = sizeof(struct ahci_fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    hdr->p = 0;
    hdr->a = 0;
    hdr->c = 1;
    hdr->prdtl = prdt_count;
    hdr->prdbc = 0;

    uint64_t remaining = size;
    uint64_t offset = 0;
    uint64_t phys_base = vmm_get_phys((uint64_t) buf);
    for (uint32_t i = 0; i < prdt_count; i++) {
        uint64_t chunk =
            (remaining > MAX_PRDT_ENTRY_SIZE) ? MAX_PRDT_ENTRY_SIZE : remaining;

        uint64_t phys_addr = phys_base + offset;

        cmd_tbl->prdt_entry[i].dba = (uint32_t) (phys_addr & 0xFFFFFFFF);
        cmd_tbl->prdt_entry[i].dbau = (uint32_t) (phys_addr >> 32);
        cmd_tbl->prdt_entry[i].dbc = (uint32_t) (chunk - 1); // size - 1
        cmd_tbl->prdt_entry[i].i = (i == prdt_count - 1) ? 1 : 0;

        offset += chunk;
        remaining -= chunk;
    }
}

void ahci_setup_fis(struct ahci_cmd_table *cmd_tbl, uint8_t command,
                    bool is_atapi) {
    struct ahci_fis_reg_h2d *fis = (struct ahci_fis_reg_h2d *) cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_reg_h2d));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;

    if (is_atapi) {
        fis->device = 1 << 6; // LBA bit
    }
}

void ahci_identify(struct ahci_disk *disk) {
    struct ahci_full_port *port = &disk->device->regs[disk->port];
    uint32_t slot = ahci_find_slot(port->port);

    uint8_t *buffer = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!buffer)
        return;

    ahci_prepare_command(port, slot, false, buffer, PAGE_SIZE);

    if (!buffer)
        return;

    ahci_setup_fis(port->cmd_tables[slot], AHCI_CMD_IDENTIFY, false);

    struct ahci_request req = {
        .slot = slot, .port = disk->port, .buffer = buffer};
    ahci_send_command(disk, port, &req);
    struct ata_identify *ident = (struct ata_identify *) buffer;
    ata_ident_print(ident);
    kfree_aligned(buffer);
}

static void ahci_on_bio_complete(struct ahci_request *req) {
    struct bio_request *bio = (struct bio_request *) req->user_data;

    bio->done = true;
    bio->status = req->status;

    if (bio->on_complete)
        bio->on_complete(bio);

    kfree(req);
}

bool ahci_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio) {
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_request *ahci_req = kmalloc(sizeof(struct ahci_request));
    if (!ahci_req)
        return false;

    struct ahci_full_port *p = &ahci_disk->device->regs[ahci_disk->port];

    ahci_req->port = ahci_disk->port;
    ahci_req->slot = ahci_find_slot(p->port);
    ahci_req->lba = bio->lba;
    ahci_req->buffer = bio->buffer;
    ahci_req->sector_count = bio->sector_count;
    ahci_req->size = bio->size;
    ahci_req->write = bio->write;
    ahci_req->done = false;

    ahci_req->on_complete = ahci_on_bio_complete;
    ahci_req->user_data = bio;

    if (bio->write) {
        return ahci_write_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                               bio->sector_count, ahci_req);
    } else {
        return ahci_read_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                              bio->sector_count, ahci_req);
    }
}
