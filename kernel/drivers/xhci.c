#include <drivers/usb.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    return vmm_map_phys(phys_addr, size, PAGING_UNCACHABLE);
}

static struct xhci_device *xhci_device_create(void *mmio) {
    struct xhci_device *dev = kmalloc(sizeof(struct xhci_device));
    if (unlikely(!dev))
        k_panic("Could not allocate space for XHCI device");

    struct xhci_cap_regs *cap = mmio;
    struct xhci_op_regs *op = mmio + cap->cap_length;
    void *runtime_regs = (void *) mmio + cap->rtsoff;
    struct xhci_intr_regs *ir_base = (void *) ((uint8_t *) runtime_regs + 0x20);

    dev->port_regs = op->regs;
    dev->intr_regs = ir_base;
    dev->cap_regs = cap;
    dev->op_regs = op;
    dev->ports = cap->hcs_params1 & 0xff;

    return dev;
}

static bool xhci_controller_stop(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop = 0;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while ((mmio_read_32(&op->usbsts) & 1) == 0 && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

static bool xhci_controller_reset(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.host_controller_reset = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw); // Reset
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;

    while (mmio_read_32(&op->usbcmd) & (1 << 1) && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

static bool xhci_controller_start(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while (mmio_read_32(&op->usbsts) & 1 && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }

    return true;
}

static void xhci_controller_enable_ints(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;
    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.interrupter_enable = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
}

static void xhci_setup_event_ring(struct xhci_device *dev) {
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

static void xhci_setup_command_ring(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    uint64_t cmd_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *cmd_ring =
        vmm_map_phys(cmd_ring_phys, sizeof(struct xhci_trb) * TRB_RING_SIZE,
                     PAGING_UNCACHABLE);
    memset(cmd_ring, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);

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

static void xhci_ring_doorbell(struct xhci_device *dev, uint64_t idx) {
    uint32_t *doorbell = (void *) dev->cap_regs + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[idx], 0);
}

static void xhci_send_command(struct xhci_device *dev, uint64_t parameter,
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

    xhci_ring_doorbell(dev, 0);
}

static uint64_t xhci_wait_for_response(struct xhci_device *dev) {
    struct xhci_ring *event_ring = dev->event_ring;
    uint32_t dq_idx = event_ring->dequeue_index;
    uint8_t expected_cycle = event_ring->cycle;

    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = evt->control;
        if ((control & 1) != expected_cycle) {
            continue;
        }

        uint8_t trb_type = (control >> 10) & 0x3F;
        if (trb_type == TRB_TYPE_COMMAND_COMPLETION) {

            dq_idx++;
            if (dq_idx == event_ring->size) {
                dq_idx = 0;
                expected_cycle ^= 1;
            }
            event_ring->dequeue_index = dq_idx;
            event_ring->cycle = expected_cycle;

            uint64_t offset = dq_idx * sizeof(struct xhci_trb);
            uint64_t erdp = event_ring->phys + offset;
            mmio_write_64(&dev->intr_regs->erdp, erdp | 1);

            return control;
        }

        dq_idx++;
        if (dq_idx == event_ring->size) {
            dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = dq_idx;
        event_ring->cycle = expected_cycle;
    }
}

static uint8_t xhci_enable_slot(struct xhci_device *dev) {
    xhci_send_command(
        dev, 0, (TRB_TYPE_ENABLE_SLOT << 10) | (dev->cmd_ring->cycle & 1));

    return (xhci_wait_for_response(dev) >> 24) & 0xff;
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
bool xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                         uint8_t speed, uint8_t port) {
    uint64_t input_ctx_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_input_ctx *input_ctx =
        vmm_map_phys(input_ctx_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(input_ctx, 0, PAGE_SIZE);

    input_ctx->ctrl_ctx.add_flags = (1 << 0) | (1 << 1); // slot + ep0
    input_ctx->ctrl_ctx.drop_flags = 0;

    struct xhci_slot_ctx *slot = &input_ctx->slot_ctx;
    slot->route_string = 0;
    slot->speed = speed;
    slot->context_entries = 1;
    slot->root_hub_port = port;
    slot->mtt = 0;
    slot->hub = 0;
    slot->num_ports = 0;

    uint64_t ep0_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *ep0_ring =
        vmm_map_phys(ep0_ring_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(ep0_ring, 0, PAGE_SIZE);
    ep0_ring[TRB_RING_SIZE - 1].parameter = ep0_ring_phys;
    ep0_ring[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    struct xhci_ep_ctx *ep0 = &input_ctx->ep0_ctx;
    ep0->ep_type = 4; /* Control */
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dcs = 1;
    ep0->dequeue_ptr_raw = ep0_ring_phys;

    uint64_t dev_ctx_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_device_ctx *dev_ctx =
        vmm_map_phys(dev_ctx_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(dev_ctx, 0, PAGE_SIZE);

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    xhci_send_command(ctrl, input_ctx_phys,
                      (TRB_TYPE_ADDRESS_DEVICE << 10) |
                          (ctrl->cmd_ring->cycle & 1) | (slot_id << 24));

    if (!(xhci_wait_for_response(ctrl) & (1 << 0))) {
        xhci_warn("Address device failed for slot %u, port %u", slot_id, port);
        return false;
    }

    xhci_info("Address device completed for slot %u, port %u", slot_id, port);
    return true;
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func) {
    xhci_info("Found device at %02x:%02x.%02x", bus, slot, func);
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);

    if (!xhci_controller_stop(dev))
        return;

    if (!xhci_controller_reset(dev))
        return;

    xhci_parse_ext_caps(dev);
    xhci_setup_event_ring(dev);

    xhci_setup_command_ring(dev);

    xhci_controller_start(dev);
    xhci_controller_enable_ints(dev);

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = mmio_read_32(&dev->port_regs[port - 1]);

        if (portsc & PORTSC_CCS) {
            uint8_t speed = portsc & 0xF;

            xhci_reset_port(dev, port);
            uint8_t slot_id = xhci_enable_slot(dev);
            if (slot_id == 0) {
                xhci_warn("Failed to enable slot for port %lu\n", port);
                continue;
            }

            dev->port_info[port - 1].device_connected = true;
            dev->port_info[port - 1].speed = speed;
            dev->port_info[port - 1].slot_id = slot_id;
            xhci_address_device(dev, slot_id, speed, port);
        }
    }

    xhci_info("Device initialized successfully");
}
static void xhci_pci_init(uint8_t bus, uint8_t slot, uint8_t func,
                          struct pci_device *dev) {
    switch (dev->prog_if) {
    case 0x30: xhci_init(bus, slot, func);
    default: break;
    }
}

REGISTER_PCI_DEV(xhci, 0x0C, 0x03, 0x030, 0xFFFF, xhci_pci_init)
