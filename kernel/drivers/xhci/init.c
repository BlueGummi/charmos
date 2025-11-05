#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

void xhci_setup_event_ring(struct xhci_device *dev) {
    struct xhci_erst_entry *erst_table = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t erst_table_phys = vmm_get_phys((uintptr_t) erst_table);

    struct xhci_trb *event_ring = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t event_ring_phys = vmm_get_phys((uintptr_t) event_ring);

    event_ring[0].control = 1;

    erst_table[0].ring_segment_base = event_ring_phys;
    erst_table[0].ring_segment_size = 256;
    erst_table[0].reserved = 0;

    struct xhci_interrupter_regs *ir = dev->intr_regs;
    struct xhci_erdp erdp;
    erdp.raw = event_ring_phys;

    mmio_write_32(&ir->imod, 0);
    mmio_write_32(&ir->erstsz, 1);
    mmio_write_64(&ir->erstba, erst_table_phys);
    mmio_write_64(&ir->erdp, erdp.raw);

    struct xhci_ring *ring = kzalloc(sizeof(struct xhci_ring));
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
    cmd_ring[last_index].control = TRB_SET_TYPE(TRB_TYPE_LINK);
    cmd_ring[last_index].control |= 1 << 1;

    struct xhci_dcbaa *dcbaa_virt = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t dcbaa_phys = vmm_get_phys((uintptr_t) dcbaa_virt);

    struct xhci_ring *ring = kzalloc(sizeof(struct xhci_ring));
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

uint8_t xhci_enable_slot(struct xhci_device *dev) {
    xhci_send_command(dev, 0,
                      TRB_SET_TYPE(TRB_TYPE_ENABLE_SLOT) |
                          TRB_SET_CYCLE(dev->cmd_ring->cycle));

    return (xhci_wait_for_response(dev) >> 24) & 0xff;
}

bool xhci_consume_port_status_change(struct xhci_device *dev) {
    struct xhci_ring *event_ring = dev->event_ring;
    struct xhci_interrupter_regs *intr = dev->intr_regs;

    uint32_t dq_idx = event_ring->dequeue_index;
    uint8_t expected_cycle = event_ring->cycle;

    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & TRB_CYCLE_BIT) != expected_cycle)
            break;

        uint8_t trb_type = TRB_GET_TYPE(control);
        if (trb_type == TRB_TYPE_PORT_STATUS_CHANGE) {
            uint32_t dw0 = (uint32_t) mmio_read_32(&evt->parameter);
            uint8_t port_id = (dw0 >> 24) & 0xFF;

            bool bad_port_id = port_id == 0 || port_id > dev->ports;

            if (bad_port_id)
                xhci_warn("Unexpected port id %u in PSC event", port_id);

            uint32_t *portsc = (void *) &dev->port_regs[port_id];
            uint32_t pval = mmio_read_32(portsc);
            mmio_write_32(portsc, pval | (1U << 21)); /* clear PRC */

            /* Advance the dequeue and program ERDP */
            xhci_advance_dequeue(event_ring, &dq_idx, &expected_cycle);
            uint64_t offset = (uint64_t) dq_idx * sizeof(struct xhci_trb);
            mmio_write_64(&intr->erdp, event_ring->phys + offset | 1ULL);

            /* commit back to software state */
            event_ring->dequeue_index = dq_idx;
            event_ring->cycle = expected_cycle;

            return true;
        }

        /* Not PSC, just advance */
        xhci_advance_dequeue(event_ring, &dq_idx, &expected_cycle);
    }

    /* No PSC found */
    return false;
}

bool xhci_reset_port(struct xhci_device *dev, uint32_t portnum) {
    uint32_t *portsc = (uint32_t *) &dev->port_regs[portnum];
    uint32_t val = mmio_read_32(portsc);
    bool is_usb3 = dev->port_info[portnum].usb3;

    /* Power */
    if (!(val & PORTSC_PP)) {
        val |= PORTSC_PP;
        mmio_write_32(portsc, val);
        sleep_us(5000);

        if (!(mmio_read_32(portsc) & PORTSC_PP)) {
            xhci_warn("Port %u power enable failed", portnum);
            return false;
        }
    }

    uint32_t old_ped = val & PORTSC_PED;

    if (is_usb3) {
        val |= PORTSC_WPR;
        mmio_write_32(portsc, val);

        uint16_t timeout = 25;
        while (mmio_read_32(portsc) & PORTSC_RESET) {
            if (timeout-- == 0) {
                xhci_warn("Can't reset USB 3.0 device on port %u", portnum);
                return false;
            }
            sleep_us(5000);
        }
    } else {
        val |= PORTSC_RESET;
        mmio_write_32(portsc, val);

        uint16_t timeout = 25;
        while ((mmio_read_32(portsc) & PORTSC_PED) == old_ped) {
            if (timeout-- == 0) {
                xhci_warn("Can't reset USB 2.0 device on port %u", portnum);
                return false;
            }
            sleep_us(5000);
        }
    }

    sleep_us(5000);

    if (is_usb3) {
        val = mmio_read_32(portsc);
        val &= ~PORTSC_PLS_MASK;
        val |= PORTSC_PLS_U0 << PORTSC_PLS_SHIFT;
        mmio_write_32(portsc, val);
    }

    if ((mmio_read_32(portsc) & PORTSC_SPEED_MASK) == 0) {
        xhci_warn("Port Reset failed -- port speed undefined");
    }

    if (!xhci_consume_port_status_change(dev)) {
        xhci_warn("Port %u reset did not generate a PSC event", portnum);
        return false;
    }

    return true;
}

/*

bool xhci_reset_port(struct xhci_device *dev, uint32_t port_index) {
    uint32_t *portsc = (void *) &dev->port_regs[port_index];
    uint32_t val = mmio_read_32(portsc);

    bool is_usb3 = dev->port_info[port_index].usb3;

    val |= PORTSC_PRC | PORTSC_CSC | PORTSC_CCS | PORTSC_PEC | PORTSC_PLC;
    mmio_write_32(portsc, val);

    val = mmio_read_32(portsc);

    if (is_usb3) {
        val &= ~PORTSC_PLS_MASK;
        val |= PORTSC_PLS_RXDETECT << PORTSC_PLS_SHIFT;
        mmio_write_32(portsc, val);

        uint64_t timeout = 100 * 1000;
        while (!(mmio_read_32(portsc) & PORTSC_PED) && timeout--) {
            sleep_us(50);
        }

        if (!(mmio_read_32(portsc) & PORTSC_PED)) {
            xhci_warn("Port %u USB3 never enabled (PED=0)", port_index);
            return false;
        }
    }

    val = mmio_read_32(portsc);
    uint32_t old_ped = val & PORTSC_PED;
    val = mmio_read_32(portsc);
    val |= PORTSC_RESET;
    mmio_write_32(portsc, val);

    uint64_t timeout = 100;
    if (is_usb3) {
        while ((mmio_read_32(portsc) & PORTSC_RESET) && timeout--) {
            sleep_us(50);
        }
    } else {
        while ((mmio_read_32(portsc) & PORTSC_PED) == old_ped && timeout--) {
            sleep_us(50);
        }
    }

    if ((mmio_read_32(portsc) & PORTSC_RESET) && is_usb3) {
        xhci_warn("Port %u USB3 reset timed out", port_index);
        return false;
    } else if (!(is_usb3) && (mmio_read_32(portsc) & PORTSC_PED) == old_ped) {
        xhci_warn("Port %u USB2 reset timed out", port_index);
        return false;
    }

    if (!xhci_consume_port_status_change(dev)) {
        xhci_warn("Port %u reset did not generate a PSC event", port_index);
        return false;
    }

    val = mmio_read_32(portsc);
    val &= ~PORTSC_PLS_MASK;
    val |= PORTSC_PLS_U0 << PORTSC_PLS_SHIFT;
    mmio_write_32(portsc, val);

    return true;
}

*/

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

    for (uint32_t i = 0; i < 64; i++) {
        dev->port_info[i].usb3 = false;
    }

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
