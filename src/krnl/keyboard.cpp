#include "keyboard.h"

#include <stdbool.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_COMMAND_READ_BYTE 0x20
#define PS2_COMMAND_WRITE_BYTE 0x60
#define PS2_COMMAND_ENABLE_KEYBOARD 0xAE
#define PS2_COMMAND_DISABLE_KEYBOARD 0xAD
#define PS2_KEYBOARD_ENABLE_SCANNING 0xF4
#define PS2_ACK 0xFA
#define PS2_TIMEOUT 1000000
#define KEY_QUEUE_CAPACITY 64

static EFI_SIMPLE_TEXT_INPUT_PROTOCOL KeyboardProtocol;
static EFI_INPUT_KEY KeyQueue[KEY_QUEUE_CAPACITY];
static volatile uint8_t QueueHead;
static volatile uint8_t QueueTail;
static bool LeftShift;
static bool RightShift;
static bool Control;
static bool CapsLock;
static bool ExtendedScanCode;

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static bool wait_input_clear(void) {
    for (uint32_t count = 0; count < PS2_TIMEOUT; ++count) {
        if ((port_in8(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t count = 0; count < PS2_TIMEOUT; ++count) {
        if ((port_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static bool controller_command(uint8_t command) {
    if (!wait_input_clear()) {
        return false;
    }
    port_out8(PS2_COMMAND_PORT, command);
    return true;
}

static bool keyboard_command(uint8_t command) {
    if (!wait_input_clear()) {
        return false;
    }
    port_out8(PS2_DATA_PORT, command);
    if (!wait_output_full()) {
        return false;
    }
    return port_in8(PS2_DATA_PORT) == PS2_ACK;
}

static uint8_t translate_scancode(uint8_t code, bool shifted) {
    static const char normal[128] = {
        0,    0,    '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',
        '0',  '-',  '=',  '\b', '\t', 'q',  'w',  'e',  'r',  't',  'y',
        'u',  'i',  'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',  'd',
        'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'', '`',  0,    '\\',
        'z',  'x',  'c',  'v',  'b',  'n',  'm',  ',',  '.',  '/',  0,
        '*',  0,    ' ',  0};
    static const char shifted_values[128] = {
        0,    0,    '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',
        ')',  '_',  '+',  '\b', '\t', 'Q',  'W',  'E',  'R',  'T',  'Y',
        'U',  'I',  'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',  'D',
        'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',  '~',  0,    '|',
        'Z',  'X',  'C',  'V',  'B',  'N',  'M',  '<',  '>',  '?',  0,
        '*',  0,    ' ',  0};
    uint8_t character = (uint8_t)(shifted ? shifted_values[code]
                                          : normal[code]);

    if (CapsLock && character >= 'a' && character <= 'z') {
        character = (uint8_t)(character - 'a' + 'A');
    } else if (CapsLock && character >= 'A' && character <= 'Z') {
        character = (uint8_t)(character - 'A' + 'a');
    }
    if (Control && character >= 'a' && character <= 'z') {
        character = (uint8_t)(character - 'a' + 1);
    } else if (Control && character >= 'A' && character <= 'Z') {
        character = (uint8_t)(character - 'A' + 1);
    }
    return character;
}

static EFI_STATUS EFIAPI keyboard_reset(void *self, uint8_t extended) {
    (void)self;
    (void)extended;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI keyboard_read_key(void *self, EFI_INPUT_KEY *key) {
    (void)self;
    if (key == NULL) {
        return EFI_ERROR_BIT | 2;
    }
    if (QueueTail == QueueHead) {
        return EFI_NOT_READY;
    }
    *key = KeyQueue[QueueTail];
    QueueTail = (uint8_t)((QueueTail + 1) & (KEY_QUEUE_CAPACITY - 1));
    return EFI_SUCCESS;
}

void keyboard_handle_interrupt(void) {
    uint8_t scan = port_in8(PS2_DATA_PORT);
    uint8_t next_head;
    EFI_INPUT_KEY key;
    bool released;

    if (scan == 0xE0 || scan == 0xE1) {
        ExtendedScanCode = scan == 0xE0;
        return;
    }

    released = (scan & 0x80) != 0;
    scan &= 0x7F;
    if (scan == 0x2A) {
        LeftShift = !released;
        return;
    }
    if (scan == 0x36) {
        RightShift = !released;
        return;
    }
    if (scan == 0x1D) {
        Control = !released;
        ExtendedScanCode = false;
        return;
    }
    if (!ExtendedScanCode && scan == 0x3A && !released) {
        CapsLock = !CapsLock;
        return;
    }
    key.ScanCode = 0;
    key.UnicodeChar = 0;
    if (released) {
        ExtendedScanCode = false;
        return;
    }
    if (ExtendedScanCode) {
        switch (scan) {
        case 0x48:
            key.ScanCode = EFI_SCAN_UP;
            break;
        case 0x50:
            key.ScanCode = EFI_SCAN_DOWN;
            break;
        case 0x4D:
            key.ScanCode = EFI_SCAN_RIGHT;
            break;
        case 0x4B:
            key.ScanCode = EFI_SCAN_LEFT;
            break;
        case 0x47:
            key.ScanCode = EFI_SCAN_HOME;
            break;
        case 0x4F:
            key.ScanCode = EFI_SCAN_END;
            break;
        case 0x53:
            key.ScanCode = EFI_SCAN_DELETE;
            break;
        default:
            break;
        }
        ExtendedScanCode = false;
        if (key.ScanCode == 0) {
            return;
        }
    } else {
        key.UnicodeChar =
            translate_scancode(scan, LeftShift || RightShift);
        if (key.UnicodeChar == 0) {
            return;
        }
    }

    next_head = (uint8_t)((QueueHead + 1) & (KEY_QUEUE_CAPACITY - 1));
    if (next_head != QueueTail) {
        KeyQueue[QueueHead] = key;
        QueueHead = next_head;
    }
}

bool keyboard_initialize(void) {
    uint8_t controller_configuration;

    if (!controller_command(PS2_COMMAND_DISABLE_KEYBOARD) ||
        !wait_input_clear()) {
        return false;
    }
    while ((port_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
        (void)port_in8(PS2_DATA_PORT);
    }
    if (!controller_command(PS2_COMMAND_READ_BYTE) || !wait_output_full()) {
        return false;
    }
    controller_configuration = port_in8(PS2_DATA_PORT);
    controller_configuration =
        (uint8_t)(controller_configuration | 0x41);
    if (!controller_command(PS2_COMMAND_WRITE_BYTE) || !wait_input_clear()) {
        return false;
    }
    port_out8(PS2_DATA_PORT, controller_configuration);
    if (!controller_command(PS2_COMMAND_ENABLE_KEYBOARD) ||
        !keyboard_command(PS2_KEYBOARD_ENABLE_SCANNING)) {
        return false;
    }

    KeyboardProtocol.Reset = keyboard_reset;
    KeyboardProtocol.ReadKeyStroke = keyboard_read_key;
    KeyboardProtocol.WaitForKey = NULL;
    LeftShift = false;
    RightShift = false;
    Control = false;
    CapsLock = false;
    ExtendedScanCode = false;
    QueueHead = 0;
    QueueTail = 0;
    serial_write_string("[kernel] PS/2 keyboard interrupt input initialized\n");
    return true;
}

EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keyboard_input(void) {
    return &KeyboardProtocol;
}

void kernel_stall(size_t microseconds) {
    uint64_t ticks = (microseconds * 100 + 999999) / 1000000;
    uint64_t deadline = timer_ticks() + (ticks == 0 ? 1 : ticks);

    while ((int64_t)(timer_ticks() - deadline) < 0) {
        __asm__ volatile("hlt");
    }
}
