#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

bool usb_keyboard_probe(struct usb_device *dev) {
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];

        /* Look for IN endpoints */
    }

    usb_warn("usbkb: No interrupt IN endpoint found on keyboard");
    return false;
}

REGISTER_USB_DRIVER(keyboard, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                    USB_PROTOCOL_HID_KEYBOARD, usb_keyboard_probe, NULL);
