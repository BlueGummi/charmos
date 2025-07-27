#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void xhci_setup_event_ring(struct xhci_device *dev) {
    uint64_t erst_table_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_erst_entry *erst_table =
        vmm_map_phys(erst_table_phys, PAGE_SIZE, PAGING_UNCACHABLE);

    uint64_t event_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *event_ring =
        vmm_map_phys(event_ring_phys, PAGE_SIZE, PAGING_UNCACHABLE);

    event_ring[0].control = 1;
    memset(event_ring, 0, PAGE_SIZE);

    erst_table[0].ring_segment_base = event_ring_phys;
    erst_table[0].ring_segment_size = 256;
    erst_table[0].reserved = 0;

    struct xhci_intr_regs *ir = dev->intr_regs;
    struct xhci_erdp erdp;
    erdp.raw = event_ring_phys;
    erdp.desi = 1;

    mmio_write_32(&ir->iman, 1 << 1);
    mmio_write_32(&ir->erstsz, 1);
    mmio_write_64(&ir->erstba, erst_table_phys);
    mmio_write_64(&ir->erdp, erdp.raw);

    struct xhci_ring *ring = kmalloc(sizeof(struct xhci_ring));
    if (unlikely(!ring))
        k_panic("Could not allocate space for XHCI ring\n");

    ring->phys = event_ring_phys;
    ring->cycle = 1;
    ring->size = 256;
    ring->trbs = event_ring;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;
    dev->event_ring = ring;
}

void xhci_setup_command_ring(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_trb *cmd_ring = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t cmd_ring_phys = vmm_get_phys((uintptr_t) cmd_ring);

    int last_index = TRB_RING_SIZE - 1;
    cmd_ring[last_index].parameter = cmd_ring_phys;
    cmd_ring[last_index].status = 0;
    /* Toggle Cycle, Cycle bit = 1 */
    cmd_ring[last_index].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    uint64_t dcbaa_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_dcbaa *dcbaa_virt =
        vmm_map_phys(dcbaa_phys, sizeof(struct xhci_dcbaa), PAGING_UNCACHABLE);

    struct xhci_ring *ring = kmalloc(sizeof(struct xhci_ring));
    if (unlikely(!ring))
        k_panic("Could not allocate space for XHCI ring\n");

    ring->phys = cmd_ring_phys;
    ring->trbs = cmd_ring;
    ring->size = TRB_RING_SIZE;
    ring->cycle = 1;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    dev->dcbaa = dcbaa_virt;
    dev->cmd_ring = ring;
    mmio_write_64(&op->crcr, cmd_ring_phys | 1);
    mmio_write_64(&op->dcbaap, dcbaa_phys | 1);
}

void xhci_ring_doorbell(struct xhci_device *dev, uint32_t slot_id,
                        uint32_t ep_id) {
    uint32_t *doorbell = (void *) dev->cap_regs + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[slot_id], ep_id);
}

void xhci_send_command(struct xhci_device *dev, uint64_t parameter,
                       uint32_t control) {
    struct xhci_ring *cmd_ring = dev->cmd_ring;

    struct xhci_trb *trb = &cmd_ring->trbs[cmd_ring->enqueue_index];

    trb->parameter = parameter;
    trb->status = 0;
    trb->control = control;
    cmd_ring->enqueue_index++;
    if (cmd_ring->enqueue_index == cmd_ring->size) {
        cmd_ring->enqueue_index = 0;
        cmd_ring->cycle ^= 1;
    }

    xhci_ring_doorbell(dev, 0, 0);
}

typedef uint64_t (*xhci_wait_cb)(struct xhci_device *, struct xhci_trb *evt,
                                 uint8_t slot_id, uint32_t *dq_idx,
                                 struct xhci_ring *evt_ring,
                                 uint8_t expected_cycle, bool *success_out);

static uint64_t xhci_generic_wait(struct xhci_device *dev, uint8_t slot_id,
                                  xhci_wait_cb callback) {
    struct xhci_ring *event_ring = dev->event_ring;
    uint32_t dq_idx = event_ring->dequeue_index;
    uint8_t expected_cycle = event_ring->cycle;

    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = mmio_read_32(&evt->control);
        if ((control & 1) != expected_cycle) {
            continue;
        }

        bool success = false;
        uint64_t ret = callback(dev, evt, slot_id, &dq_idx, event_ring,
                                expected_cycle, &success);
        if (success)
            return ret;

        dq_idx++;
        if (dq_idx == event_ring->size) {
            dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = dq_idx;
        event_ring->cycle = expected_cycle;
    }
}

static uint64_t wait_for_response(struct xhci_device *dev, struct xhci_trb *evt,
                                  uint8_t slot_id, uint32_t *dq_idx,
                                  struct xhci_ring *event_ring,
                                  uint8_t expected_cycle, bool *success_out) {
    (void) slot_id;
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = (control >> 10) & 0x3F;
    if (trb_type == TRB_TYPE_COMMAND_COMPLETION) {

        (*dq_idx)++;
        if (*dq_idx == event_ring->size) {
            *dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = *dq_idx;
        event_ring->cycle = expected_cycle;

        uint64_t offset = *dq_idx * sizeof(struct xhci_trb);
        uint64_t erdp = event_ring->phys + offset;
        mmio_write_64(&dev->intr_regs->erdp, erdp | 1);

        *success_out = true;
        return control;
    } else {
        *success_out = false;
        return 0;
    }
}

static uint64_t wait_for_transfer_event(struct xhci_device *dev,
                                        struct xhci_trb *evt, uint8_t slot_id,
                                        uint32_t *dq_idx,
                                        struct xhci_ring *event_ring,
                                        uint8_t expected_cycle,
                                        bool *success_out) {
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = (control >> 10) & 0x3F;
    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
        uint8_t completion_code = (evt->status >> 24) & 0xFF;
        uint8_t evt_slot_id = (control >> 24) & 0xFF;

        if (evt_slot_id != slot_id) {
            (*dq_idx)++;
            if (*dq_idx == event_ring->size) {
                *dq_idx = 0;
                expected_cycle ^= 1;
            }
            *success_out = false;
        }

        (*dq_idx)++;
        if (*dq_idx == event_ring->size) {
            *dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = *dq_idx;
        event_ring->cycle = expected_cycle;

        uint64_t offset = *dq_idx * sizeof(struct xhci_trb);
        uint64_t erdp = event_ring->phys + offset;
        mmio_write_64(&dev->intr_regs->erdp, erdp | 1);

        *success_out = true;
        return (completion_code == 1);
    } else {
        *success_out = false;
        return 0;
    }
}

uint64_t xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_generic_wait(dev, 0, wait_for_response);
}

bool xhci_wait_for_transfer_event(struct xhci_device *dev, uint8_t slot_id) {
    return (bool) xhci_generic_wait(dev, slot_id, wait_for_transfer_event);
}

uint8_t xhci_enable_slot(struct xhci_device *dev) {
    xhci_send_command(
        dev, 0, (TRB_TYPE_ENABLE_SLOT << 10) | (dev->cmd_ring->cycle & 1));

    return (xhci_wait_for_response(dev) >> 24) & 0xff;
}

bool xhci_reset_port(struct xhci_device *dev, uint32_t port_index) {
    uint32_t *portsc = (void *) &dev->port_regs[port_index];

    uint32_t val = mmio_read_32(portsc);
    val |= (1 << 4); // Port Reset
    mmio_write_32(portsc, val);

    uint64_t timeout = 100 * 1000;
    while ((mmio_read_32(portsc) & (1 << 4)) && timeout--) {
        sleep_us(1);
    }

    if (mmio_read_32(portsc) & (1 << 4)) {
        xhci_warn("Port %u reset timed out", port_index + 1);
        return false;
    }

    return true;
}

void xhci_parse_ext_caps(struct xhci_device *dev) {
    uint32_t hcc_params1 = mmio_read_32(&dev->cap_regs->hcc_params1);
    uint32_t offset = (hcc_params1 >> 16) & 0xFFFF;

    while (offset) {
        void *ext_cap_addr = (uint8_t *) dev->cap_regs + offset * 4;
        uint32_t cap_header = mmio_read_32(ext_cap_addr);

        uint8_t cap_id = cap_header & 0xFF;
        uint8_t next = (cap_header >> 8) & 0xFF;

        if (cap_id != XHCI_EXT_CAP_ID_LEGACY_SUPPORT) {
            offset = next;
            continue;
        }

        void *bios_owns_addr = (uint8_t *) ext_cap_addr + 4;
        void *os_owns_addr = (uint8_t *) ext_cap_addr + 8;

        mmio_write_32(os_owns_addr, 1);

        uint64_t timeout = 1000 * 1000;
        while ((mmio_read_32(bios_owns_addr) & 1) && timeout--) {
            sleep_us(1);
        }

        uint32_t own_data = mmio_read_32(bios_owns_addr);
        if (own_data & 1) {
            xhci_warn("BIOS ownership handoff failed");
        } else {
            xhci_info("BIOS ownership handoff completed");
        }

        break;
    }
}
