#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

#define MAX_PRDT_ENTRY_SIZE (4 * 1024 * 1024) // 4MB

void ahci_process_completions(struct ahci_device *dev, uint32_t port) {
    struct ahci_port *p = dev->regs[port].port;
    uint32_t completed = ~(p->ci | p->sact) & 0xFFFFFFFF;

    for (uint64_t slot = 0; slot < 32; slot++) {
        if (completed & (1ULL << slot)) {
            struct thread *t = dev->io_waiters[port][slot];
            if (t) {
                dev->io_statuses[port][slot] = 0;
                t->state = READY;
                t->mlfq_level = 0;
                t->time_in_level = 0;
                uint64_t core = t->curr_core;
                scheduler_add_thread(local_schs[core], t, false, false, true);
                lapic_send_ipi(core, SCHEDULER_ID);
            }
        }
    }

    p->is = p->is;
}

void ahci_isr_handler(void *ctx, uint8_t vector, void *rsp) {
    struct ahci_device *dev = ctx;
    for (uint32_t port = 0; port < AHCI_MAX_PORTS; port++) {
        if (!dev->regs[port].port)
            continue;

        if (dev->regs[port].port->is & dev->regs[port].port->ie) {
            ahci_process_completions(dev, port);
            dev->regs[port].port->is =
                dev->regs[port].port->is; // Clear interrupt
        }
    }
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

uint32_t find_free_cmd_slot(struct ahci_port *port) {
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

bool ahci_send_command(struct ahci_disk *disk, struct ahci_full_port *port,
                       uint32_t slot) {
    struct thread *curr = scheduler_get_curr_thread();
    struct ahci_device *dev = disk->device;

    mmio_write_32(&port->port->is, 0xFFFFFFFF);

    uint32_t ci = mmio_read_32(&port->port->ci);
    ci |= (1 << slot);
    mmio_write_32(&port->port->ci, ci);

    curr->state = BLOCKED;
    dev->io_waiters[disk->port][slot] = curr;
    dev->io_statuses[disk->port][slot] = 0xFFFF; // In-flight

    scheduler_yield();

    dev->io_waiters[disk->port][slot] = NULL;

    uint32_t tfd = mmio_read_32(&port->port->tfd);
    if (tfd & (1 << 0) || tfd & (1 << 1)) {
        return false;
    }

    return true;
}

void ahci_identify(struct ahci_disk *disk) {
    struct ahci_full_port *port = &disk->device->regs[disk->port];
    uint32_t slot = find_free_cmd_slot(port->port);

    uint8_t *buffer = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!buffer)
        return;

    ahci_prepare_command(port, slot, false, buffer, PAGE_SIZE);

    if (!buffer)
        return;

    ahci_setup_fis(port->cmd_tables[slot], AHCI_CMD_IDENTIFY, false);

    if (ahci_send_command(disk, port, slot)) {
        struct ata_identify *ident = (struct ata_identify *) buffer;
        ata_ident_print(ident);
    } else {
        k_printf("AHCI IDENTIFY failed\n");
    }
    kfree_aligned(buffer);
}
