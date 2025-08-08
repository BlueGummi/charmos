#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void xhci_setup_event_ring(struct xhci_device *dev) {
    struct xhci_erst_entry *erst_table = hugepage_alloc_page();
    uintptr_t erst_table_phys = vmm_get_phys((uintptr_t) erst_table);

    struct xhci_trb *event_ring = hugepage_alloc_page();
    uintptr_t event_ring_phys = vmm_get_phys((uintptr_t) event_ring);

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

    struct xhci_trb *cmd_ring = hugepage_alloc_page();
    uintptr_t cmd_ring_phys = vmm_get_phys((uintptr_t) cmd_ring);

    int last_index = TRB_RING_SIZE - 1;
    cmd_ring[last_index].parameter = cmd_ring_phys;
    cmd_ring[last_index].status = 0;
    /* Toggle Cycle, Cycle bit = 1 */
    cmd_ring[last_index].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    struct xhci_dcbaa *dcbaa_virt = hugepage_alloc_page();
    uintptr_t dcbaa_phys = vmm_get_phys((uintptr_t) dcbaa_virt);

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
