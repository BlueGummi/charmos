#include <console/printf.h>
#include <drivers/usb_generic/hid.h>
#include <drivers/usb_generic/kb.h>
#include <drivers/usb_generic/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <string.h>

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

static inline bool generic_keyboard_is_modifier(uint32_t keycode) {
    return keycode >= USB_HID_MODIFIER_BASE &&
           keycode < USB_HID_MODIFIER_BASE + 8;
}

static void generic_keyboard_dispatch(struct generic_keyboard *kbd,
                                      uint32_t keycode, bool pressed) {
    if (generic_keyboard_is_modifier(keycode)) {
        uint8_t bit = keycode - USB_HID_MODIFIER_BASE;
        if (pressed)
            kbd->modifiers |= (1 << bit);
        else
            kbd->modifiers &= ~(1 << bit);
        return;
    }

    kbd->emit(kbd, keycode, pressed);
}

static inline bool usb_kbd_shift_active(const struct generic_keyboard *kbd) {
    const uint8_t shift_mask = (1 << 1) | /* Left Shift */
                               (1 << 5);  /* Right Shift */

    return (kbd->modifiers & shift_mask) != 0;
}

static void tty_keyboard_emit(struct generic_keyboard *kbd, uint32_t keycode,
                              bool pressed) {
    if (!pressed)
        return;

    bool shift = usb_kbd_shift_active(kbd);
    char ch =
        shift ? keycode_to_ascii_shifted[keycode] : keycode_to_ascii[keycode];

    k_printf("%c", ch);
}

static bool key_in_report(uint8_t key, const struct usb_kbd_report *r) {
    for (int i = 0; i < 6; i++)
        if (r->keys[i] == key)
            return true;
    return false;
}

void usb_kbd_process_report(struct usb_hid_keyboard *kbd,
                            const struct usb_kbd_report *cur) {
    const struct usb_kbd_report *prev = &kbd->last;

    uint8_t changed = prev->modifiers ^ cur->modifiers;
    if (changed) {
        for (int bit = 0; bit < 8; bit++) {
            if (!(changed & (1 << bit)))
                continue;

            uint32_t keycode = USB_HID_MODIFIER_BASE + bit;
            bool pressed = cur->modifiers & (1 << bit);

            kbd->gkbd.emit(&kbd->gkbd, keycode, pressed);
        }
    }

    for (int i = 0; i < 6; i++) {
        uint8_t key = prev->keys[i];
        if (!key)
            continue;

        if (!key_in_report(key, cur)) {
            kbd->gkbd.emit(&kbd->gkbd, key, false);
        }
    }

    for (int i = 0; i < 6; i++) {
        uint8_t key = cur->keys[i];
        if (!key)
            continue;

        if (!key_in_report(key, prev)) {
            kbd->gkbd.emit(&kbd->gkbd, key, true);
        }
    }
}

enum usb_status usb_keyboard_get_descriptor(struct usb_device *dev,
                                            uint8_t interface_number,
                                            uint16_t len, void *buf) {
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

    struct usb_request req = {
        .setup = &setup,
        .buffer = buf,
        .dev = dev,
    };

    return usb_transfer_sync(dev->host->ops.submit_control_transfer, &req);
}

static void usb_kbd_worker(void *arg) {
    struct usb_hid_keyboard *kbd = arg;

    while (true) {
        enum usb_status ret = usb_transfer_sync(
            kbd->dev->host->ops.submit_interrupt_transfer, &kbd->req);
        if (ret != USB_OK)
            break;

        usb_kbd_process_report(kbd, &kbd->cur);
        kbd->last = kbd->cur;
    }
}

struct usb_hid_keyboard *usb_keyboard_create(struct usb_device *dev,
                                             struct usb_endpoint *ep) {
    struct usb_hid_keyboard *kbd = kzalloc(sizeof(*kbd), ALLOC_PARAMS_DEFAULT);

    kbd->dev = dev;
    kbd->ep = ep;

    kbd->gkbd.emit = tty_keyboard_emit;
    kbd->gkbd.priv = kbd;

    kbd->req = (struct usb_request) {
        .buffer = &kbd->cur,
        .length = sizeof(kbd->cur),
        .ep = kbd->ep,
        .dev = dev,
    };

    thread_spawn("usb_kbd_worker", usb_kbd_worker, kbd);
    return kbd;
}

enum usb_status usb_keyboard_bringup(struct usb_device *dev) {
    struct usb_interface_descriptor *intf =
        usb_find_interface(dev, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                           USB_PROTOCOL_HID_KEYBOARD);
    if (!intf)
        return USB_ERR_NO_DEVICE;

    k_info("USBKB", K_INFO, "Keyboard connected");

    uint8_t iface_num = intf->interface_number;

    uint8_t *report_buf = kzalloc_aligned(256, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    enum usb_status err = USB_OK;
    if ((err = usb_keyboard_get_descriptor(dev, iface_num, 256, report_buf)) !=
        USB_OK) {
        kfree_aligned(report_buf, FREE_PARAMS_DEFAULT);
        return err;
    }

    kfree_aligned(report_buf, FREE_PARAMS_DEFAULT);

    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];
        if ((ep->address & 0x80) &&
            ep->type == USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT) {

            dev->driver_private = usb_keyboard_create(dev, ep);
            return USB_OK;
        }
    }

    k_info("USBKB", K_WARN, "No interrupt IN endpoint found");
    return USB_ERR_NO_ENDPOINT;
}

void usb_keyboard_teardown(struct usb_device *dev) {
    (void) dev;
}

void usb_keyboard_free(struct usb_device *dev) {
    kfree(dev->driver_private, FREE_PARAMS_DEFAULT);
    k_info("USBKB", K_INFO, "Keyboard disconnected");
}

USB_DRIVER_REGISTER(keyboard, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                    USB_PROTOCOL_HID_KEYBOARD, usb_keyboard_bringup,
                    usb_keyboard_teardown, usb_keyboard_free);
