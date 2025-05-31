#include <misc/dbg.h>
#include <asm.h>
#include <console/printf.h>
#include <stdint.h>
#include <string.h>
#define KB_DOWN_LSHIFT 0x2A
#define KB_UP_LSHIFT 0xAA
#define KB_CAPSLOCK 0x3A
#define KB_STATUS_PORT 0x64
#define KB_DATA_PORT 0x60

uint8_t shift = 1;
uint16_t index = 0;
char kbuffer[512] = {0};
unsigned char keyboard_map[128] = {
    '\0', '\e', '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',
    '-',  '=',  '\b', '\t', 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    'o',  'p',  '[',  ']',  '\n', '\0', 'a',  's',  'd',  'f',  'g',  'h',
    'j',  'k',  'l',  ';',  '\'', '`',  '\0', '\\', 'z',  'x',  'c',  'v',
    'b',  'n',  'm',  ',',  '.',  '/',  '\0', '*',  '\0', ' ',  '\0', '\0',
    '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '7',
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  '2',  '3',  '0',  '.',
    '\0', '\0', '\0', '\0', '\0'};

unsigned char keyboard_shift_map[128] = {
    '\0', '\e', '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
    '_',  '+',  '\b', '\t', 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    'O',  'P',  '{',  '}',  '\n', '\0', 'A',  'S',  'D',  'F',  'G',  'H',
    'J',  'K',  'L',  ':',  '"',  '`',  '\0', '|',  'Z',  'X',  'C',  'V',
    'B',  'N',  'M',  '<',  '>',  '?',  '\0', '*',  '\0', ' ',  '\0', '\0',
    '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '7',
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  '2',  '3',  '0',  '.',
    '\0', '\0', '\0', '\0', '\0'};

__attribute__((interrupt)) void keyboard_handler(void *a) {
    (void) a;
    uint8_t status;
    do {
        status = inb(KB_STATUS_PORT);
    } while ((status & 1) == 0);

    uint8_t keycode = inb(KB_DATA_PORT);

    if (keycode == KB_DOWN_LSHIFT || keycode == KB_UP_LSHIFT ||
        keycode == KB_CAPSLOCK) {
        shift = !shift;
        goto finish;
    }

    if (keycode < sizeof(keyboard_map) && keyboard_map[keycode] != 0) {
        char character =
            shift ? keyboard_map[keycode] : keyboard_shift_map[keycode];
        k_printf("%c", character);
        if (character == '\n') {
            if (memcmp(kbuffer, "shutdown", 8) == 0 ||
                memcmp(kbuffer, "poweroff", 8) == 0) {
                k_info("Poweroff signal received\n");
            } else if (memcmp(kbuffer, ":3", 2) == 0) {
                k_info("silly!");
            }
            memset(kbuffer, 0, sizeof(kbuffer));
            index = 0;
        } else {
            if (character == '1') {
                debug_print_registers();
                asm volatile("cli;hlt");
            }
            kbuffer[index++] = character;
        }
    }
finish:
    outb(0x20, 0x20);
    outb(0xA0, 0x20);
}
