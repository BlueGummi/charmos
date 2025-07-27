#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

static uint8_t get_desc_bitmap(void) {
    uint8_t bitmap = 0;
    bitmap |= USB_REQUEST_TRANS_DTH << USB_REQUEST_TRANSFER_SHIFT;
    bitmap |= USB_REQUEST_TYPE_STANDARD << USB_REQUEST_TYPE_SHIFT;
    bitmap |= USB_REQUEST_RECIPIENT_DEVICE;
    return bitmap;
}

bool usb_get_string_descriptor(struct usb_device *dev, uint8_t string_idx,
                               char *out, size_t max_len) {
    if (!string_idx)
        return false;

    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;
    uint8_t *desc = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_STRING << USB_DESC_TYPE_SHIFT) | string_idx,
        .index = 0,
        .length = 255,
    };

    if (!ctrl->ops.submit_control_transfer(ctrl, port, &setup, desc))
        return false;

    uint8_t bLength = desc[0];
    if (bLength < 2 || bLength > 255)
        return false;

    size_t out_idx = 0;
    for (size_t i = 2; i < bLength && out_idx < (max_len - 1); i += 2) {
        out[out_idx++] = (desc[i + 1] == 0) ? desc[i] : '?';
    }
    out[out_idx] = '\0';

    return true;
}

void usb_get_device_descriptor(struct usb_device *dev) {
    uint8_t *desc = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_DEVICE << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;
    if (!ctrl->ops.submit_control_transfer(ctrl, port, &setup, desc)) {
        usb_warn("Failed to get device descriptor on port %u", port);
        return;
    }

    struct usb_device_descriptor *ddesc = (void *) desc;

    char manufacturer[128] = {0};
    char product[128] = {0};

    usb_get_string_descriptor(dev, ddesc->manufacturer, manufacturer,
                              sizeof(manufacturer));
    usb_get_string_descriptor(dev, ddesc->product, product, sizeof(product));

    dev->descriptor = ddesc;
    k_printf("%s manufactured by %s:\n", product, manufacturer);
    k_printf("  Length: %u\n", ddesc->length);
    k_printf("  DescriptorType: %u\n", ddesc->type);
    k_printf("  USB: %04x\n", ddesc->usb_num_bcd);
    k_printf("  DeviceClass: %u\n", ddesc->class);
    k_printf("  Vendor: %04x\n", ddesc->vendor_id);
    k_printf("  Product: %04x\n", ddesc->product_id);
    k_printf("  NumConfigurations: %u\n", ddesc->num_configs);
}

void usb_get_config_descriptor(struct usb_device *dev) {
    uint8_t *desc = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_CONFIG << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;
    if (!ctrl->ops.submit_control_transfer(ctrl, port, &setup, desc)) {
        usb_warn("Failed to get config descriptor on port %u", port);
        return;
    }

    struct usb_config_descriptor *cdesc = (void *) desc;

    memcpy(&dev->config, cdesc, sizeof(struct usb_config_descriptor));

    char conf[128] = {0};
    usb_get_string_descriptor(dev, cdesc->configuration, conf, sizeof(conf));
    k_printf("CONFIG is %s\n", conf);

    uint16_t total_len = cdesc->total_length;
    setup.length = total_len;

    if (!ctrl->ops.submit_control_transfer(ctrl, port, &setup, desc)) {
        usb_warn("Failed to get full config descriptor on port %u", port);
        return;
    }

    uint8_t *ptr = desc;
    uint8_t *end = desc + total_len;

    while (ptr < end) {
        uint8_t len = ptr[0];
        uint8_t dtype = ptr[1];

        if (dtype == USB_DESC_TYPE_INTERFACE) {
            struct usb_interface_descriptor *iface = (void *) ptr;
            if (iface->class == USB_CLASS_HID && iface->subclass == 0x01 &&
                iface->protocol == 0x01) {
                k_printf("Found keyboard interface at %u\n",
                         iface->interface_number);
            }
        }

        ptr += len;
    }

    uint8_t bitmap = 0;
    bitmap |= USB_REQUEST_TRANS_HTD << USB_REQUEST_TRANSFER_SHIFT;
    bitmap |= USB_REQUEST_TYPE_STANDARD << USB_REQUEST_TYPE_SHIFT;
    bitmap |= USB_REQUEST_RECIPIENT_DEVICE;

    struct usb_setup_packet set_cfg = {
        .bitmap_request_type = bitmap,
        .request = USB_RQ_CODE_SET_CONFIG,
        .value = cdesc->configuration_value,
        .index = 0,
        .length = 0,
    };

    if (!ctrl->ops.submit_control_transfer(ctrl, port, &set_cfg, NULL)) {
        usb_warn("Failed to set configuration on port %u\n", port);
        return;
    }
}
