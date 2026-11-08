#include "console.h"

#include "font.h"

#include <stdbool.h>

#define COM1 0x3F8
#define FONT_WIDTH 8
#define FONT_HEIGHT 8
#define CELL_HEIGHT 16
#define COLOR_FOREGROUND UINT32_C(0xFFFFFF)
#define COLOR_BACKGROUND UINT32_C(0x000000)

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL ConsoleProtocol;
static EFI_SIMPLE_TEXT_OUTPUT_MODE ConsoleMode;
static volatile uint32_t *Framebuffer;
static size_t FramebufferWidth;
static size_t FramebufferHeight;
static size_t PixelsPerScanLine;
static size_t ConsoleColumns;
static size_t ConsoleRows;
static size_t CursorColumn;
static size_t CursorRow;
static uint32_t PixelFormat;
static BOOT_PIXEL_MASK PixelMask;
static bool CursorVisible = true;
static char PreviousSerialCharacter;

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_initialize(void) {
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x80);
    port_out8(COM1 + 0, 0x01);
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x03);
    port_out8(COM1 + 2, 0xC7);
    port_out8(COM1 + 4, 0x0B);
}

void serial_write_character(char character) {
    unsigned int timeout = 100000;

    while ((port_in8(COM1 + 5) & 0x20) == 0 && timeout > 0) {
        --timeout;
    }
    if (timeout > 0) {
        port_out8(COM1, (uint8_t)character);
    }
}

void serial_write_string(const char *text) {
    while (*text != '\0') {
        if (*text == '\n') {
            serial_write_character('\r');
        }
        serial_write_character(*text++);
    }
}

static uint32_t component_from_mask(uint8_t component, uint32_t mask) {
    uint32_t shift = 0;
    uint32_t maximum;

    while (shift < 32 && ((mask >> shift) & 1) == 0) {
        ++shift;
    }
    maximum = mask >> shift;
    return maximum == 0
               ? 0
               : (((uint32_t)component * maximum + 127) / 255 << shift) &
                     mask;
}

static uint32_t encode_pixel(uint32_t color) {
    uint8_t red = (uint8_t)(color >> 16);
    uint8_t green = (uint8_t)(color >> 8);
    uint8_t blue = (uint8_t)color;

    if (PixelFormat == BOOT_INFO_PIXEL_RGB) {
        return (uint32_t)red | ((uint32_t)green << 8) |
               ((uint32_t)blue << 16);
    }
    if (PixelFormat == BOOT_INFO_PIXEL_BGR) {
        return (uint32_t)blue | ((uint32_t)green << 8) |
               ((uint32_t)red << 16);
    }
    return component_from_mask(red, PixelMask.red_mask) |
           component_from_mask(green, PixelMask.green_mask) |
           component_from_mask(blue, PixelMask.blue_mask);
}

static void fill_pixels(size_t x, size_t y, size_t width, size_t height,
                        uint32_t color) {
    uint32_t pixel = encode_pixel(color);

    for (size_t row = y; row < y + height; ++row) {
        volatile uint32_t *destination =
            Framebuffer + row * PixelsPerScanLine + x;
        for (size_t column = 0; column < width; ++column) {
            destination[column] = pixel;
        }
    }
}

static void redraw_cell(size_t column, size_t row, uint8_t character) {
    size_t x = column * FONT_WIDTH;
    size_t y = row * CELL_HEIGHT;
    uint8_t glyph = character < 128 ? character : '?';

    fill_pixels(x, y, FONT_WIDTH, CELL_HEIGHT, COLOR_BACKGROUND);
    for (size_t glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
        uint8_t bits = font8x8_basic[glyph][glyph_row];
        for (size_t glyph_column = 0; glyph_column < FONT_WIDTH;
             ++glyph_column) {
            if ((bits & (1u << glyph_column)) != 0) {
                fill_pixels(x + glyph_column, y + glyph_row * 2, 1, 2,
                            COLOR_FOREGROUND);
            }
        }
    }
}

static void scroll_console(void) {
    size_t scroll_pixels = CELL_HEIGHT;
    size_t row_count = FramebufferHeight - scroll_pixels;

    for (size_t row = 0; row < row_count; ++row) {
        volatile uint32_t *destination = Framebuffer + row * PixelsPerScanLine;
        volatile uint32_t *source =
            Framebuffer + (row + scroll_pixels) * PixelsPerScanLine;
        for (size_t column = 0; column < FramebufferWidth; ++column) {
            destination[column] = source[column];
        }
    }
    fill_pixels(0, row_count, FramebufferWidth, scroll_pixels,
                COLOR_BACKGROUND);
    if (CursorRow > 0) {
        --CursorRow;
    }
}

static void advance_line(void) {
    CursorColumn = 0;
    ++CursorRow;
    if (CursorRow >= ConsoleRows) {
        CursorRow = ConsoleRows - 1;
        scroll_console();
    }
}

static void draw_character(uint8_t character) {
    switch (character) {
    case '\r':
        CursorColumn = 0;
        return;
    case '\n':
        advance_line();
        return;
    case '\b':
        if (CursorColumn > 0) {
            --CursorColumn;
            redraw_cell(CursorColumn, CursorRow, ' ');
        }
        return;
    case '\t': {
        size_t spaces = 4 - CursorColumn % 4;
        while (spaces-- != 0) {
            draw_character(' ');
        }
        return;
    }
    default:
        break;
    }
    if (character < 0x20 || character > 0x7E) {
        return;
    }
    redraw_cell(CursorColumn, CursorRow, character);
    ++CursorColumn;
    if (CursorColumn >= ConsoleColumns) {
        advance_line();
    }
}

static EFI_STATUS EFIAPI console_reset(void *self, uint8_t extended) {
    (void)self;
    (void)extended;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_output_string(void *self, CHAR16 *text) {
    (void)self;
    if (text == NULL) {
        return EFI_ERROR_BIT | 2;
    }
    while (*text != 0) {
        CHAR16 character = *text++;
        if (character <= 0x7F) {
            draw_character((uint8_t)character);
            if (character == '\n' && PreviousSerialCharacter != '\r') {
                serial_write_character('\r');
            }
            if (character == '\b') {
                serial_write_character('\b');
                serial_write_character(' ');
                serial_write_character('\b');
            } else if (character == '\t') {
                serial_write_character('\t');
            } else if (character >= 0x20 && character <= 0x7E) {
                serial_write_character((char)character);
            } else if (character == '\r' || character == '\n') {
                serial_write_character((char)character);
            }
            PreviousSerialCharacter = (char)character;
        } else {
            draw_character('?');
            serial_write_character('?');
            PreviousSerialCharacter = '?';
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_test_string(void *self, CHAR16 *text) {
    (void)self;
    (void)text;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_query_mode(void *self, size_t mode,
                                             size_t *columns,
                                             size_t *rows) {
    (void)self;
    (void)mode;
    if (columns == NULL || rows == NULL) {
        return EFI_ERROR_BIT | 2;
    }
    *columns = ConsoleColumns;
    *rows = ConsoleRows;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_set_mode(void *self, size_t mode) {
    (void)self;
    (void)mode;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_set_attribute(void *self, size_t attribute) {
    (void)self;
    (void)attribute;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_clear_screen(void *self) {
    (void)self;
    CursorColumn = 0;
    CursorRow = 0;
    fill_pixels(0, 0, FramebufferWidth, FramebufferHeight, COLOR_BACKGROUND);
    serial_write_string("\x1b[2J\x1b[H");
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_set_cursor_position(void *self, size_t column,
                                                      size_t row) {
    (void)self;
    if (column >= ConsoleColumns || row >= ConsoleRows) {
        return EFI_ERROR_BIT | 2;
    }
    CursorColumn = column;
    CursorRow = row;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI console_enable_cursor(void *self, uint8_t visible) {
    (void)self;
    CursorVisible = visible != 0;
    return EFI_SUCCESS;
}

bool console_initialize(const BOOT_INFO *boot_info) {
    uint64_t pixel_count;
    uint64_t required_framebuffer_size;

    if (boot_info == NULL || boot_info->magic != BOOT_INFO_MAGIC ||
        boot_info->version != BOOT_INFO_VERSION ||
        boot_info->structure_size < sizeof(*boot_info) ||
        boot_info->framebuffer_base == 0 || boot_info->width < FONT_WIDTH ||
        boot_info->height < CELL_HEIGHT ||
        boot_info->pixels_per_scan_line < boot_info->width ||
        boot_info->pixel_format > BOOT_INFO_PIXEL_BITMASK) {
        return false;
    }
    if ((uint64_t)boot_info->pixels_per_scan_line >
            UINT64_MAX / boot_info->height ||
        (boot_info->pixel_format == BOOT_INFO_PIXEL_BITMASK &&
         (boot_info->pixel_mask.red_mask == 0 ||
          boot_info->pixel_mask.green_mask == 0 ||
          boot_info->pixel_mask.blue_mask == 0))) {
        return false;
    }
    pixel_count =
        (uint64_t)boot_info->pixels_per_scan_line * boot_info->height;
    if (pixel_count > UINT64_MAX / sizeof(uint32_t)) {
        return false;
    }
    required_framebuffer_size = pixel_count * sizeof(uint32_t);
    if (required_framebuffer_size > boot_info->framebuffer_size) {
        return false;
    }

    Framebuffer =
        reinterpret_cast<volatile uint32_t *>(boot_info->framebuffer_base);
    FramebufferWidth = boot_info->width;
    FramebufferHeight = boot_info->height;
    PixelsPerScanLine = boot_info->pixels_per_scan_line;
    ConsoleColumns = FramebufferWidth / FONT_WIDTH;
    ConsoleRows = FramebufferHeight / CELL_HEIGHT;
    PixelFormat = boot_info->pixel_format;
    PixelMask = boot_info->pixel_mask;
    CursorColumn = 0;
    CursorRow = 0;

    ConsoleProtocol.Reset = console_reset;
    ConsoleProtocol.OutputString = console_output_string;
    ConsoleProtocol.TestString = console_test_string;
    ConsoleProtocol.QueryMode = console_query_mode;
    ConsoleProtocol.SetMode = console_set_mode;
    ConsoleProtocol.SetAttribute = console_set_attribute;
    ConsoleProtocol.ClearScreen = console_clear_screen;
    ConsoleProtocol.SetCursorPosition = console_set_cursor_position;
    ConsoleProtocol.EnableCursor = console_enable_cursor;
    ConsoleProtocol.Mode = &ConsoleMode;
    ConsoleMode.MaxMode = 1;
    ConsoleMode.Mode = 0;
    ConsoleMode.CursorColumn = 0;
    ConsoleMode.CursorRow = 0;
    ConsoleMode.CursorVisible = 1;

    serial_initialize();
    fill_pixels(0, 0, FramebufferWidth, FramebufferHeight, COLOR_BACKGROUND);
    serial_write_string("[kernel] GOP framebuffer console initialized\n");
    return true;
}

EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console_output(void) {
    return &ConsoleProtocol;
}
