#include "cpu.h"

#include "console.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} IDT_GATE;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDT_REGISTER;

static IDT_GATE InterruptDescriptorTable[256] __attribute__((aligned(16)));
static volatile uint64_t TimerTicks;

static void set_gate(uint8_t vector, uintptr_t handler) {
    uint16_t code_segment;
    IDT_GATE *gate = &InterruptDescriptorTable[vector];

    __asm__ volatile("mov %%cs, %0" : "=r"(code_segment));
    // The architectural 64-bit handler address is split across three IDT fields.
    gate->offset_low = (uint16_t)handler;
    gate->selector = code_segment;
    gate->ist = 0;
    gate->attributes = 0x8E;
    gate->offset_middle = (uint16_t)(handler >> 16);
    gate->offset_high = (uint32_t)(handler >> 32);
    gate->reserved = 0;
}

void interrupt_set_gate(uint8_t vector, void (*handler)(void)) {
    set_gate(vector, (uintptr_t)handler);
}

static void report_exception(uint32_t vector, uint64_t error_code,
                             uint64_t instruction_pointer) {
    static const char *const names[32] = {
        "divide error", "debug", "non-maskable interrupt", "breakpoint",
        "overflow", "bound range", "invalid opcode", "device unavailable",
        "double fault", "coprocessor segment overrun", "invalid TSS",
        "segment not present", "stack fault", "general protection fault",
        "page fault", "reserved", "x87 floating point", "alignment check",
        "machine check", "SIMD floating point", "virtualization",
        "control protection", "reserved", "reserved", "reserved", "reserved",
        "reserved", "reserved", "hypervisor injection", "VMM communication",
        "security", "reserved"};
    static const char digits[] = "0123456789ABCDEF";

    // This kernel cannot recover yet, so capture the fault details before halting.
    serial_write_string("[exception] ");
    serial_write_string(vector < 32 ? names[vector] : "unknown");
    serial_write_string(" vector=");
    for (int shift = 4; shift >= 0; --shift) {
        serial_write_character(digits[(vector >> (shift * 4)) & 0xF]);
    }
    serial_write_string(" error=0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_character(digits[(error_code >> shift) & 0xF]);
    }
    serial_write_string(" rip=0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_character(digits[(instruction_pointer >> shift) & 0xF]);
    }
    if (vector == 14) {
        uint64_t fault_address;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
        serial_write_string(" cr2=0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            serial_write_character(digits[(fault_address >> shift) & 0xF]);
        }
    }
    serial_write_string("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

#define EXCEPTION_NO_ERROR(number)                                             \
    static void __attribute__((interrupt)) exception_##number(                \
        INTERRUPT_FRAME *frame) {                                              \
        report_exception(number, 0, frame->instruction_pointer);               \
    }

#define EXCEPTION_WITH_ERROR(number)                                           \
    static void __attribute__((interrupt)) exception_##number(                \
        INTERRUPT_FRAME *frame, uint64_t error_code) {                         \
        report_exception(number, error_code, frame->instruction_pointer);      \
    }

EXCEPTION_NO_ERROR(0)
EXCEPTION_NO_ERROR(1)
EXCEPTION_NO_ERROR(2)
EXCEPTION_NO_ERROR(3)
EXCEPTION_NO_ERROR(4)
EXCEPTION_NO_ERROR(5)
EXCEPTION_NO_ERROR(6)
EXCEPTION_NO_ERROR(7)
EXCEPTION_WITH_ERROR(8)
EXCEPTION_NO_ERROR(9)
EXCEPTION_WITH_ERROR(10)
EXCEPTION_WITH_ERROR(11)
EXCEPTION_WITH_ERROR(12)
EXCEPTION_WITH_ERROR(13)
EXCEPTION_WITH_ERROR(14)
EXCEPTION_NO_ERROR(15)
EXCEPTION_NO_ERROR(16)
EXCEPTION_WITH_ERROR(17)
EXCEPTION_NO_ERROR(18)
EXCEPTION_NO_ERROR(19)
EXCEPTION_NO_ERROR(20)
EXCEPTION_WITH_ERROR(21)
EXCEPTION_NO_ERROR(22)
EXCEPTION_NO_ERROR(23)
EXCEPTION_NO_ERROR(24)
EXCEPTION_NO_ERROR(25)
EXCEPTION_NO_ERROR(26)
EXCEPTION_NO_ERROR(27)
EXCEPTION_NO_ERROR(28)
EXCEPTION_WITH_ERROR(29)
EXCEPTION_WITH_ERROR(30)
EXCEPTION_NO_ERROR(31)

bool exceptions_initialize(void) {
    IDT_REGISTER idtr;
    uintptr_t handlers[32] = {
        (uintptr_t)exception_0,  (uintptr_t)exception_1,
        (uintptr_t)exception_2,  (uintptr_t)exception_3,
        (uintptr_t)exception_4,  (uintptr_t)exception_5,
        (uintptr_t)exception_6,  (uintptr_t)exception_7,
        (uintptr_t)exception_8,  (uintptr_t)exception_9,
        (uintptr_t)exception_10, (uintptr_t)exception_11,
        (uintptr_t)exception_12, (uintptr_t)exception_13,
        (uintptr_t)exception_14, (uintptr_t)exception_15,
        (uintptr_t)exception_16, (uintptr_t)exception_17,
        (uintptr_t)exception_18, (uintptr_t)exception_19,
        (uintptr_t)exception_20, (uintptr_t)exception_21,
        (uintptr_t)exception_22, (uintptr_t)exception_23,
        (uintptr_t)exception_24, (uintptr_t)exception_25,
        (uintptr_t)exception_26, (uintptr_t)exception_27,
        (uintptr_t)exception_28, (uintptr_t)exception_29,
        (uintptr_t)exception_30, (uintptr_t)exception_31};

    // Keep IF clear until all vectors point at valid handlers.
    __asm__ volatile("cli");
    for (uint16_t vector = 0; vector < 256; ++vector) {
        set_gate((uint8_t)vector, (uintptr_t)exception_31);
    }
    for (uint8_t vector = 0; vector < 32; ++vector) {
        set_gate(vector, handlers[vector]);
    }
    idtr.limit = sizeof(InterruptDescriptorTable) - 1;
    idtr.base = (uint64_t)(uintptr_t)InterruptDescriptorTable;
    __asm__ volatile("lidt %0" : : "m"(idtr));
    serial_write_string("[kernel] IDT exception handlers installed\n");
    return true;
}

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void pic_remap(void) {
    uint8_t master_mask = port_in8(0x21);
    uint8_t slave_mask = port_in8(0xA1);

    // Move PIC IRQ vectors above CPU exception vectors, preserving mask state.
    port_out8(0x20, 0x11);
    port_out8(0xA0, 0x11);
    port_out8(0x21, 0x20);
    port_out8(0xA1, 0x28);
    port_out8(0x21, 0x04);
    port_out8(0xA1, 0x02);
    port_out8(0x21, 0x01);
    port_out8(0xA1, 0x01);
    port_out8(0x21, master_mask);
    port_out8(0xA1, slave_mask);
}

static void program_pit(void) {
    const uint16_t divisor = 11932;

    // Channel 0, low/high divisor, square-wave mode; approximately 100 Hz.
    port_out8(0x43, 0x36);
    port_out8(0x40, (uint8_t)divisor);
    port_out8(0x40, (uint8_t)(divisor >> 8));
}

static void irq0_handler(INTERRUPT_FRAME *frame)
    __attribute__((interrupt));
static void irq0_handler(INTERRUPT_FRAME *frame) {
    (void)frame;
    // PIT ticks provide the kernel's coarse delay clock; acknowledge the master PIC.
    TimerTicks = TimerTicks + 1;
    port_out8(0x20, 0x20);
}

static void irq1_handler(INTERRUPT_FRAME *frame)
    __attribute__((interrupt));
static void irq1_handler(INTERRUPT_FRAME *frame) {
    (void)frame;
    extern void keyboard_handle_interrupt(void);
    keyboard_handle_interrupt();
    port_out8(0x20, 0x20);
}

bool interrupts_initialize(void) {
    // Configure vectors and masks before enabling any hardware interrupt.
    __asm__ volatile("cli");
    pic_remap();
    set_gate(32, (uintptr_t)irq0_handler);
    set_gate(33, (uintptr_t)irq1_handler);
    program_pit();
    port_out8(0x21, 0xFC);
    port_out8(0xA1, 0xFF);
    __asm__ volatile("sti");
    serial_write_string("[kernel] PIC and PIT interrupts initialized\n");
    return true;
}

void interrupt_enable_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t mask = port_in8(0x21);
        port_out8(0x21, (uint8_t)(mask & ~(1u << irq)));
    } else if (irq < 16) {
        uint8_t slave_mask = port_in8(0xA1);
        uint8_t master_mask = port_in8(0x21);
        port_out8(0xA1, (uint8_t)(slave_mask & ~(1u << (irq - 8))));
        port_out8(0x21, (uint8_t)(master_mask & ~(1u << 2)));
    }
}

uint64_t timer_ticks(void) {
    return TimerTicks;
}
