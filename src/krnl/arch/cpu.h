#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>

typedef struct {
    /* CPU-generated frame fields; privilege-transition fields may be absent. */
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
    uint64_t stack_pointer;
    uint64_t stack_segment;
} INTERRUPT_FRAME;

/* Install fatal handlers for CPU exceptions in the IDT. */
bool exceptions_initialize(void);
/* Remap the legacy PIC, start the PIT clock, and enable timer/keyboard IRQs. */
bool interrupts_initialize(void);
/* Unmask a legacy PIC interrupt line (0-15). */
void interrupt_enable_irq(uint8_t irq);
/* Return PIT ticks since interrupt initialization. */
uint64_t timer_ticks(void);
/* Replace one IDT vector; intended for kernel-owned interrupt handlers. */
void interrupt_set_gate(uint8_t vector, void (*handler)(void));

#endif
