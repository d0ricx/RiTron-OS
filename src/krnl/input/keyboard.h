#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

#include "console.h"
#include "cpu.h"

#define EFI_SCAN_UP 0x01
#define EFI_SCAN_DOWN 0x02
#define EFI_SCAN_RIGHT 0x03
#define EFI_SCAN_LEFT 0x04
#define EFI_SCAN_HOME 0x05
#define EFI_SCAN_END 0x06
#define EFI_SCAN_DELETE 0x08

/* Configure the PS/2 controller and expose its interrupt queue as UEFI input. */
bool keyboard_initialize(void);
EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keyboard_input(void);
/* Sleep using PIT ticks; requires interrupts to be enabled. */
void kernel_stall(size_t microseconds);

#endif
