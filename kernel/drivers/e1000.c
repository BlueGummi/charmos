#include <asm.h>
#include <console/printf.h>
#include <drivers/e1000.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <drivers/pci.h>
#include <sleep.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define E1000_MAX_TX_PACKET_SIZE 1518
#define REG32(dev, offset) (&(dev->regs[(offset) / 4U]))

static void e1000_reset(struct e1000_device *dev) {
    mmio_write_32(REG32(dev, E1000_REG_CTRL), E1000_CTRL_RST);
    sleep_ms(1);
}

static void e1000_setup_tx_ring(struct e1000_device *dev) {
    size_t space = sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC;
    dev->tx_descs_phys = (uintptr_t) pmm_alloc_pages(space / PAGE_SIZE, false);
    dev->tx_descs = vmm_map_phys(dev->tx_descs_phys, space);
    memset(dev->tx_descs, 0, space);

    for (int i = 0; i < E1000_NUM_TX_DESC; ++i) {
        dev->tx_buffers[i] = kmalloc(2048);
        dev->tx_descs[i].addr = vmm_get_phys((uintptr_t) dev->tx_buffers[i]);
        dev->tx_descs[i].status = E1000_TXD_STAT_DD;
    }

    mmio_write_32(REG32(dev, E1000_REG_TDBAL),
                  (uint32_t) (dev->tx_descs_phys & 0xFFFFFFFF));
    mmio_write_32(REG32(dev, E1000_REG_TDBAH),
                  (uint32_t) (dev->tx_descs_phys >> 32));
    mmio_write_32(REG32(dev, E1000_REG_TDLEN),
                  E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    mmio_write_32(REG32(dev, E1000_REG_TDH), 0);
    mmio_write_32(REG32(dev, E1000_REG_TDT), 0);
    dev->tx_tail = 0;

    mmio_write_32(REG32(dev, E1000_REG_TCTL),
                  E1000_TCTL_EN | E1000_TCTL_PSP |
                      (0x10 << E1000_TCTL_CT_SHIFT) |
                      (0x40 << E1000_TCTL_COLD_SHIFT));
}

static void e1000_setup_rx_ring(struct e1000_device *dev) {
    size_t space = sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC;
    dev->rx_descs_phys = (uintptr_t) pmm_alloc_pages(space / PAGE_SIZE, false);
    dev->rx_descs = vmm_map_phys(dev->rx_descs_phys, space);
    memset(dev->rx_descs, 0, space);

    for (int i = 0; i < E1000_NUM_RX_DESC; ++i) {
        dev->rx_buffers[i] = kmalloc(E1000_RX_BUF_SIZE);
        dev->rx_descs[i].addr = vmm_get_phys((uintptr_t) dev->rx_buffers[i]);
        dev->rx_descs[i].status = 0;
    }

    mmio_write_32(REG32(dev, E1000_REG_RDBAL),
                  (uint32_t) (dev->rx_descs_phys & 0xFFFFFFFF));
    mmio_write_32(REG32(dev, E1000_REG_RDBAH),
                  (uint32_t) (dev->rx_descs_phys >> 32));
    mmio_write_32(REG32(dev, E1000_REG_RDLEN),
                  E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    mmio_write_32(REG32(dev, E1000_REG_RDH), 0);
    mmio_write_32(REG32(dev, E1000_REG_RDT), E1000_NUM_RX_DESC - 1);
    dev->rx_tail = E1000_NUM_RX_DESC - 1;

    mmio_write_32(REG32(dev, E1000_REG_RCTL),
                  E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

int e1000_send_packet(struct e1000_device *dev, const void *data, size_t len) {
    if (len > E1000_MAX_TX_PACKET_SIZE) {
        return -1;
    }

    uint32_t next = dev->tx_tail;
    struct e1000_tx_desc *desc = &dev->tx_descs[next];

    if (!(desc->status & E1000_TXD_STAT_DD)) {
        return -1;
    }

    memcpy(dev->tx_buffers[next], data, len);

    desc->length = (uint16_t) len;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;

    dev->tx_tail = (next + 1) % E1000_NUM_TX_DESC;
    mmio_write_32(REG32(dev, E1000_REG_TDT), dev->tx_tail);

    return 0;
}

static inline uint16_t htons(uint16_t hostshort) {
    return (hostshort << 8) | (hostshort >> 8);
}

static inline uint32_t htonl(uint32_t hostlong) {
    return ((hostlong << 24) & 0xFF000000) | ((hostlong << 8) & 0x00FF0000) |
           ((hostlong >> 8) & 0x0000FF00) | ((hostlong >> 24) & 0x000000FF);
}

static uint16_t checksum(void *data, int len) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *) data;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0)
        sum += *((uint8_t *) ptr);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t) (~sum);
}

void send_hardcoded_ping(struct e1000_device *dev) {
    uint8_t packet[14 + 20 + 8 + 32];
    memset(packet, 0, sizeof(packet));

    struct eth_hdr *eth = (void *) packet;
    struct ipv4_hdr *ip = (void *) (packet + 14);
    struct icmp_hdr *icmp = (void *) (packet + 14 + 20);
    uint8_t *payload = packet + 14 + 20 + 8;

    uint8_t src_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t dest_mac[6] = {0x52, 0x54, 0x00, 0x8e, 0x61, 0xf4};
    ip->dest_ip = htonl(0xC0A87A01); // 192.168.122.1
    ip->src_ip = htonl(0xC0A87A64);  //  192.168.122.100

    memcpy(eth->dest, dest_mac, 6);
    memcpy(eth->src, src_mac, 6);
    eth->ethertype = htons(0x0800);

    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(20 + 8 + 32);
    ip->id = htons(0x1234);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = 1;
    ip->checksum = 0;
    ip->checksum = checksum(ip, 20);

    icmp->type = 8; // Echo request
    icmp->code = 0;
    icmp->identifier = htons(0x1);
    icmp->sequence = htons(0x1);
    memset(payload, 0xAA, 32); // dummy payload

    icmp->checksum = 0;
    icmp->checksum = checksum(icmp, 8 + 32);

    e1000_send_packet(dev, packet, sizeof(packet));
}

bool e1000_init(struct pci_device *pci, struct e1000_device *dev) {
    memset(dev, 0, sizeof(*dev));

    dev->bus = pci->bus;
    dev->device = pci->device;
    dev->function = pci->function;
    k_info("e1000", K_INFO, "Found device at %02x:%02x.%02x", pci->bus,
           pci->device, pci->function);

    uint32_t bar = pci_read(dev->bus, dev->device, dev->function, PCI_BAR0);
    if (bar & 0x1)
        return false; // Not MMIO

    uint32_t phys_addr = bar & ~0xF;

    pci_write(dev->bus, dev->device, dev->function, PCI_BAR0, 0xFFFFFFFF);
    uint32_t bar_mask =
        pci_read(dev->bus, dev->device, dev->function, PCI_BAR0);
    pci_write(dev->bus, dev->device, dev->function, PCI_BAR0, bar);

    size_t mmio_size = ~(bar_mask & ~0xF) + 1;
    if (mmio_size == 0 || mmio_size > (1 << 24))
        return false;

    dev->regs = vmm_map_phys(phys_addr, mmio_size);
    if (!dev->regs)
        return false;

    e1000_reset(dev);

    e1000_setup_tx_ring(dev);
    e1000_setup_rx_ring(dev);
    send_hardcoded_ping(dev);
    k_info("e1000", K_INFO, "Device initialized successfully");
    return true;
}
