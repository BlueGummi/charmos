#include <console/printf.h>
#include <drivers/usb.h>
#include <drivers/usb_hid.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

struct usb_kbd_report {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

static const char keycode_to_ascii[256] = {
    [0x04] = 'a',  [0x05] = 'b',  [0x06] = 'c',  [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f',  [0x0A] = 'g',  [0x0B] = 'h',  [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k',  [0x0F] = 'l',  [0x10] = 'm',  [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p',  [0x14] = 'q',  [0x15] = 'r',  [0x16] = 's', [0x17] = 't',
    [0x18] = 'u',  [0x19] = 'v',  [0x1A] = 'w',  [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',  [0x1E] = '1',  [0x1F] = '2',  [0x20] = '3', [0x21] = '4',
    [0x22] = '5',  [0x23] = '6',  [0x24] = '7',  [0x25] = '8', [0x26] = '9',
    [0x27] = '0',  [0x28] = '\n', [0x29] = 27, // ESC
    [0x2A] = '\b',                             // Backspace
    [0x2B] = '\t',                             // Tab
    [0x2C] = ' ',  [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[', [0x30] = ']',
    [0x31] = '\\', [0x33] = ';',  [0x34] = '\'', [0x35] = '`', [0x36] = ',',
    [0x37] = '.',  [0x38] = '/'};

static const char keycode_to_ascii_shifted[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z', [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(',
    [0x27] = ')', [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}',
    [0x31] = '|', [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<',
    [0x37] = '>', [0x38] = '?'};

void parse_keyboard_report(const struct usb_kbd_report *report) {
    static uint8_t prev_keys[6] = {0};

    const bool shift = (report->modifiers & 0x22) != 0;
    const char *table = shift ? keycode_to_ascii_shifted : keycode_to_ascii;

    for (int i = 0; i < 6; i++) {
        uint8_t code = report->keys[i];
        if (!code)
            continue;

        if (memchr(prev_keys, code, sizeof(prev_keys)) == NULL) {
            char ch = table[code];
            if (ch) {
                k_printf("%c", ch);
            }
        }
    }

    memcpy(prev_keys, report->keys, sizeof(prev_keys));
}

struct usb_interface_descriptor *usb_find_interface(struct usb_device *dev,
                                                    uint8_t class,
                                                    uint8_t subclass,
                                                    uint8_t protocol) {
    for (size_t i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface_descriptor *intf = dev->interfaces[i];
        if (intf->class == class && intf->subclass == subclass &&
            intf->protocol == protocol)
            return intf;
    }
    return NULL;
}

bool usb_keyboard_get_descriptor(struct usb_device *dev,
                                 uint8_t interface_number, uint16_t len,
                                 void *buf) {
    uint8_t bm = usb_construct_rq_bitmap(USB_REQUEST_TRANS_DTH,
                                         USB_REQUEST_TYPE_STANDARD,
                                         USB_REQUEST_RECIPIENT_INTERFACE);

    struct usb_setup_packet setup = {
        .bitmap_request_type = bm,
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = USB_HID_DESC_TYPE_REPORT << 8,
        .length = len,
        .index = interface_number,
    };

    struct usb_packet packet = {
        .setup = &setup,
        .data = buf,
    };

    return dev->host->ops.submit_control_transfer(dev, &packet);
}

void usb_keyboard_poll(struct usb_device *dev) {
    struct usb_endpoint *ep = NULL;
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        if ((dev->endpoints[i]->address & 0x80) &&
            dev->endpoints[i]->type == USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT) {
            ep = dev->endpoints[i];
            break;
        }
    }

    if (!ep) {
        usb_warn("usbkbd: no interrupt IN endpoint found");
        return;
    }

    return;

    struct usb_kbd_report last = {0}, report = {0};

    struct usb_packet packet = {
        .data = &report,
        .length = sizeof(report),
        .ep = ep,
    };

    while (true) {
        bool ok = dev->host->ops.submit_interrupt_transfer(dev, &packet);

        if (!ok)
            continue;

        if (memcmp(&last, &report, sizeof(report)) != 0) {
            struct xhci_device *x = dev->host->driver_data;
            k_printf("0b%b\n", x->intr_regs->iman);
            parse_keyboard_report(&report);
            last = report;
        }

        sleep_us(10);
    }
}

bool usb_keyboard_probe(struct usb_device *dev) {
    struct usb_interface_descriptor *intf =
        usb_find_interface(dev, 0x03, 0x01, 0x01);
    if (!intf)
        return false;

    uint8_t iface_num = intf->interface_number;

    uint8_t *report_buf = kzalloc_aligned(256, PAGE_SIZE);
    if (!usb_keyboard_get_descriptor(dev, iface_num, 256, report_buf)) {
        usb_warn("usbkbd: Failed to fetch report descriptor");
        return false;
    }
    kfree_aligned(report_buf);

    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];
        if ((ep->address & 0x80) &&
            ep->type == USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT) {

            usb_keyboard_poll(dev);
            k_info("usbkbd", K_INFO, "Keyboard endpoint 0x%02x ready",
                   ep->address);
            return true;
        }
    }

    usb_warn("usbkbd: No interrupt IN endpoint found");
    return false;
}

REGISTER_USB_DRIVER(keyboard, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                    USB_PROTOCOL_HID_KEYBOARD, usb_keyboard_probe, NULL);
