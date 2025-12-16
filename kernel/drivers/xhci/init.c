#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb_generic/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

void xhci_setup_event_ring(struct xhci_device *dev) {
    struct xhci_erst_entry *erst =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    paddr_t erst_phys = vmm_get_phys((vaddr_t) erst, VMM_FLAG_NONE);

    dev->event_ring = xhci_allocate_event_ring();
    erst[0].ring_segment_base = dev->event_ring->phys;
    erst[0].ring_segment_size = dev->event_ring->size;
    erst[0].reserved = 0;

    struct xhci_interrupter_regs *ir = dev->intr_regs;

    mmio_write_32(&ir->imod, 0);
    mmio_write_32(&ir->erstsz, 1);
    mmio_write_64(&ir->erstba, erst_phys);

    mmio_write_64(&ir->erdp, dev->event_ring->phys | XHCI_ERDP_EHB_BIT);
}

void xhci_setup_command_ring(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;
    dev->cmd_ring = xhci_allocate_ring();
    uintptr_t trb_phys = dev->cmd_ring->phys;

    struct xhci_dcbaa *dcbaa_virt =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t dcbaa_phys = vmm_get_phys((uintptr_t) dcbaa_virt, VMM_FLAG_NONE);

    dev->dcbaa = dcbaa_virt;
    mmio_write_64(&op->crcr, trb_phys | 1);
    mmio_write_64(&op->dcbaap, dcbaa_phys | 1);
}

uint8_t xhci_enable_slot(struct xhci_device *dev) {
    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = 0,
        .control = TRB_SET_TYPE(TRB_TYPE_ENABLE_SLOT) |
                   TRB_SET_CYCLE(dev->cmd_ring->cycle),
        .status = 0,
    };

    cmd = (struct xhci_command) {
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .ep_id = 0,
        .slot = NULL,
        .ring = dev->cmd_ring,
        .request = &request,
        .num_trbs = 1,
    };

    xhci_send_command_and_block(dev, &cmd);

    return TRB_SLOT(request.return_control);
}

void xhci_disable_slot(struct xhci_device *dev, struct xhci_slot *slot) {
    struct xhci_request request;
    struct xhci_command cmd;

    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = 0,
        .status = 0,
        .control = TRB_SET_TYPE(TRB_TYPE_DISABLE_SLOT) |
                   TRB_SET_CYCLE(dev->cmd_ring->cycle) |
                   TRB_SET_SLOT_ID(slot->slot_id),
    };

    cmd = (struct xhci_command) {
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .ep_id = 0,
        .slot = NULL,
        .ring = dev->cmd_ring,
        .request = &request,
        .num_trbs = 1,
    };

    xhci_send_command_and_block(dev, &cmd);
}

bool xhci_reset_port(struct xhci_device *dev, uint32_t portnum) {
    uint32_t *portsc = xhci_portsc_ptr(dev, portnum);
    bool is_usb3 = dev->port_info[portnum - 1].usb3;

    uint32_t old = mmio_read_32(portsc);

    /* Power on the port */
    if (!(old & PORTSC_PP)) {
        /* Only write the PP bit */
        mmio_write_32(portsc, PORTSC_PP);
        sleep_us(2000);
        old = mmio_read_32(portsc);
        if (!(old & PORTSC_PP)) {
            xhci_warn("port %u: power enable failed", portnum);
            return false;
        }
    }

    uint32_t cur = mmio_read_32(portsc);

    uint32_t clear_mask = PORTSC_PRC | PORTSC_CSC | PORTSC_PEC | PORTSC_WRC;

    uint32_t preserve =
        cur & (PORTSC_PP | PORTSC_PED | (PORTSC_PLS_MASK << PORTSC_PLS_SHIFT));

    uint32_t to_write = clear_mask | preserve;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum irql lirql = spin_lock_irq_disable(&dev->lock);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);
    list_add_tail(&request.list, &dev->requests[XHCI_REQUEST_OUTGOING]);

    mmio_write_32(portsc, to_write);

    sleep_us(500);
    old = mmio_read_32(portsc);

    uint32_t write_mask = PORTSC_PR;

    if (old & PORTSC_PED) {
        write_mask |= PORTSC_PED;
    }

    if (is_usb3) {
        write_mask |= PORTSC_WPR;
    }

    mmio_write_32(portsc, write_mask);

    thread_block(scheduler_get_current_thread(), THREAD_BLOCK_REASON_IO,
                 THREAD_WAIT_UNINTERRUPTIBLE, dev);

    spin_unlock(&dev->lock, lirql);
    irql_lower(irql);

    thread_wait_for_wake_match();

    time_t timeout_ms = 100;
    bool settled = false;

    while (timeout_ms-- > 0) {
        uint32_t cur = mmio_read_32(portsc);
        if (is_usb3) {
            if (!(cur & PORTSC_PR)) {
                settled = true;
                break;
            }
        } else {
            if (cur & PORTSC_PED) {
                settled = true;
                break;
            }
        }
        sleep_us(1000); /* 1ms */
    }

    if (!settled) {
        xhci_warn(
            "port %u: reset timed out (PR did not clear or PED did not change)",
            portnum);
        return false;
    }

    if (!xhci_request_ok(&request)) {
        xhci_warn("port %u reset did not generate a PSC event", portnum);
        return false;
    }

    uint32_t final = mmio_read_32(portsc);
    uint32_t pls = (final >> PORTSC_PLS_SHIFT) & PORTSC_PLS_MASK;
    if (is_usb3) {
        if (pls == PORTSC_PLS_U0 && (final & PORTSC_SPEED_MASK)) {
            xhci_info("port %u: USB3 reset success (PLS=U0, speed=%u)", portnum,
                      final & PORTSC_SPEED_MASK);
        } else if (pls == PORTSC_PLS_RXDETECT) {
            xhci_info("port %u: USB3 reset failed (PLS=RXDETECT)", portnum);
            return false;
        } else {
            xhci_info("port %u: USB3 reset unknown state pls=%u speed=%u",
                      portnum, pls, final & PORTSC_SPEED_MASK);
        }
    } else {
        xhci_info("port %u: USB2 reset success", portnum);
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

void xhci_detect_usb3_ports(struct xhci_device *dev) {
    uint32_t hcc_params1 = mmio_read_32(&dev->cap_regs->hcc_params1);
    uint32_t offset = (hcc_params1 >> 16) & 0xFFFF;

    while (offset) {
        void *ext_cap_addr = (uint8_t *) dev->cap_regs + offset * 4;
        uint32_t cap_header = mmio_read_32(ext_cap_addr);

        uint8_t cap_id = cap_header & 0xFF;
        uint8_t next = (cap_header >> 8) & 0xFF;

        if (cap_id == XHCI_EXT_CAP_ID_USB) {
            uint32_t cap[4];
            for (int i = 0; i < 4; i++)
                cap[i] = mmio_read_32((uint8_t *) ext_cap_addr + i * 4);

            uint8_t portcount = (cap[2] >> 8) & 0xFF;
            uint8_t portoffset = (cap[2]) & 0xFF;
            uint8_t major = (cap[0] >> 24) & 0xFF;

            if (portoffset == 0 || portcount == 0) {
                xhci_warn("USB capability with invalid port offset/count");
            } else {
                uint8_t start = portoffset - 1;

                for (uint8_t i = start; i < start + portcount; i++) {
                    dev->port_info[i].usb3 = major >= 3;
                    xhci_info("Port %u detected as USB%u", i + 1, major);
                }
            }
        }

        offset = next;
    }
}

void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    return vmm_map_phys(phys_addr, size, PAGING_UNCACHABLE, VMM_FLAG_NONE);
}

struct xhci_device *xhci_device_create(void *mmio) {
    struct xhci_device *dev =
        kzalloc(sizeof(struct xhci_device), ALLOC_PARAMS_DEFAULT);
    if (unlikely(!dev))
        k_panic("Could not allocate space for XHCI device");

    struct xhci_cap_regs *cap = mmio;
    struct xhci_op_regs *op = mmio + cap->cap_length;
    void *runtime_regs = (void *) mmio + cap->rtsoff;
    struct xhci_interrupter_regs *ir_base =
        (void *) ((uint8_t *) runtime_regs + 0x20);

    for (size_t i = 0; i < XHCI_REQUEST_MAX; i++) {
        INIT_LIST_HEAD(&dev->requests[i]);
    }

    dev->num_devices = 0;
    dev->port_regs = op->regs;
    dev->intr_regs = ir_base;
    dev->cap_regs = cap;
    dev->op_regs = op;
    dev->ports = cap->hcs_params1 & 0xff;
    INIT_LIST_HEAD(&dev->devices);

    for (uint32_t i = 0; i < XHCI_PORT_COUNT; i++) {

        struct xhci_port *p = &dev->port_info[i];
        p->generation = 0;
        p->usb3 = false;
        p->state = XHCI_PORT_STATE_DISCONNECTED;
        uint32_t portsc = xhci_read_portsc(dev, i + 1);
        uint8_t speed = portsc & 0xF;
        p->speed = speed;
        p->port_id = (i + 1);
    }

    for (size_t i = 0; i < XHCI_SLOT_COUNT; i++) {
        struct xhci_slot *xs = &dev->slots[i];
        xs->dev = dev;
        xs->state = XHCI_SLOT_STATE_DISCONNECTED;
        xs->slot_id = (i + 1);
    }

    return dev;
}

void xhci_device_start_interrupts(uint8_t bus, uint8_t slot, uint8_t func,
                                  struct xhci_device *dev) {
    dev->irq = irq_alloc_entry();
    irq_register("xhci", dev->irq, xhci_isr, dev, IRQ_FLAG_NONE);
    pci_program_msix_entry(bus, slot, func, 0, dev->irq, /*core=*/0);
}
