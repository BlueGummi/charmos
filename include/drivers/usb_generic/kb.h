#include <drivers/usb_generic/usb.h>
#include <input/keyboard.h>

struct usb_kbd_report {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

struct usb_hid_keyboard {
    struct usb_device *dev;
    struct usb_endpoint *ep;

    struct generic_keyboard gkbd;

    struct usb_kbd_report last;
    struct usb_kbd_report cur;

    struct usb_request req;
    atomic_bool worker_exietd;
};
