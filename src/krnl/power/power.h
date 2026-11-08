#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

#include <stdint.h>

#define KERNEL_POWER_OFF_NO_RETURN __attribute__((noreturn))

/* Initialize power-control hooks; ACPI table parsing is not yet implemented. */
void kernel_power_initialize(uint64_t acpi_root_table);
/* Ask supported emulators to power off; halts the CPU if control returns. */
KERNEL_POWER_OFF_NO_RETURN void kernel_power_off(void);

#endif
