#include <asm.h>
#include <console/printf.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

// TODO: timeouts... again... :p

static void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    return vmm_map_phys(phys_addr, size);
}

static struct xhci_device *xhci_device_create(void *mmio) {
    struct xhci_device *dev = kmalloc(sizeof(struct xhci_device));

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
    uint64_t timeout = XHCI_DEVICE_TIMEOUT;

    while ((mmio_read_32(&op->usbsts) & 1) == 0 && timeout--) {
        sleep_ms(1);
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
    uint64_t timeout = XHCI_DEVICE_TIMEOUT;

    while (mmio_read_32(&op->usbcmd) & (1 << 1) && timeout--) {
        sleep_ms(1);
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
    uint64_t timeout = XHCI_DEVICE_TIMEOUT;

    while (mmio_read_32(&op->usbsts) & 1 && timeout--) {
        sleep_ms(1);
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
    struct xhci_erst_entry *erst_table = vmm_map_phys(erst_table_phys, 4096);

    uint64_t event_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *event_ring = vmm_map_phys(event_ring_phys, 4096);

    event_ring[0].control = 1;
    memset(event_ring, 0, 4096);

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
    ring->phys = event_ring_phys;
    ring->cycle = 1;
    ring->size = 256;
    ring->trbs = event_ring;
    ring->enqueue_index = 0;
    dev->event_ring = ring;
}

static void xhci_setup_command_ring(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    uint64_t cmd_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *cmd_ring =
        vmm_map_phys(cmd_ring_phys, sizeof(struct xhci_trb) * TRB_RING_SIZE);
    memset(cmd_ring, 0, sizeof(struct xhci_trb) * TRB_RING_SIZE);

    int last_index = TRB_RING_SIZE - 1;
    cmd_ring[last_index].parameter = cmd_ring_phys;
    cmd_ring[last_index].status = 0;
    /* Toggle Cycle, Cycle bit = 1 */
    cmd_ring[last_index].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    mmio_write_64(&op->crcr, cmd_ring_phys | 1);

    uint64_t dcbaa_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_dcbaa *dcbaa_virt =
        vmm_map_phys(dcbaa_phys, sizeof(struct xhci_dcbaa));
    mmio_write_64(&op->dcbaap, dcbaa_phys | 1);

    struct xhci_ring *ring = kmalloc(sizeof(struct xhci_ring));
    ring->phys = cmd_ring_phys;
    ring->trbs = cmd_ring;
    ring->size = TRB_RING_SIZE;
    ring->cycle = 1;
    ring->enqueue_index = 0;

    dev->dcbaa = dcbaa_virt;
    dev->cmd_ring = ring;
}

void xhci_cmd_enable_slot(struct xhci_device *dev) {
    struct xhci_ring *ring = dev->cmd_ring;
    struct xhci_trb *trb = &ring->trbs[ring->enqueue_index++];

    trb->parameter = 0;
    trb->status = 0;
    trb->control = (TRB_TYPE_ENABLE_SLOT << 10) | ring->cycle;
}

static void xhci_ring_doorbell(struct xhci_device *dev, uint64_t idx) {
    uint32_t *doorbell = (void *) dev->cap_regs + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[idx], 0);
}

static void xhci_address_device(struct xhci_device *dev, uint64_t slot_id) {
    struct xhci_device_ctx *device_ctx;

    uint64_t input_ctx_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_input_ctx *input_ctx = vmm_map_phys(input_ctx_phys, 4096);
    memset(input_ctx, 0, 4096);

    input_ctx->ctrl_ctx.add_flags = (1 << 0) | (1 << 1);

    struct xhci_slot_ctx *slot = &input_ctx->slot_ctx;
    slot->context_entries = 1;
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func) {
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);

    if (!xhci_controller_stop(dev))
        k_printf("Could stop XHCI controller\n");

    if (!xhci_controller_reset(dev))
        k_printf("Could not reset XHCI controller\n");

    xhci_setup_event_ring(dev);

    xhci_setup_command_ring(dev);
    xhci_cmd_enable_slot(dev);

    xhci_controller_enable_ints(dev);

    xhci_controller_start(dev);

    xhci_ring_doorbell(dev, 0);

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = mmio_read_32(&dev->port_regs[port - 1].portsc);
        if (portsc & PORTSC_CCS) {
            uint8_t speed_bits = portsc & 0xF;
            switch (speed_bits) {
            case 1: k_printf("lowspeed\n"); break;
            case 2: k_printf("fullspeed\n"); break;
            case 3: k_printf("highspeed\n"); break;
            case 4: k_printf("superspeed\n"); break;
            case 5: k_printf("superspeedplus\n"); break;
            default: k_printf("unknown speed\n"); break;
            }
            continue;
        }
    }

    uint8_t expected_cycle = 1;
    struct xhci_ring *event_ring = dev->event_ring;
    uint32_t dq_idx = event_ring->enqueue_index;
    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & 1) != expected_cycle)
            continue;

        uint8_t trb_type = (control >> 10) & 0x3F;
        if (trb_type == TRB_TYPE_COMMAND_COMPLETION) {
            uint8_t slot_id = evt->control >> 24;
            uint8_t code = (evt->status >> 24) & 0xFF;
            k_printf("Enable Slot complete, code=%u, slot=%u\n", code, slot_id);

            uint64_t offset = dq_idx * sizeof(struct xhci_trb);
            uint64_t erdp = event_ring->phys + offset;
            mmio_write_64(&dev->intr_regs->erdp, erdp | 1);
            dq_idx++;
            if (dq_idx == event_ring->size) {
                dq_idx = 1;
                expected_cycle ^= 1;
            }
            event_ring->enqueue_index = dq_idx;

            break;
        }
        dq_idx++;
        if (dq_idx == event_ring->size) {
            dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->enqueue_index = dq_idx;
    }

    k_printf("XHCI controller initialized.\n");
}
