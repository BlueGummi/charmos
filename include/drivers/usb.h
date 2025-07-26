#pragma once
#include <stdbool.h>
#include <stdint.h>

/* All page numbers are for the USB 2.0 Specification */

/* Page 279 - Descriptor types */
#define USB_DESC_TYPE_DEVICE 1
#define USB_DESC_TYPE_CONFIG 2
#define USB_DESC_TYPE_STRING 3
#define USB_DESC_TYPE_INTERFACE 4
#define USB_DESC_TYPE_ENDPOINT 5
#define USB_DESC_TYPE_DEV_QUALIFIER 6
#define USB_DESC_TYPE_OTHER_SPEED_CONFIG 7
#define USB_DESC_TYPE_INTERFACE_POWER 8

/* Request bitmap field definitions */
#define USB_REQUEST_TRANS_HTD 0
#define USB_REQUEST_TRANS_DTH 1

#define USB_REQUEST_TYPE_STANDARD 0
#define USB_REQUEST_TYPE_CLASS 1
#define USB_REQUEST_TYPE_VENDOR 2

#define USB_REQUEST_RECIPIENT_DEVICE 0
#define USB_REQUEST_RECIPIENT_INTERFACE 1
#define USB_REQUEST_RECIPIENT_ENDPOINT 2
#define USB_REQUEST_RECIPIENT_OTHER 3

#define USB_REQUEST_TRANSFER(byte) ((byte >> 7) & 1)
#define USB_REQUEST_TYPE(byte) ((byte >> 5) & 3)
#define USB_REQUEST_RECIPIENT(byte) (byte & 0x1F)

/* Config bitmap definitions */
#define USB_CONFIG_SELF_POWERED (1 << 7)
#define USB_CONFIG_REMOTE_WAKEUP (1 << 6)

/* Endpoint address bitmap definitions */
#define USB_ENDPOINT_ADDR_EP_DIRECTION_OUT 0
#define USB_ENDPOINT_ADDR_EP_DIRECTION_IN 1
#define USB_ENDPOINT_ADDR_EP_NUM(byte) (byte & 7)
#define USB_ENDPOINT_ADDR_EP_DIRECTION(byte) ((byte >> 7) & 1)

/* Endpoint attribute bitmap definitions */
#define USB_ENDPOINT_ATTR_TRANS_TYPE_CONTROL 0
#define USB_ENDPOINT_ATTR_TRANS_TYPE_ISOCHRONOUS 1
#define USB_ENDPOINT_ATTR_TRANS_TYPE_BULK 2
#define USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT 3

#define USB_ENDPOINT_ATTR_SYNC_TYPE_NO_SYNC 0
#define USB_ENDPOINT_ATTR_SYNC_TYPE_ASYNC 1
#define USB_ENDPOINT_ATTR_SYNC_TYPE_ADAPTIVE 2
#define USB_ENDPOINT_ATTR_SYNC_TYPE_SYNC 3

#define USB_ENDPOINT_ATTR_USAGE_TYPE_DATA 0
#define USB_ENDPOINT_ATTR_USAGE_TYPE_FEEDBACK 1
#define USB_ENDPOINT_ATTR_USAGE_TYPE_IMPLICIT_FEEDBACK_DATA 2

#define USB_ENDPOINT_ATTR_TRANS_TYPE(byte) (byte & 3)
#define USB_ENDPOINT_ATTR_SYNC_TYPE(byte) ((byte >> 2) & 3)
#define USB_ENDPOINT_ATTR_USAGE_TYPE(byte) ((byte >> 4) & 3)

/* Endpoint max packet size bitmap definitions */
#define USB_ENDPOINT_MAX_PACKET_SIZE_SIZE(byte) (byte & 0x3FF)
#define USB_ENDPOINT_MAX_PACKET_SIZE_TRANSACT_OPP(byte) ((byte >> 11) & 3)

#define USB_ENDPOINT_MAX_PACKET_SIZE_TRANSACT_OPP_NUM(num) (num + 1)

/* Endpoint interval conversion definitions */
#define USB_ENDPOINT_INTERVAL_TO_INTEGER_LOW_SPEED(num) (num)
#define USB_ENDPOINT_INTEGER_TO_INTERVAL_LOW_SPEED(num) (num)

#define USB_ENDPOINT_INTERVAL_TO_INTEGER_HIGH_SPEED(num) (2 << (num - 2))

/* Request codes */
enum usb_rq_code : uint8_t {
    USB_RQ_CODE_GET_STATUS = 0,    /* Page 282 */
    USB_RQ_CODE_CLEAR_FEATURE = 1, /* Page 280 */
    USB_RQ_CODE_SET_FEATURE = 3,   /* Page 286 */
    USB_RQ_CODE_SET_ADDR = 5,      /* Page 284 */

    USB_RQ_CODE_GET_DESCRIPTOR = 6, /* Page 281 */
    USB_RQ_CODE_SET_DESCRIPTOR = 7, /* Page 285 */

    USB_RQ_CODE_GET_CONFIG = 8, /* Page 281 */
    USB_RQ_CODE_SET_CONFIG = 9, /* Page 285 */

    USB_RQ_CODE_GET_INTERFACE = 10, /* Page 282 */
    USB_RQ_CODE_SET_INTERFACE = 11, /* Page 287 */

    USB_RQ_CODE_SYNCH_FRAME = 12, /* Page 288 */
};

enum usb_controller_type {
    USB_CONTROLLER_UHCI,
    USB_CONTROLLER_EHCI,
    USB_CONTROLLER_XHCI,
};

struct usb_setup_packet {        /* Refer to page 276 */
    uint8_t bitmap_request_type; /* Characteristics */
    enum usb_rq_code request;    /* Specific request */
    uint16_t value;              /* Varies according to request */

    union {
        uint16_t index;
        uint16_t offset;
    };

    uint16_t length; /* Number of bytes if there is a data stage */

} __attribute__((packed));
_Static_assert(sizeof(struct usb_setup_packet) == 8, "");

struct usb_device_descriptor { /* Refer to page 290 */
    uint8_t length;
    uint8_t type;
    uint16_t usb_num_bcd; /* USB number in binary coded
                           * decimal. 2.10 is 210H */

    uint8_t class;    /* Class code */
    uint8_t subclass; /* Subclass code */
    uint8_t protocol;
    uint8_t max_packet_size; /* Only 8, 16, 32, or 64 are valid */

    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_num_bcd; /* Device number in binary coded decimal */

    uint8_t manufacturer; /* Index of string desc. describing manufacturer */
    uint8_t product;      /* Index of string desc. describing product */
    uint8_t serial_num;   /* Index of string desc. describing serial number */

    uint8_t num_configs; /* Number of possible configurations */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_device_descriptor) == 18, "");

struct usb_interface_descriptor { /* Page 296 */
    uint8_t length;
    uint8_t type;
    uint8_t interface_number; /* zero-based value ident. the index
                               * in the array of concurrent interfaces */

    uint8_t alternate_setting; /* Select this alternate
                                * setting for the interface */

    uint8_t num_endpoints; /* Number of endpoings excluding EP0 */
    uint8_t class;         /* Interface class code */
    uint8_t subclass;      /* Interface subclass code */

    uint8_t protocol; /* Interface protocol code qualified by `class` and
                         `subclass` */

    uint8_t interface; /* Index of string desc. describing this interface */

} __attribute__((packed));
_Static_assert(sizeof(struct usb_interface_descriptor) == 9, "");

struct usb_config_descriptor { /* Page 293 */
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length; /* Total length of data for this config */
    uint8_t num_interfaces;
    uint8_t configuration_value; /* Value to use as argument
                                  * to SetConfig() request */

    uint8_t configuration; /* Index of string desc. describing this config */
    uint8_t bitmap_attributes; /* Bits 7 and 0..4 are reserved */

    uint8_t max_power; /* Max power of USB device in milliamps */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_config_descriptor) == 9, "");

struct usb_endpoint_descriptor { /* Page 297 */
    uint8_t length;
    uint8_t type;
    uint8_t address;    /* 3..0 is EP number, 7 is direction */
    uint8_t attributes; /* Refer to definitions */

    uint16_t max_packet_size; /* Max packet size as a bitmap */

    uint8_t interval; /* Interval for polling this EP for data transfer */
} __attribute__((packed));
_Static_assert(sizeof(struct usb_endpoint_descriptor) == 7, "");

struct usb_endpoint {
    struct usb_endpoint_descriptor *desc;

    /* Putting these here so code won't look like
     * ep->desc.type, I think that looks bad */
    uint8_t type;
    uint8_t number;
    bool in; /* true - in, false - out */
    uint8_t *transfer_buffer;
    uint16_t transfer_len;

    void *hc_data;
};

struct usb_controller { /* Generic USB controller */
    enum usb_controller_type type;
    void *data;
};

struct usb_device {
    uint8_t address;
    uint8_t speed;
    uint8_t port;            /* Port number on the root hub */
    uint8_t slot_id;         /* Used for xHCI */
    uint8_t max_packet_size; /* For endpoint 0 */

    struct usb_device_descriptor *descriptor;
    struct usb_config_descriptor *config;

    struct usb_endpoint *endpoints; /* List of endpoints */
    uint8_t num_endpoints;

    struct usb_controller host;

    struct usb_driver *driver; /* Attached driver */

    bool configured;
};
