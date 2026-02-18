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
    mmio_write_64(&ir->erdp, dev->event_ring->phys);
    mmio_write_64(&ir->erstba, erst_phys);
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

void xhci_nop(struct xhci_device *dev) {
    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = 0,
        .control =
            TRB_SET_TYPE(TRB_TYPE_NO_OP) | TRB_SET_CYCLE(dev->cmd_ring->cycle),
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

    xhci_send_command_and_block(dev, &cmd, NULL);
}

uint8_t xhci_enable_slot(struct xhci_device *dev) {
    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
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

    xhci_send_command_and_block(dev, &cmd, NULL);

    return TRB_SLOT(request.return_control);
}

void xhci_disable_slot(struct xhci_device *dev, uint8_t slot_id) {
    struct xhci_request request = {0};
    struct xhci_command cmd = {0};

    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = 0,
        .status = 0,
        .control = TRB_SET_TYPE(TRB_TYPE_DISABLE_SLOT) |
                   TRB_SET_CYCLE(dev->cmd_ring->cycle) |
                   TRB_SET_SLOT_ID(slot_id),
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

    xhci_send_command_and_block(dev, &cmd, NULL);
}

static enum usb_status xhci_spin_wait_port_reset(uint32_t *portsc,
                                                 bool is_usb3) {
    const uint64_t timeout_us = 100 * 1000;
    uint64_t start = time_get_us();

    while (time_get_us() - start < timeout_us) {
        uint32_t v = mmio_read_32(portsc);

        if (is_usb3) {
            if (!(v & PORTSC_WPR)) {
                uint32_t pls = (v >> PORTSC_PLS_SHIFT) & PORTSC_PLS_MASK;
                if (pls == PORTSC_PLS_RXDETECT)
                    return USB_ERR_IO;
                if (pls == PORTSC_PLS_U0)
                    return USB_OK;
            }
        } else {
            if (!(v & PORTSC_PR)) {
                if (v & PORTSC_PED)
                    return USB_OK;
                return USB_ERR_IO;
            }
        }

        cpu_relax();
        sleep_us(10);
    }

    return USB_ERR_TIMEOUT;
}

enum usb_status xhci_reset_port(struct xhci_device *dev, uint32_t portnum) {
    uint32_t *portsc = xhci_portsc_ptr(dev, portnum);
    bool is_usb3 = dev->port_info[portnum - 1].usb3;

    uint32_t v = mmio_read_32(portsc);

    /* Power on */
    if (!(v & PORTSC_PP)) {
        mmio_write_32(portsc, v | PORTSC_PP);
        sleep_us(2000);
        v = mmio_read_32(portsc);
        if (!(v & PORTSC_PP))
            return USB_ERR_IO;
    }

    /* Clear change bits */
    mmio_write_32(portsc,
                  v | PORTSC_CSC | PORTSC_PEC | PORTSC_PRC | PORTSC_WRC);

    sleep_us(100);

    /* Initiate reset */
    if (is_usb3) {
        mmio_write_32(portsc, PORTSC_WPR);
    } else {
        mmio_write_32(portsc, PORTSC_PR);
    }

    /* Spin-poll completion */
    enum usb_status st = xhci_spin_wait_port_reset(portsc, is_usb3);

    if (st != USB_OK)
        return st;

    /* Final sanity check */
    v = mmio_read_32(portsc);
    if (!(v & PORTSC_PED))
        return USB_ERR_IO;

    return USB_OK;
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

    for (size_t i = 0; i < XHCI_REQ_LIST_MAX; i++) {
        INIT_LIST_HEAD(&dev->requests[i]);
    }

    dev->num_devices = 0;
    dev->port_regs = op->regs;
    dev->intr_regs = ir_base;
    dev->cap_regs = cap;
    dev->op_regs = op;
    dev->ports = cap->hcs_params1 & 0xff;
    INIT_LIST_HEAD(&dev->devices);
    semaphore_init(&dev->port_disconnect, 0, SEMAPHORE_INIT_IRQ_DISABLE);
    semaphore_init(&dev->port_connect, 0, SEMAPHORE_INIT_IRQ_DISABLE);
    semaphore_init(&dev->sem, 0, SEMAPHORE_INIT_IRQ_DISABLE);

    for (uint32_t i = 0; i < XHCI_PORT_COUNT; i++) {
        struct xhci_port *p = &dev->port_info[i];
        p->generation = 0;
        p->usb3 = false;
        p->state = XHCI_PORT_STATE_DISCONNECTED;
        uint32_t portsc = xhci_read_portsc(dev, i + 1);
        uint8_t speed = portsc & 0xF;
        p->speed = speed;
        p->dev = dev;
        p->port_id = (i + 1);
        spinlock_init(&p->update_lock);
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
    irq_set_chip(dev->irq, lapic_get_chip(), NULL);
    pci_program_msix_entry(bus, slot, func, 0, dev->irq, /*core=*/0);
}

enum usb_status xhci_port_init(struct xhci_port *p) {
    struct xhci_device *dev = p->dev;
    uint8_t port = p->port_id;
    enum usb_status err = USB_OK;
    uint8_t slot_id;
    struct usb_device *usb;
    if (!(usb = kzalloc(sizeof(struct usb_device), ALLOC_PARAMS_DEFAULT))) {
        return USB_ERR_OOM;
    }

    k_log("reset_port sent\n");
    if ((err = xhci_reset_port(dev, port)) != USB_OK) {
        k_log("reset_port fail\n");
        return err;
    }

    k_log("reset_port returned\n");

    k_log("enable_slot sent\n");
    if ((slot_id = xhci_enable_slot(dev)) == 0) {
        k_log("enable_slot fail\n");
        return USB_ERR_NO_DEVICE;
    }
    k_log("enable_slot returned\n");

    struct xhci_slot temp_slot = {0};
    temp_slot.state = XHCI_SLOT_STATE_ENABLED;
    temp_slot.slot_id = slot_id;
    temp_slot.dev = dev;

    k_log("address_device sent\n");
    if ((err = xhci_address_device(p, slot_id, &temp_slot)) != USB_OK) {
        k_log("address_device fail\n");
        k_log("disable_slot sent\n");
        xhci_disable_slot(dev, slot_id);
        k_log("disable_slot returned\n");
        return err;
    }
    k_log("address_device returned\n");

    usb->speed = p->speed;
    usb->port = port;
    usb->configured = false;
    usb->host = dev->controller;
    usb->driver_private = dev;

    refcount_init(&usb->refcount, 1);
    INIT_LIST_HEAD(&usb->hc_list);

    struct xhci_slot *this_slot = xhci_get_slot(dev, slot_id);
    memcpy(this_slot, &temp_slot, sizeof(struct xhci_slot));

    xhci_port_set_state(p, XHCI_PORT_STATE_CONNECTED);
    refcount_init(&this_slot->refcount, 1);
    p->slot = this_slot;
    this_slot->port = p;
    this_slot->udev = usb;

    list_add_tail(&usb->hc_list, &dev->devices);
    dev->num_devices++;
    usb->slot = this_slot;

    return err;
}
