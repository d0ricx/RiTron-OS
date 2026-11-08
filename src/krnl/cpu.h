#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>

typedef struct {
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
    uint64_t stack_pointer;
    uint64_t stack_segment;
} INTERRUPT_FRAME;

bool exceptions_initialize(void);
bool interrupts_initialize(void);
void interrupt_enable_irq(uint8_t irq);
uint64_t timer_ticks(void);
void interrupt_set_gate(uint8_t vector, void (*handler)(void));

#endif
