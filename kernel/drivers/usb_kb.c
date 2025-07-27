#include <drivers/usb.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>

/*void setup_keyboard_polling(struct usb_device *dev, struct usb_endpoint *ep) {
    while (true) {
        uint8_t buf[8] = {0};
        bool success = dev->host->ops.submit_interrupt_transfer(dev->host,
dev->port, ep, buf, sizeof(buf));

        if (success) {
            parse_keyboard_report(buf);
        }

        sleep(10); // ms
    }
}*/

bool usb_keyboard_probe(struct usb_device *dev) {
    for (uint8_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];

        bool is_interrupt = ep->type == USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT;
        if ((ep->address & 0x80) && is_interrupt) {
            // setup_keyboard_polling(dev, ep);
            k_info("usbkbd", K_INFO, "USB keyboard initialized");
            return true;
        }
    }

    usb_warn("usbkb: No interrupt IN endpoint found on keyboard");
    return false;
}

REGISTER_USB_DRIVER(keyboard, USB_CLASS_HID, USB_SUBCLASS_HID_BOOT_INTERFACE,
                    USB_PROTOCOL_HID_KEYBOARD, usb_keyboard_probe, NULL);
