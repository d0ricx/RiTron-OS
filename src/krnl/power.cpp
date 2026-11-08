#include "power.h"

#include "console.h"

#define QEMU_ACPI_SHUTDOWN_PORT 0x604
#define BOCHS_SHUTDOWN_PORT 0xB004
#define ACPI_SHUTDOWN_VALUE 0x2000

static void port_out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void kernel_power_initialize(uint64_t acpi_root_table) {
    (void)acpi_root_table;
    serial_write_string("[kernel] Native power control initialized\n");
}

KERNEL_POWER_OFF_NO_RETURN void kernel_power_off(void) {
    serial_write_string("[kernel] Requesting ACPI shutdown\n");
    port_out16(QEMU_ACPI_SHUTDOWN_PORT, ACPI_SHUTDOWN_VALUE);
    port_out16(BOCHS_SHUTDOWN_PORT, ACPI_SHUTDOWN_VALUE);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
