/* @title: USB HID */

/*
 * USB Human Interface Device (HID) class definitions
 * Spec: USB HID 1.11
 */
#pragma once

/* ================================
 * USB class / subclass / protocol
 * ================================ */

#define USB_CLASS_HID 0x03

/* HID subclasses */
#define USB_HID_SUBCLASS_NONE 0x00
#define USB_HID_SUBCLASS_BOOT 0x01

/* HID protocols (only meaningful for BOOT subclass) */
#define USB_HID_PROTOCOL_NONE 0x00
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE 0x02

/* ================================
 * Descriptor types
 * ================================ */
#define USB_HID_DESC_TYPE_HID 0x21
#define USB_HID_DESC_TYPE_REPORT 0x22
#define USB_HID_DESC_TYPE_PHYSICAL 0x23

/* ================================
 * HID class-specific requests
 * ================================ */
#define USB_HID_REQ_GET_REPORT 0x01
#define USB_HID_REQ_GET_IDLE 0x02
#define USB_HID_REQ_GET_PROTOCOL 0x03
#define USB_HID_REQ_SET_REPORT 0x09
#define USB_HID_REQ_SET_IDLE 0x0A
#define USB_HID_REQ_SET_PROTOCOL 0x0B

/* ================================
 * Report types (wValue high byte)
 * ================================ */
#define USB_HID_REPORT_INPUT 0x01
#define USB_HID_REPORT_OUTPUT 0x02
#define USB_HID_REPORT_FEATURE 0x03

/* ================================
 * Protocol values (SET_PROTOCOL)
 * ================================ */
#define USB_HID_PROTOCOL_BOOT 0x00
#define USB_HID_PROTOCOL_REPORT 0x01

/* ================================
 * Country codes (HID descriptor)
 * ================================ */
#define USB_HID_COUNTRY_NONE 0x00
#define USB_HID_COUNTRY_ARABIC 0x01
#define USB_HID_COUNTRY_BELGIAN 0x02
#define USB_HID_COUNTRY_CANADIAN_BI 0x03
#define USB_HID_COUNTRY_CANADIAN_FR 0x04
#define USB_HID_COUNTRY_CZECH 0x05
#define USB_HID_COUNTRY_DANISH 0x06
#define USB_HID_COUNTRY_FINNISH 0x07
#define USB_HID_COUNTRY_FRENCH 0x08
#define USB_HID_COUNTRY_GERMAN 0x09
#define USB_HID_COUNTRY_GREEK 0x0A
#define USB_HID_COUNTRY_HEBREW 0x0B
#define USB_HID_COUNTRY_HUNGARY 0x0C
#define USB_HID_COUNTRY_INTL_ISO 0x0D
#define USB_HID_COUNTRY_ITALIAN 0x0E
#define USB_HID_COUNTRY_JAPAN_KATA 0x0F
#define USB_HID_COUNTRY_KOREAN 0x10
#define USB_HID_COUNTRY_LATIN_AMER 0x11
#define USB_HID_COUNTRY_NETHERLANDS 0x12
#define USB_HID_COUNTRY_NORWEGIAN 0x13
#define USB_HID_COUNTRY_PERSIAN 0x14
#define USB_HID_COUNTRY_POLAND 0x15
#define USB_HID_COUNTRY_PORTUGUESE 0x16
#define USB_HID_COUNTRY_RUSSIA 0x17
#define USB_HID_COUNTRY_SLOVAKIA 0x18
#define USB_HID_COUNTRY_SPANISH 0x19
#define USB_HID_COUNTRY_SWEDISH 0x1A
#define USB_HID_COUNTRY_SWISS_FR 0x1B
#define USB_HID_COUNTRY_SWISS_DE 0x1C
#define USB_HID_COUNTRY_SWITZ_FR 0x1D
#define USB_HID_COUNTRY_TAIWAN 0x1E
#define USB_HID_COUNTRY_TURKISH_Q 0x1F
#define USB_HID_COUNTRY_UK 0x20
#define USB_HID_COUNTRY_US 0x21
#define USB_HID_COUNTRY_YUGOSLAVIA 0x22
#define USB_HID_COUNTRY_TURKISH_F 0x23

/* ================================
 * Boot keyboard definitions
 * ================================ */

/* Modifier byte bits */
#define USB_HID_MODIFIER_BASE 0xE0

#define USB_HID_MOD_LCTRL 0x01
#define USB_HID_MOD_LSHIFT 0x02
#define USB_HID_MOD_LALT 0x04
#define USB_HID_MOD_LGUI 0x08
#define USB_HID_MOD_RCTRL 0x10
#define USB_HID_MOD_RSHIFT 0x20
#define USB_HID_MOD_RALT 0x40
#define USB_HID_MOD_RGUI 0x80

/* LED output report bits */
#define USB_HID_LED_NUM_LOCK 0x01
#define USB_HID_LED_CAPS_LOCK 0x02
#define USB_HID_LED_SCROLL_LOCK 0x04
#define USB_HID_LED_COMPOSE 0x08
#define USB_HID_LED_KANA 0x10

/* ================================
 * Boot mouse definitions
 * ================================ */

#define USB_HID_MOUSE_BTN_LEFT 0x01
#define USB_HID_MOUSE_BTN_RIGHT 0x02
#define USB_HID_MOUSE_BTN_MIDDLE 0x04
#define USB_HID_MOUSE_BTN_BACK 0x08
#define USB_HID_MOUSE_BTN_FORWARD 0x10

/* ================================
 * Interface class-specific bmRequestType
 * ================================ */

#define USB_HID_REQTYPE_IN 0xA1  /* Class | Interface | IN  */
#define USB_HID_REQTYPE_OUT 0x21 /* Class | Interface | OUT */

/* ================================
 * Common HID interface expectations
 * ================================ */

/*
 * - Interrupt IN endpoint mandatory
 * - Interrupt OUT optional
 * - No bulk or isochronous endpoints
 * - Control endpoint used for reports & protocol
 */
