#include <stdint.h>
#include <stdint.h>
#define MAX_PCI_DEVICES 256

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
};



const char *pci_class_name(uint8_t class_code, uint8_t subclass);

void scan_pci_devices(struct pci_device **devices_out, uint64_t *count_out);
