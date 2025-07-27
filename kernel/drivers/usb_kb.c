#include <console/printf.h>
#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

static const char keycode_to_ascii[256] = {
    [0x04] = 'a',  [0x05] = 'b', [0x06] = 'c',  [0x07] = 'd',
    [0x08] = 'e',  [0x09] = 'f', [0x0A] = 'g',  [0x0B] = 'h',
    [0x0C] = 'i',  [0x0D] = 'j', [0x0E] = 'k',  [0x0F] = 'l',
    [0x10] = 'm',  [0x11] = 'n', [0x12] = 'o',  [0x13] = 'p',
    [0x14] = 'q',  [0x15] = 'r', [0x16] = 's',  [0x17] = 't',
    [0x18] = 'u',  [0x19] = 'v', [0x1A] = 'w',  [0x1B] = 'x',
    [0x1C] = 'y',  [0x1D] = 'z', [0x1E] = '1',  [0x1F] = '2',
    [0x20] = '3',  [0x21] = '4', [0x22] = '5',  [0x23] = '6',
    [0x24] = '7',  [0x25] = '8', [0x26] = '9',  [0x27] = '0',
    [0x28] = '\n', [0x2C] = ' ', [0x2D] = '-',  [0x2E] = '=',
    [0x2F] = '[',  [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',',  [0x37] = '.',
    [0x38] = '/',  [0x39] = '\e' // ESC
};

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

void parse_keyboard_report(const uint8_t *report) {
    static uint8_t prev_keys[6] = {0};

    uint8_t modifiers = report[0];
    const bool shift = (modifiers & (1 << 1)) || (modifiers & (1 << 5));

    const char *table = shift ? keycode_to_ascii_shifted : keycode_to_ascii;

    for (int i = 2; i < 8; i++) {
        uint8_t code = report[i];
        if (code && memchr(prev_keys, code, 6) == NULL) {
            char ch = table[code];
            if (ch)
                k_printf("%c", ch);
        }
    }

    memcpy(prev_keys, &report[2], 6);
}

void setup_keyboard_polling(struct usb_device *dev, struct usb_endpoint *ep) {
    struct usb_packet pkt = {
        .data = kzalloc(8),
        .length = 8,
    };

    while (true) {
        bool success = dev->host->ops.submit_interrupt_transfer(
            dev->host, dev->port, &pkt);

        if (success) {
            parse_keyboard_report(pkt.data);
        }

        sleep_ms(10);
    }
}

bool usb_keyboard_probe(struct usb_device *dev) {
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];

        bool is_interrupt = ep->type == USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT;
        if ((ep->address & 0x80) && is_interrupt) {
            //  setup_keyboard_polling(dev, ep);
            k_info("usbkbd", K_INFO, "USB keyboard initialized");
            return true;
        }
    }

    usb_warn("usbkb: No interrupt IN endpoint found on keyboard");
    return false;
}

REGISTER_USB_DRIVER(keyboard, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                    USB_PROTOCOL_HID_KEYBOARD, usb_keyboard_probe, NULL);
