#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include "bootinfo.h"

#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef uint16_t CHAR16;

#define EFI_SUCCESS UINT64_C(0)
#define EFI_ERROR_BIT UINT64_C(0x8000000000000000)
#define EFI_NOT_READY (EFI_ERROR_BIT | 6)
#define EFI_ERROR(status) (((EFI_STATUS)(status) >> 63) != 0)

typedef struct {
    uint16_t ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct {
    int32_t MaxMode;
    int32_t Mode;
    int32_t Attribute;
    int32_t CursorColumn;
    int32_t CursorRow;
    uint8_t CursorVisible;
} EFI_SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *Reset)(void *, uint8_t);
    EFI_STATUS(EFIAPI *ReadKeyStroke)(void *, EFI_INPUT_KEY *);
    void *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *Reset)(void *, uint8_t);
    EFI_STATUS(EFIAPI *OutputString)(void *, CHAR16 *);
    EFI_STATUS(EFIAPI *TestString)(void *, CHAR16 *);
    EFI_STATUS(EFIAPI *QueryMode)(void *, size_t, size_t *, size_t *);
    EFI_STATUS(EFIAPI *SetMode)(void *, size_t);
    EFI_STATUS(EFIAPI *SetAttribute)(void *, size_t);
    EFI_STATUS(EFIAPI *ClearScreen)(void *);
    EFI_STATUS(EFIAPI *SetCursorPosition)(void *, size_t, size_t);
    EFI_STATUS(EFIAPI *EnableCursor)(void *, uint8_t);
    EFI_SIMPLE_TEXT_OUTPUT_MODE *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Validate the GOP handoff, initialize framebuffer output, and set up COM1. */
bool console_initialize(const BOOT_INFO *boot_info);
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console_output(void);
/* Read the framebuffer text cursor; either output pointer may be null. */
void console_get_cursor_position(size_t *column, size_t *row);
/* Draw one printable byte directly to the framebuffer, without serial echo. */
void console_redraw_character(char character);
/* Initialize and write to the kernel's COM1 diagnostic stream. */
void serial_initialize(void);
void serial_write_character(char character);
void serial_write_string(const char *text);

#endif
