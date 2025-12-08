#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

uint8_t usb_construct_rq_bitmap(uint8_t transfer, uint8_t type, uint8_t recip) {
    uint8_t bitmap = 0;
    bitmap |= transfer << USB_REQUEST_TRANSFER_SHIFT;
    bitmap |= type << USB_REQUEST_TYPE_SHIFT;
    bitmap |= recip;
    return bitmap;
}

static uint8_t get_desc_bitmap(void) {
    return usb_construct_rq_bitmap(USB_REQUEST_TRANS_DTH,
                                   USB_REQUEST_TYPE_STANDARD,
                                   USB_REQUEST_RECIPIENT_DEVICE);
}

bool usb_get_string_descriptor(struct usb_device *dev, uint8_t string_idx,
                               char *out, size_t max_len) {
    if (!string_idx)
        return false;

    struct usb_controller *ctrl = dev->host;
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_STRING << USB_DESC_TYPE_SHIFT) | string_idx,
        .index = 0,
        .length = 255,
    };

    struct usb_packet packet = {
        .setup = &setup,
        .data = desc,
    };

    if (!ctrl->ops.submit_control_transfer(dev, &packet))
        return false;

    uint8_t bLength = desc[0];
    if (bLength < 2 || bLength > 255)
        return false;

    size_t out_idx = 0;
    for (size_t i = 2; i < bLength && out_idx < (max_len - 1); i += 2) {
        out[out_idx++] = (desc[i + 1] == 0) ? desc[i] : '?';
    }
    out[out_idx] = '\0';

    kfree_aligned(desc, FREE_PARAMS_DEFAULT);
    return true;
}

void usb_get_device_descriptor(struct usb_device *dev) {
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_DEVICE << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;

    struct usb_packet packet = {
        .setup = &setup,
        .data = desc,
    };

    if (!ctrl->ops.submit_control_transfer(dev, &packet)) {
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

static void match_interfaces(struct usb_driver *driver,
                             struct usb_device *dev) {
    for (uint8_t i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface_descriptor *in = dev->interfaces[i];
        bool class, subclass, proto;
        class = driver->class_code == 0xFF || driver->class_code == in->class;
        subclass = driver->subclass == 0xFF || driver->subclass == in->subclass;
        proto = driver->protocol == 0xFF || driver->protocol == in->protocol;

        bool everything_matches = class && subclass && proto;
        if (everything_matches) {
            if (driver->probe && driver->probe(dev)) {
                dev->driver = driver;
                return;
            }
        }
    }
}

void usb_try_bind_driver(struct usb_device *dev) {
    struct usb_driver *start = __skernel_usb_drivers;
    struct usb_driver *end = __ekernel_usb_drivers;

    for (struct usb_driver *d = start; d < end; d++)
        match_interfaces(d, dev);
}

static void
usb_register_dev_interface(struct usb_device *dev,
                           struct usb_interface_descriptor *interface) {
    struct usb_interface_descriptor *new_int =
        kmalloc(sizeof(struct usb_interface_descriptor), ALLOC_PARAMS_DEFAULT);
    memcpy(new_int, interface, sizeof(struct usb_interface_descriptor));

    size_t size = (dev->num_interfaces + 1) * sizeof(void *);

    dev->interfaces = krealloc(dev->interfaces, size, ALLOC_PARAMS_DEFAULT);
    dev->interfaces[dev->num_interfaces++] = new_int;
}

static void usb_register_dev_ep(struct usb_device *dev,
                                struct usb_endpoint_descriptor *endpoint) {
    struct usb_endpoint_descriptor *new_ep =
        kmalloc(sizeof(struct usb_endpoint_descriptor), ALLOC_PARAMS_DEFAULT);
    memcpy(new_ep, endpoint, sizeof(struct usb_endpoint_descriptor));

    struct usb_endpoint *ep =
        kzalloc(sizeof(struct usb_endpoint), ALLOC_PARAMS_DEFAULT);

    ep->type = USB_ENDPOINT_ATTR_TRANS_TYPE(endpoint->attributes);
    ep->number = USB_ENDPOINT_ADDR_EP_NUM(endpoint->address);
    ep->address = endpoint->address;
    ep->max_packet_size = endpoint->max_packet_size;
    ep->interval = endpoint->interval;

    ep->in = USB_ENDPOINT_ADDR_EP_DIRECTION(endpoint->address);

    size_t size = (dev->num_endpoints + 1) * sizeof(void *);
    dev->endpoints = krealloc(dev->endpoints, size, ALLOC_PARAMS_DEFAULT);
    dev->endpoints[dev->num_endpoints++] = ep;
}

static void setup_config_descriptor(struct usb_device *dev, uint8_t *ptr,
                                    uint8_t *end) {
    while (ptr < end) {
        uint8_t len = ptr[0];
        uint8_t dtype = ptr[1];

        if (dtype == USB_DESC_TYPE_INTERFACE) {
            struct usb_interface_descriptor *iface = (void *) ptr;
            usb_register_dev_interface(dev, iface);
        } else if (dtype == USB_DESC_TYPE_ENDPOINT) {
            struct usb_endpoint_descriptor *epd = (void *) ptr;
            usb_register_dev_ep(dev, epd);
        }

        ptr += len;
    }
}

bool usb_parse_config_descriptor(struct usb_device *dev) {
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_CONFIG << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;

    struct usb_packet packet = {
        .setup = &setup,
        .data = desc,

    };

    if (!ctrl->ops.submit_control_transfer(dev, &packet)) {
        usb_warn("Failed to get config descriptor header on port %u", port);
        kfree_aligned(desc, FREE_PARAMS_DEFAULT);
        return false;
    }

    struct usb_config_descriptor *cdesc = (void *) desc;
    memcpy(&dev->config, cdesc, sizeof(struct usb_config_descriptor));

    uint16_t total_len = cdesc->total_length;
    setup.length = total_len;

    if (!ctrl->ops.submit_control_transfer(dev, &packet)) {
        usb_warn("Failed to get full config descriptor on port %u", port);
        kfree_aligned(desc, FREE_PARAMS_DEFAULT);
        return false;
    }

    char conf[128] = {0};
    usb_get_string_descriptor(dev, cdesc->configuration, conf, sizeof(conf));
    k_printf("CONFIG is %s\n", conf);

    setup_config_descriptor(dev, desc, desc + total_len);
    kfree_aligned(desc, FREE_PARAMS_DEFAULT);
    return true;
}

bool usb_set_configuration(struct usb_device *dev) {
    struct usb_controller *ctrl = dev->host;
    uint8_t port = dev->port;

    uint8_t bitmap = usb_construct_rq_bitmap(USB_REQUEST_TRANS_HTD,
                                             USB_REQUEST_TYPE_STANDARD,
                                             USB_REQUEST_RECIPIENT_DEVICE);

    struct usb_setup_packet set_cfg = {
        .bitmap_request_type = bitmap,
        .request = USB_RQ_CODE_SET_CONFIG,
        .value = dev->config.configuration_value,
        .index = 0,
        .length = 0,
    };

    struct usb_packet packet = {
        .setup = &set_cfg,
        .data = NULL,
    };

    if (!ctrl->ops.submit_control_transfer(dev, &packet)) {
        usb_warn("Failed to set configuration on port %u\n", port);
        return false;
    }

    return true;
}
