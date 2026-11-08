#include "ata.h"
#include "cpu.h"
#include "e1000.h"
#include "fat16.h"
#include "keyboard.h"
#include "memory.h"
#include "power.h"

#include <stddef.h>
#include <stdint.h>

// The shell owns command-line editing and the full-screen editor. Large fixed
// buffers avoid dynamic allocation in early kernel startup and interrupt paths.
#define SHELL_LINE_CAPACITY 600
#define SHELL_ARGUMENT_COUNT 4
#define SHELL_HISTORY_CAPACITY 8
#define EDITOR_BUFFER_CAPACITY 65536
#define EDITOR_TAB_WIDTH 4

typedef void (*KERNEL_STALL)(size_t);
typedef void (*KERNEL_POWER_OFF)(void);

extern "C" int32_t daneels_ritron_http_get(
    uint32_t remote_ip, uint16_t port, const uint8_t *path,
    size_t path_length, uint8_t *output, size_t output_capacity,
    size_t *output_length, uint16_t *http_status,
    void (*stall)(size_t));

static bool FilesystemMounted;
// Large editor/completion buffers live statically to avoid exhausting the kernel stack.
static char EditorBuffer[EDITOR_BUFFER_CAPACITY];
static char ShellPathBuffer[17 * 256 + 2];
static char ShellHistory[SHELL_HISTORY_CAPACITY][SHELL_LINE_CAPACITY];
static size_t ShellHistoryCount;
static size_t ShellHistoryNext;

static const char *filesystem_result_message(FAT16_RESULT result);

static void serial_write_status(EFI_STATUS status) {
    static const char digits[] = "0123456789ABCDEF";

    serial_write_string("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_character(digits[(status >> shift) & 0xF]);
    }
}

static void serial_log_error(const char *operation, EFI_STATUS status) {
    serial_write_string("[kernel] ERROR: ");
    serial_write_string(operation);
    serial_write_string(" returned ");
    serial_write_status(status);
    serial_write_string("\n");
}

static void serial_log_hex(const char *label, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    serial_write_string(label);
    serial_write_string("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_character(digits[(value >> shift) & 0xF]);
    }
    serial_write_string("\n");
}

static void serial_log_filesystem_error(FAT16_RESULT result) {
    serial_write_string("[kernel] ERROR: FAT16 operation failed (");
    serial_write_character((char)('0' + result / 10));
    serial_write_character((char)('0' + result % 10));
    serial_write_string(")\n");
}

static bool ata_read_callback(void *context, uint32_t lba,
                              uint32_t block_count, void *buffer) {
    (void)context;
    return ata_pio_read_blocks(lba, block_count, buffer);
}

static bool ata_write_callback(void *context, uint32_t lba,
                               uint32_t block_count, const void *buffer) {
    (void)context;
    return ata_pio_write_blocks(lba, block_count, buffer);
}

static bool ata_flush_callback(void *context) {
    (void)context;
    return ata_pio_flush();
}

static EFI_STATUS console_write_character(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, CHAR16 character) {
    CHAR16 text[] = {character, 0};
    return console->OutputString(console, text);
}

static EFI_STATUS console_write_ascii(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, const char *text) {
    EFI_STATUS status;

    while (*text != '\0') {
        status = console_write_character(console, (uint8_t)*text++);
        if (EFI_ERROR(status)) {
            return status;
        }
    }
    return EFI_SUCCESS;
}

static bool parse_http_endpoint(const char *text, uint32_t *address,
                               uint16_t *port) {
    uint32_t parsed_address = 0;
    uint32_t parsed_port = 80;
    const char *cursor = text;

    // Parse exactly four decimal octets; reject overflow instead of truncating.
    for (size_t octet = 0; octet < 4; ++octet) {
        uint32_t value = 0;
        size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            uint32_t digit = (uint32_t)(*cursor++ - '0');
            if (value > (255 - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            ++digits;
        }
        if (digits == 0) {
            return false;
        }
        parsed_address = (parsed_address << 8) | value;
        if (octet != 3) {
            if (*cursor++ != '.') {
                return false;
            }
        }
    }

    if (*cursor == ':') {
        ++cursor;
        parsed_port = 0;
        size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            uint32_t digit = (uint32_t)(*cursor++ - '0');
            if (parsed_port > (65535 - digit) / 10) {
                return false;
            }
            parsed_port = parsed_port * 10 + digit;
            ++digits;
        }
        if (digits == 0 || parsed_port == 0) {
            return false;
        }
    }
    if (*cursor != '\0') {
        return false;
    }
    *address = parsed_address;
    *port = (uint16_t)parsed_port;
    return true;
}

static const char *http_client_error(int32_t result) {
    switch (result) {
    case 1:
        return "Invalid address, port, or HTTP path.\r\n";
    case 2:
        return "Network is not configured; check DHCP with ip.\r\n";
    case 3:
        return "Could not resolve the next-hop MAC address (ARP timeout).\r\n";
    case 4:
        return "TCP connection or HTTP response timed out.\r\n";
    case 5:
        return "The remote host reset the TCP connection.\r\n";
    case 6:
        return "The server returned an invalid HTTP response.\r\n";
    case 7:
        return "HTTP response exceeded the 64 KiB buffer.\r\n";
    case 8:
        return "The network adapter could not transmit a frame.\r\n";
    default:
        return "HTTP request failed.\r\n";
    }
}

static EFI_STATUS console_write_decimal(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        EFI_STATUS status =
            console_write_character(console, (uint8_t)digits[--count]);
        if (EFI_ERROR(status)) {
            return status;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS console_write_ipv4(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, uint32_t address) {
    EFI_STATUS status;
    for (int shift = 24; shift >= 0; shift -= 8) {
        status = console_write_decimal(console, (uint8_t)(address >> shift));
        if (EFI_ERROR(status)) {
            return status;
        }
        if (shift != 0) {
            status = console_write_character(console, '.');
            if (EFI_ERROR(status)) {
                return status;
            }
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS print_network_configuration(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output) {
    const E1000_NETWORK_CONFIG *config = e1000_network_config();
    EFI_STATUS status;

    if (!e1000_is_initialized()) {
        return console_write_ascii(
            output, "No supported Intel e1000 adapter detected.\r\n");
    }
    status = console_write_ascii(output, "e1000 MAC: ");

    for (size_t index = 0;
         index < sizeof(config->mac_address) && !EFI_ERROR(status); ++index) {
        static const char digits[] = "0123456789ABCDEF";
        char octet[] = {digits[config->mac_address[index] >> 4],
                        digits[config->mac_address[index] & 0x0F]};
        CHAR16 text[] = {(uint8_t)octet[0], (uint8_t)octet[1], 0};
        status = output->OutputString(output, text);
        if (!EFI_ERROR(status) && index + 1 < sizeof(config->mac_address)) {
            status = console_write_character(output, ':');
        }
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, "\r\nIPv4: ");
    }
    if (EFI_ERROR(status)) {
        return status;
    }
    if (!e1000_has_ipv4_address()) {
        return console_write_ascii(output,
                                   "No DHCP address. Check the QEMU network.\r\n");
    }
    status = console_write_ipv4(output, config->ipv4_address);
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, "\r\nSubnet: ");
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ipv4(output, config->subnet_mask);
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, "\r\nGateway: ");
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ipv4(output, config->gateway);
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, "\r\nDNS: ");
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ipv4(output, config->dns_server);
    }
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, "\r\n");
    }
    return status;
}

static int strings_equal(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static size_t string_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void advance_editor_position(char character, size_t columns,
                                    size_t *row, size_t *column) {
    if (character == '\n') {
        ++*row;
        *column = 0;
        return;
    }

    // Tabs occupy the next fixed-width stop; other printable bytes use one cell.
    size_t cells = character == '\t'
                       ? EDITOR_TAB_WIDTH - (*column % EDITOR_TAB_WIDTH)
                       : 1;
    while (cells-- != 0) {
        ++*column;
        if (*column >= columns) {
            ++*row;
            *column = 0;
        }
    }
}

static void editor_position_at(const char *buffer, size_t length,
                               size_t columns, size_t *row,
                               size_t *column) {
    *row = 0;
    *column = 0;
    for (size_t index = 0; index < length; ++index) {
        advance_editor_position(buffer[index], columns, row, column);
    }
}

static EFI_STATUS editor_render(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                                size_t buffer_length, size_t cursor,
                                const char *filename, bool modified) {
    static const char footer[] =
        "Ctrl+S Save   Ctrl+X Close   Tab Insert tab";
    size_t columns;
    size_t rows;
    size_t cursor_row;
    size_t cursor_column;
    size_t visible_rows;
    size_t first_visible_row;
    size_t row = 0;
    size_t column = 0;
    EFI_STATUS status;

    if (output->Mode == NULL || output->Mode->Mode < 0 ||
        output->QueryMode == NULL || output->SetCursorPosition == NULL) {
        return EFI_ERROR_BIT | 2;
    }
    status = output->QueryMode(output, (size_t)output->Mode->Mode, &columns,
                               &rows);
    if (EFI_ERROR(status) || columns == 0 || rows < 3) {
        return EFI_ERROR(status) ? status : EFI_ERROR_BIT | 2;
    }
    // Reserve the first and last terminal rows for the title and key legend.
    visible_rows = rows - 2;
    editor_position_at(EditorBuffer, cursor, columns, &cursor_row,
                       &cursor_column);
    // Keep the cursor visible by scrolling the rendered window, not the buffer.
    first_visible_row =
        cursor_row >= visible_rows ? cursor_row - visible_rows + 1 : 0;

    status = output->ClearScreen(output);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = output->SetCursorPosition(output, 0, 0);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = console_write_ascii(output, "Editing: ");
    if (!EFI_ERROR(status)) {
        status = console_write_ascii(output, filename);
    }
    if (!EFI_ERROR(status) && modified) {
        status = console_write_ascii(output, " [modified]");
    }
    if (EFI_ERROR(status)) {
        return status;
    }

    for (size_t index = 0; index < buffer_length; ++index) {
        char character = EditorBuffer[index];
        size_t cells = character == '\t'
                           ? EDITOR_TAB_WIDTH -
                                 (column % EDITOR_TAB_WIDTH)
                           : character == '\n' ? 0 : 1;

        if (character == '\n') {
            ++row;
            column = 0;
            continue;
        }
        while (cells-- != 0) {
            if (row >= first_visible_row &&
                row < first_visible_row + visible_rows) {
                status = output->SetCursorPosition(
                    output, column, row - first_visible_row + 1);
                if (EFI_ERROR(status)) {
                    return status;
                }
                status = console_write_character(
                    output, character == '\t' ? ' ' : (uint8_t)character);
                if (EFI_ERROR(status)) {
                    return status;
                }
            }
            ++column;
            if (column >= columns) {
                ++row;
                column = 0;
            }
        }
    }

    status = output->SetCursorPosition(output, 0, rows - 1);
    if (EFI_ERROR(status)) {
        return status;
    }
    for (size_t index = 0; footer[index] != '\0' && index < columns;
         ++index) {
        status = console_write_character(output, (uint8_t)footer[index]);
        if (EFI_ERROR(status)) {
            return status;
        }
    }
    status = output->SetCursorPosition(output, cursor_column,
                                       cursor_row - first_visible_row + 1);
    return status;
}

static bool editor_insert(char value, size_t *length, size_t *cursor) {
    if (*length >= EDITOR_BUFFER_CAPACITY) {
        return false;
    }
    // Shift the suffix right in reverse order so insertion does not overwrite it.
    for (size_t index = *length; index > *cursor; --index) {
        EditorBuffer[index] = EditorBuffer[index - 1];
    }
    EditorBuffer[*cursor] = value;
    ++*length;
    ++*cursor;
    return true;
}

static void editor_remove(size_t *length, size_t *cursor, bool forward) {
    size_t remove_at;

    if (forward) {
        if (*cursor >= *length) {
            return;
        }
        remove_at = *cursor;
    } else {
        if (*cursor == 0) {
            return;
        }
        remove_at = *cursor - 1;
        --*cursor;
    }
    // Compact the remaining text over the removed character.
    for (size_t index = remove_at; index + 1 < *length; ++index) {
        EditorBuffer[index] = EditorBuffer[index + 1];
    }
    --*length;
}

static size_t editor_line_start(size_t cursor) {
    while (cursor > 0 && EditorBuffer[cursor - 1] != '\n') {
        --cursor;
    }
    return cursor;
}

static size_t editor_line_end(size_t cursor, size_t length) {
    while (cursor < length && EditorBuffer[cursor] != '\n') {
        ++cursor;
    }
    return cursor;
}

static void editor_move_vertical(size_t *cursor, size_t length, int direction) {
    size_t current_start = editor_line_start(*cursor);
    size_t current_column = *cursor - current_start;
    size_t target;
    size_t target_end;

    if (direction < 0) {
        if (current_start == 0) {
            return;
        }
        target_end = current_start - 1;
        target = editor_line_start(target_end);
    } else {
        size_t current_end = editor_line_end(*cursor, length);
        if (current_end >= length) {
            return;
        }
        target = current_end + 1;
        target_end = editor_line_end(target, length);
    }
    // Preserve the horizontal column where possible, clamping on short lines.
    *cursor = target + (current_column < target_end - target
                            ? current_column
                            : target_end - target);
}

static EFI_STATUS editor_wait_for_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                                      KERNEL_STALL stall, EFI_INPUT_KEY *key) {
    EFI_STATUS status;
    // Input is interrupt-driven; sleep briefly instead of busy-spinning.
    for (;;) {
        status = input->ReadKeyStroke(input, key);
        if (status != EFI_NOT_READY) {
            return status;
        }
        stall(10000);
    }
}

static EFI_STATUS run_editor(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                             EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                             KERNEL_STALL stall, const char *filename,
                             uint32_t initial_size) {
    size_t length = initial_size;
    size_t cursor = length;
    bool modified = false;
    EFI_STATUS status;

    // The buffer is preloaded by the caller; this loop edits until save or close.
    status = editor_render(output, length, cursor, filename, modified);
    if (EFI_ERROR(status)) {
        serial_log_error("render editor", status);
        return status;
    }

    for (;;) {
        EFI_INPUT_KEY key;
        size_t old_length = length;
        size_t old_cursor = cursor;
        bool force_redraw = false;

        status = editor_wait_for_key(input, stall, &key);
        if (EFI_ERROR(status)) {
            serial_log_error("ReadKeyStroke(editor)", status);
            return status;
        }

        if (key.UnicodeChar == 0x13) {
            FAT16_RESULT result =
                fat16_write_file(filename, EditorBuffer, (uint32_t)length);
            if (result != FAT16_OK) {
                serial_log_filesystem_error(result);
                status = output->SetCursorPosition(output, 0, 0);
                if (!EFI_ERROR(status)) {
                    status = console_write_ascii(
                        output, filesystem_result_message(result));
                }
                if (EFI_ERROR(status)) {
                    serial_log_error("show save error", status);
                    return status;
                }
            } else {
                modified = false;
                force_redraw = true;
                serial_write_string("[kernel] saved file: ");
                serial_write_string(filename);
                serial_write_string("\n");
            }
        } else if (key.UnicodeChar == 0x18) {
            if (modified) {
                serial_write_string(
                    "[kernel] closed file with unsaved edits: ");
                serial_write_string(filename);
                serial_write_string("\n");
            }
            status = output->ClearScreen(output);
            if (!EFI_ERROR(status)) {
                status = console_write_ascii(
                    output, modified ? "Closed; unsaved edits discarded.\r\n"
                                     : "Closed.\r\n");
            }
            return status;
        } else if (key.UnicodeChar == '\b') {
            editor_remove(&length, &cursor, false);
        } else if (key.ScanCode == EFI_SCAN_DELETE) {
            editor_remove(&length, &cursor, true);
        } else if (key.ScanCode == EFI_SCAN_LEFT) {
            if (cursor > 0) {
                --cursor;
            }
        } else if (key.ScanCode == EFI_SCAN_RIGHT) {
            if (cursor < length) {
                ++cursor;
            }
        } else if (key.ScanCode == EFI_SCAN_HOME) {
            cursor = editor_line_start(cursor);
        } else if (key.ScanCode == EFI_SCAN_END) {
            cursor = editor_line_end(cursor, length);
        } else if (key.ScanCode == EFI_SCAN_UP) {
            editor_move_vertical(&cursor, length, -1);
        } else if (key.ScanCode == EFI_SCAN_DOWN) {
            editor_move_vertical(&cursor, length, 1);
        } else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            (void)editor_insert('\n', &length, &cursor);
        } else if (key.UnicodeChar == '\t') {
            (void)editor_insert('\t', &length, &cursor);
        } else if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            (void)editor_insert((char)key.UnicodeChar, &length, &cursor);
        }

        if (force_redraw || length != old_length || cursor != old_cursor) {
            modified = modified || length != old_length;
            status = editor_render(output, length, cursor, filename, modified);
            if (EFI_ERROR(status)) {
                serial_log_error("render editor", status);
                return status;
            }
        }
    }
}

static FAT16_RESULT ensure_directory(const char *name) {
    FAT16_RESULT result = fat16_change_directory(name);

    if (result == FAT16_NOT_FOUND) {
        result = fat16_make_directory(name);
        if (result != FAT16_OK) {
            return result;
        }
        result = fat16_change_directory(name);
    }
    if (result != FAT16_OK) {
        return result;
    }
    return fat16_change_directory("..");
}

static FAT16_RESULT initialize_filesystem_layout(void) {
    // Build the shell's virtual root and system/user split on first mount.
    FAT16_RESULT result = ensure_directory("ROOT");

    if (result != FAT16_OK) {
        return result;
    }
    result = fat16_change_directory("ROOT");
    if (result != FAT16_OK) {
        return result;
    }
    result = ensure_directory("SYSTEM");
    if (result != FAT16_OK) {
        return result;
    }
    result = ensure_directory("USR");
    if (result != FAT16_OK) {
        return result;
    }
    result = fat16_change_directory("/");
    if (result != FAT16_OK) {
        return result;
    }
    result = fat16_set_root_directory("ROOT");
    if (result != FAT16_OK) {
        return result;
    }
    result = fat16_protect_directory("SYSTEM");
    if (result != FAT16_OK) {
        return result;
    }
    return fat16_change_directory("USR");
}

static bool mount_data_disk(void) {
    FAT16_IO io;
    FAT16_RESULT result;

    // Bind FAT16 to the ATA block callbacks, then establish shell protection policy.
    if (!ata_pio_initialize()) {
        return false;
    }
    io.context = NULL;
    io.block_count = ata_pio_sector_count();
    io.block_size = 512;
    io.read_blocks = ata_read_callback;
    io.write_blocks = ata_write_callback;
    io.flush_blocks = ata_flush_callback;
    result = fat16_mount(&io);
    if (result == FAT16_OK) {
        result = initialize_filesystem_layout();
        if (result == FAT16_OK) {
            FilesystemMounted = true;
            serial_write_string(
                "[kernel] Mounted persistent FAT16 filesystem at /ROOT\n");
            serial_write_string(
                "[kernel] /ROOT/SYSTEM is read-only; user files belong in /ROOT/USR\n");
        } else {
            serial_log_filesystem_error(result);
            serial_write_string(
                "[kernel] ERROR: failed to initialize ROOT/SYSTEM/USR\n");
        }
    } else {
        serial_log_filesystem_error(result);
    }
    if (!FilesystemMounted) {
        serial_write_string("[kernel] ERROR: no writable FAT16 data disk found\n");
    }
    return FilesystemMounted;
}

static const char *filesystem_result_message(FAT16_RESULT result) {
    switch (result) {
    case FAT16_NOT_FOUND:
        return "Entry not found.\r\n";
    case FAT16_ALREADY_EXISTS:
        return "An entry with that name already exists.\r\n";
    case FAT16_NOT_DIRECTORY:
        return "That entry is not a directory.\r\n";
    case FAT16_NOT_EMPTY:
        return "Directory is not empty; use rmdir -rf <directory>.\r\n";
    case FAT16_IS_DIRECTORY:
        return "That entry is a directory.\r\n";
    case FAT16_NO_SPACE:
        return "No space or directory entries are available.\r\n";
    case FAT16_INVALID_NAME:
        return "Invalid FAT filename (maximum 255 printable ASCII characters).\r\n";
    case FAT16_ROOT_DIRECTORY:
        return "The root directory cannot be removed.\r\n";
    case FAT16_CURRENT_DIRECTORY:
        return "Cannot remove the current directory or one of its parents.\r\n";
    case FAT16_PATH_TOO_DEEP:
        return "Directory nesting limit reached.\r\n";
    case FAT16_IO_ERROR:
        return "Disk I/O failed.\r\n";
    case FAT16_CORRUPT:
        return "The FAT16 filesystem is invalid or damaged.\r\n";
    case FAT16_FILE_TOO_LARGE:
        return "File is larger than the 64 KiB buffer limit.\r\n";
    case FAT16_READ_ONLY:
        return "System files and directories are read-only.\r\n";
    case FAT16_OK:
        return "";
    }
    return "Filesystem operation failed.\r\n";
}

static EFI_STATUS print_prompt(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console) {
    return console_write_ascii(console, ">> ");
}

static size_t split_arguments(char *line, char *arguments[],
                              size_t argument_capacity) {
    size_t count = 0;
    char *read = line;
    char *write = line;

    // Parse in place: the write cursor removes quote characters without allocation.
    while (*read != '\0') {
        char quote = '\0';

        while (*read == ' ' || *read == '\t') {
            ++read;
        }
        if (*read == '\0') {
            break;
        }
        if (count == argument_capacity) {
            return argument_capacity + 1;
        }
        arguments[count++] = write;
        while (*read != '\0') {
            char character = *read++;

            if (quote != '\0') {
                if (character == quote) {
                    quote = '\0';
                } else {
                    *write++ = character;
                }
            } else if (character == '"' || character == '\'') {
                quote = character;
            } else if (character == ' ' || character == '\t') {
                break;
            } else {
                *write++ = character;
            }
        }
        if (quote != '\0') {
            return argument_capacity + 2;
        }
        *write++ = '\0';
    }
    return count;
}

static EFI_STATUS print_filesystem_result(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console, FAT16_RESULT result,
    const char *success_message) {
    EFI_STATUS status;

    if (result == FAT16_OK) {
        status = console_write_ascii(console, success_message);
    } else {
        serial_log_filesystem_error(result);
        status = console_write_ascii(console,
                                     filesystem_result_message(result));
    }
    return status;
}

typedef struct {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *console;
    EFI_STATUS status;
} LIST_OUTPUT_CONTEXT;

static void print_list_entry(void *context, const char *name,
                             bool is_directory, uint32_t size) {
    LIST_OUTPUT_CONTEXT *output =
        static_cast<LIST_OUTPUT_CONTEXT *>(context);

    (void)size;
    if (EFI_ERROR(output->status)) {
        return;
    }
    output->status = console_write_ascii(output->console, name);
    if (!EFI_ERROR(output->status) && is_directory) {
        output->status = console_write_ascii(output->console, "/");
    }
    if (!EFI_ERROR(output->status)) {
        output->status = console_write_ascii(output->console, "\r\n");
    }
}

static EFI_STATUS console_write_file_text(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output, const char *buffer,
    uint32_t size) {
    // Normalize line endings for the console and replace non-printable bytes.
    for (uint32_t index = 0; index < size; ++index) {
        unsigned char character = (unsigned char)buffer[index];
        EFI_STATUS status;

        if (character == '\r') {
            if (index + 1 < size && buffer[index + 1] == '\n') {
                ++index;
            }
            status = console_write_ascii(output, "\r\n");
        } else if (character == '\n') {
            status = console_write_ascii(output, "\r\n");
        } else if (character == '\t' ||
                   (character >= 0x20 && character <= 0x7E)) {
            status = console_write_character(output, character);
        } else {
            status = console_write_character(output, '?');
        }
        if (EFI_ERROR(status)) {
            serial_log_error("write file contents", status);
            return status;
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS run_command(char *line,
                              EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                              EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                              KERNEL_STALL stall,
                              KERNEL_POWER_OFF power_off) {
    char *arguments[SHELL_ARGUMENT_COUNT];
    size_t argument_count;
    EFI_STATUS status;
    FAT16_RESULT result;
    const char *success_message;

    if (line[0] != '\0') {
        serial_write_string("[kernel] command: ");
        serial_write_string(line);
        serial_write_string("\n");
    }
    // Tokenize before dispatch; all command handlers share the same bounded buffers.
    argument_count = split_arguments(
        line, arguments, sizeof(arguments) / sizeof(arguments[0]));
    if (argument_count == 0) {
        return print_prompt(output);
    }
    if (argument_count > SHELL_ARGUMENT_COUNT) {
        status = console_write_ascii(
            output, argument_count == SHELL_ARGUMENT_COUNT + 2
                        ? "Unclosed quote.\r\n"
                        : "Too many arguments.\r\n");
        return EFI_ERROR(status) ? status : print_prompt(output);
    }

    if (strings_equal(arguments[0], "quit") && argument_count == 1) {
        serial_write_string("[kernel] requesting native shutdown\n");
        status = console_write_ascii(output, "Shutting down...\r\n");
        if (EFI_ERROR(status)) {
            serial_log_error("write shutdown message", status);
        }
        power_off();
    }
    if (strings_equal(arguments[0], "quit")) {
        status = console_write_ascii(output, "Usage: quit\r\n");
        return EFI_ERROR(status) ? status : print_prompt(output);
    }
    if (strings_equal(arguments[0], "help") && argument_count == 1) {
        status = console_write_ascii(
            output,
            "cd <path>  ls  pwd  mkdir <path>  rmdir [-rf] <path>\r\n"
            "mkfile <path>  cat <file>  open <file>  rm <file>\r\n"
            "cp <source> <destination>  mv <source> <destination>  ip  httpget\r\n"
            "httpget <IPv4[:port]> <path> [save-as] downloads plain HTTP.\r\n"
            "Filesystem root: /ROOT. SYSTEM is read-only; save user files in USR.\r\n");
        return EFI_ERROR(status) ? status : print_prompt(output);
    }
    if (strings_equal(arguments[0], "ip") && argument_count == 1) {
        status = print_network_configuration(output);
        return EFI_ERROR(status) ? status : print_prompt(output);
    }
    // Networking commands are independent of FAT availability unless saving a file.
    if (strings_equal(arguments[0], "httpget")) {
        uint32_t remote_ip;
        uint16_t port;
        size_t response_length = 0;
        uint16_t response_status = 0;
        int32_t http_result;

        if (argument_count != 3 && argument_count != 4) {
            status = console_write_ascii(
                output, "Usage: httpget <IPv4[:port]> <path> [save-as]\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (!parse_http_endpoint(arguments[1], &remote_ip, &port) ||
            arguments[2][0] != '/') {
            status = console_write_ascii(
                output, "Use a numeric IPv4 address and an absolute HTTP path.\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (!e1000_has_ipv4_address()) {
            status = console_write_ascii(
                output, "Network is not configured; check DHCP with ip.\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        status = console_write_ascii(output, "Connecting over plain HTTP...\r\n");
        if (EFI_ERROR(status)) {
            return status;
        }
        http_result = daneels_ritron_http_get(
            remote_ip, port, (const uint8_t *)arguments[2],
            string_length(arguments[2]), (uint8_t *)EditorBuffer,
            sizeof(EditorBuffer), &response_length, &response_status, stall);
        if (http_result != 0) {
            status = console_write_ascii(output, http_client_error(http_result));
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        status = console_write_ascii(output, "HTTP status: ");
        if (!EFI_ERROR(status)) {
            status = console_write_decimal(output, response_status);
        }
        if (!EFI_ERROR(status)) {
            status = console_write_ascii(output, "\r\n");
        }
        if (EFI_ERROR(status)) {
            return status;
        }
        if (argument_count == 4) {
            if (response_status < 200 || response_status >= 300) {
                status = console_write_ascii(
                    output, "Response was not successful; file not saved.\r\n");
                return EFI_ERROR(status) ? status : print_prompt(output);
            }
            // Filesystem-dependent operations stop here rather than reporting false success.
            if (!FilesystemMounted) {
                status = console_write_ascii(
                    output, "No FAT16 data disk is mounted; response not saved.\r\n");
                return EFI_ERROR(status) ? status : print_prompt(output);
            }
            FAT16_RESULT save_result = fat16_write_file(
                arguments[3], EditorBuffer, (uint32_t)response_length);
            if (save_result != FAT16_OK) {
                status = print_filesystem_result(output, save_result, "");
                return EFI_ERROR(status) ? status : print_prompt(output);
            }
            status = console_write_ascii(output, "Saved ");
            if (!EFI_ERROR(status)) {
                status = console_write_decimal(
                    output, (uint32_t)response_length);
            }
            if (!EFI_ERROR(status)) {
                status = console_write_ascii(output, " bytes to ");
            }
            if (!EFI_ERROR(status)) {
                status = console_write_ascii(output, arguments[3]);
            }
            if (!EFI_ERROR(status)) {
                status = console_write_ascii(output, ".\r\n");
            }
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (!EFI_ERROR(status)) {
            status = console_write_file_text(output, EditorBuffer,
                                              (uint32_t)response_length);
        }
        if (!EFI_ERROR(status) && response_length != 0) {
            status = console_write_ascii(output, "\r\n");
        }
        return EFI_ERROR(status) ? status : print_prompt(output);
    }

    if (!FilesystemMounted) {
        status = console_write_ascii(output, "No FAT16 data disk is mounted.\r\n");
        return EFI_ERROR(status) ? status : print_prompt(output);
    }

    if (strings_equal(arguments[0], "cd")) {
        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: cd <directory-path>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_change_directory(arguments[1]);
        success_message = "";
    } else if (strings_equal(arguments[0], "cat")) {
        uint32_t file_size = 0;

        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: cat <filename>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_read_file(arguments[1], EditorBuffer,
                                 sizeof(EditorBuffer), &file_size);
        if (result != FAT16_OK) {
            return print_filesystem_result(output, result, "");
        }
        status = console_write_file_text(output, EditorBuffer, file_size);
        return EFI_ERROR(status) ? status : print_prompt(output);
    } else if (strings_equal(arguments[0], "cp")) {
        if (argument_count != 3) {
            status = console_write_ascii(
                output, "Usage: cp <source-file> <destination-file>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_copy_file(arguments[1], arguments[2]);
        success_message = "File copied.\r\n";
    } else if (strings_equal(arguments[0], "mv")) {
        if (argument_count != 3) {
            status = console_write_ascii(
                output, "Usage: mv <source-file> <destination-file>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_move_file(arguments[1], arguments[2]);
        success_message = "File moved.\r\n";
    } else if (strings_equal(arguments[0], "rm")) {
        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: rm <filename>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_remove_file(arguments[1]);
        success_message = "File removed.\r\n";
    } else if (strings_equal(arguments[0], "open")) {
        uint32_t file_size = 0;

        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: open <filename>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (fat16_path_is_read_only(arguments[1])) {
            status = console_write_ascii(
                output, "System files are read-only.\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_read_file(arguments[1], EditorBuffer,
                                 sizeof(EditorBuffer), &file_size);
        if (result != FAT16_OK && result != FAT16_NOT_FOUND) {
            return print_filesystem_result(output, result, "");
        }
        if (result == FAT16_NOT_FOUND) {
            file_size = 0;
        } else {
            uint32_t write_index = 0;
            for (uint32_t read_index = 0; read_index < file_size;
                 ++read_index) {
                if (EditorBuffer[read_index] == '\r') {
                    if (read_index + 1 < file_size &&
                        EditorBuffer[read_index + 1] == '\n') {
                        ++read_index;
                    }
                    EditorBuffer[write_index++] = '\n';
                } else {
                    EditorBuffer[write_index++] = EditorBuffer[read_index];
                }
            }
            file_size = write_index;
        }
        status = run_editor(input, output, stall, arguments[1], file_size);
        return EFI_ERROR(status) ? status : print_prompt(output);
    } else if (strings_equal(arguments[0], "mkdir")) {
        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: mkdir <directory>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_make_directory(arguments[1]);
        success_message = "Directory created.\r\n";
    } else if (strings_equal(arguments[0], "rmdir")) {
        bool recursive = false;
        const char *name;

        if (argument_count == 2) {
            name = arguments[1];
        } else if (argument_count == 3 &&
                   strings_equal(arguments[1], "-rf")) {
            recursive = true;
            name = arguments[2];
        } else {
            status = console_write_ascii(
                output, "Usage: rmdir [-rf] <directory>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_remove_directory(name, recursive);
        success_message = "Directory removed.\r\n";
    } else if (strings_equal(arguments[0], "mkfile")) {
        if (argument_count != 2) {
            status = console_write_ascii(output, "Usage: mkfile <filename>\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_make_file(arguments[1]);
        success_message = "Empty file created.\r\n";
    } else if (strings_equal(arguments[0], "ls")) {
        LIST_OUTPUT_CONTEXT list_output = {output, EFI_SUCCESS};

        if (argument_count != 1) {
            status = console_write_ascii(output, "Usage: ls\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        result = fat16_list_directory(print_list_entry, &list_output);
        if (result == FAT16_OK && EFI_ERROR(list_output.status)) {
            return list_output.status;
        }
        success_message = "";
    } else if (strings_equal(arguments[0], "pwd")) {
        if (argument_count != 1) {
            status = console_write_ascii(output, "Usage: pwd\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (!fat16_get_path(ShellPathBuffer, sizeof(ShellPathBuffer))) {
            status = console_write_ascii(output, "Current path is too long.\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        status = console_write_ascii(output, ShellPathBuffer);
        if (!EFI_ERROR(status)) {
            status = console_write_ascii(output, "\r\n");
        }
        return EFI_ERROR(status) ? status : print_prompt(output);
    } else {
        status = console_write_ascii(
            output, "Unknown command. Type help to see available commands.\r\n");
        return EFI_ERROR(status) ? status : print_prompt(output);
    }

    status = print_filesystem_result(output, result, success_message);
    return EFI_ERROR(status) ? status : print_prompt(output);
}

typedef struct {
    char names[16][256];
    bool directories[16];
    size_t count;
    const char *prefix;
    size_t prefix_length;
} SHELL_COMPLETION_CONTEXT;

// Static storage avoids a several-kilobyte temporary on the limited kernel stack.
static SHELL_COMPLETION_CONTEXT ShellCompletions;

static bool shell_prefix_matches(const char *text, const char *prefix,
                                 size_t prefix_length) {
    for (size_t index = 0; index < prefix_length; ++index) {
        char left = text[index];
        char right = prefix[index];

        if (left == '\0') {
            return false;
        }
        if (left >= 'a' && left <= 'z') {
            left = (char)(left - 'a' + 'A');
        }
        if (right >= 'a' && right <= 'z') {
            right = (char)(right - 'a' + 'A');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

static void collect_completion(void *context, const char *name,
                               bool is_directory, uint32_t size) {
    SHELL_COMPLETION_CONTEXT *matches =
        static_cast<SHELL_COMPLETION_CONTEXT *>(context);

    (void)size;
    if (matches->count >= sizeof(matches->names) / sizeof(matches->names[0]) ||
        !shell_prefix_matches(name, matches->prefix,
                              matches->prefix_length)) {
        return;
    }
    size_t index = 0;
    while (name[index] != '\0' && index < sizeof(matches->names[0]) - 1) {
        matches->names[matches->count][index] = name[index];
        ++index;
    }
    matches->names[matches->count][index] = '\0';
    matches->directories[matches->count] = is_directory;
    ++matches->count;
}

static void remember_shell_command(const char *line) {
    if (line[0] == '\0' ||
        (ShellHistoryCount != 0 &&
         strings_equal(ShellHistory[(ShellHistoryNext +
                                     SHELL_HISTORY_CAPACITY - 1) %
                                    SHELL_HISTORY_CAPACITY],
                       line))) {
        return;
    }
    // Suppress only adjacent duplicates; the circular buffer retains recent order.
    size_t index = 0;
    while (line[index] != '\0' && index < SHELL_LINE_CAPACITY - 1) {
        ShellHistory[ShellHistoryNext][index] = line[index];
        ++index;
    }
    ShellHistory[ShellHistoryNext][index] = '\0';
    ShellHistoryNext = (ShellHistoryNext + 1) % SHELL_HISTORY_CAPACITY;
    if (ShellHistoryCount < SHELL_HISTORY_CAPACITY) {
        ++ShellHistoryCount;
    }
}

static EFI_STATUS redraw_shell_line(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                                    const char *line, size_t length,
                                    size_t cursor,
                                    size_t origin_column, size_t origin_row) {
    size_t columns;
    size_t rows;
    size_t line_width;
    size_t visible_capacity;
    size_t line_start = 0;
    size_t visible_length;
    bool clipped_prefix;
    size_t cursor_cell;
    EFI_STATUS status = output->QueryMode(output, output->Mode->Mode,
                                          &columns, &rows);

    if (EFI_ERROR(status) || columns <= origin_column + 4 ||
        origin_row >= rows) {
        return EFI_ERROR(status) ? status : EFI_ERROR_BIT | 2;
    }
    // Reserve prompt cells and show a '<' marker when the cursor hides a prefix.
    line_width = columns - origin_column - 4;
    visible_capacity = line_width;
    if (cursor > visible_capacity) {
        clipped_prefix = true;
        --visible_capacity;
        line_start = cursor - visible_capacity;
    } else {
        clipped_prefix = false;
    }
    visible_length = length > line_start ? length - line_start : 0;
    if (visible_length > visible_capacity) {
        visible_length = visible_capacity;
    }
    status = output->SetCursorPosition(output, origin_column, origin_row);

    if (EFI_ERROR(status)) {
        return status;
    }
    if (clipped_prefix) {
        console_redraw_character('<');
    }
    for (size_t index = 0; index < visible_length; ++index) {
        console_redraw_character(line[line_start + index]);
    }
    for (size_t index = visible_length + (clipped_prefix ? 1 : 0);
         index < line_width; ++index) {
        console_redraw_character(' ');
    }
    cursor_cell = origin_column + 3 + (clipped_prefix ? 1 : 0) +
                  cursor - line_start;
    if (origin_row + cursor_cell / columns >= rows) {
        return EFI_ERROR_BIT | 2;
    }
    status = output->SetCursorPosition(
        output, cursor_cell % columns, origin_row + cursor_cell / columns);
    return status;
}

static EFI_STATUS complete_shell_line(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                                      char *line, size_t *length,
                                      size_t *cursor, size_t *origin_column,
                                      size_t *origin_row,
                                      size_t *line_limit) {
    static const char *const commands[] = {
        "help", "ls", "pwd", "cd", "mkdir", "rmdir", "mkfile", "cat",
        "rm",   "open", "cp", "mv", "ip", "httpget", "quit"};
    size_t token_start = 0;
    bool quoted = false;
    char active_quote = '\0';
    EFI_STATUS status;

    // Completion is defined only at the end of the current input token.
    if (*cursor != *length) {
        return EFI_SUCCESS;
    }
    for (size_t index = 0; index < *length; ++index) {
        char character = line[index];
        if (active_quote != '\0') {
            if (character == active_quote) {
                active_quote = '\0';
            }
        } else if (character == '"' || character == '\'') {
            active_quote = character;
            if (index == token_start) {
                quoted = true;
            }
        } else if (character == ' ' || character == '\t') {
            token_start = index + 1;
            quoted = false;
        }
    }
    if (quoted && active_quote == '\0') {
        return EFI_SUCCESS;
    }
    size_t prefix_start = token_start + (quoted ? 1 : 0);
    const char *prefix = line + prefix_start;
    size_t prefix_length = *length - prefix_start;
    if (prefix_length == 0 ||
        prefix[0] == '/' || prefix[0] == '\\') {
        return EFI_SUCCESS;
    }

    SHELL_COMPLETION_CONTEXT *matches = &ShellCompletions;
    matches->count = 0;
    matches->prefix = prefix;
    matches->prefix_length = prefix_length;
    bool command_position = token_start == 0;
    // Commands complete in token zero; later tokens match names in the cwd only.
    if (command_position) {
        for (size_t index = 0;
             index < sizeof(commands) / sizeof(commands[0]); ++index) {
            collect_completion(matches, commands[index], false, 0);
        }
    } else if (FilesystemMounted) {
        for (size_t index = token_start; index < *length; ++index) {
            if (line[index] == '/' || line[index] == '\\') {
                return EFI_SUCCESS;
            }
        }
        FAT16_RESULT result = fat16_list_directory(collect_completion,
                                                    matches);
        if (result != FAT16_OK) {
            serial_log_filesystem_error(result);
            return console_write_ascii(output,
                                       filesystem_result_message(result));
        }
    } else {
        return EFI_SUCCESS;
    }

    if (matches->count == 0) {
        return EFI_SUCCESS;
    }
    // Multiple candidates are listed; a single candidate replaces the typed prefix.
    if (matches->count > 1) {
        status = console_write_ascii(output, "\r\n");
        for (size_t index = 0; index < matches->count && !EFI_ERROR(status);
             ++index) {
            status = console_write_ascii(output, matches->names[index]);
            if (!EFI_ERROR(status) && matches->directories[index]) {
                status = console_write_ascii(output, "/");
            }
            if (!EFI_ERROR(status)) {
                status = console_write_ascii(output, "  ");
            }
        }
        if (EFI_ERROR(status)) {
            return status;
        }
        status = console_write_ascii(output, "\r\n");
        if (!EFI_ERROR(status)) {
            status = print_prompt(output);
        }
        if (EFI_ERROR(status)) {
            return status;
        }
        console_get_cursor_position(origin_column, origin_row);
        return redraw_shell_line(output, line, *length, *cursor,
                                 *origin_column, *origin_row);
    }

    char replacement[260];
    size_t replacement_length = 0;
    const char *name = matches->names[0];
    bool quote_name = quoted;
    for (size_t index = 0; name[index] != '\0'; ++index) {
        if (name[index] == ' ') {
            quote_name = true;
        }
    }
    if (quote_name) {
        replacement[replacement_length++] = '"';
    }
    for (size_t index = 0; name[index] != '\0'; ++index) {
        replacement[replacement_length++] = name[index];
    }
    if (matches->directories[0]) {
        replacement[replacement_length++] = '/';
    }
    if (quote_name) {
        replacement[replacement_length++] = '"';
    }
    if (command_position) {
        replacement[replacement_length++] = ' ';
    }
    replacement[replacement_length] = '\0';
    size_t new_length = token_start + replacement_length;
    if (new_length >= SHELL_LINE_CAPACITY || new_length > *line_limit) {
        return EFI_SUCCESS;
    }
    for (size_t index = 0; index < replacement_length; ++index) {
        line[token_start + index] = replacement[index];
    }
    *length = new_length;
    *cursor = new_length;
    line[new_length] = '\0';
    return redraw_shell_line(output, line, *length, *cursor,
                             *origin_column, *origin_row);
}

static EFI_STATUS run_shell(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                            EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                            KERNEL_STALL stall,
                            KERNEL_POWER_OFF power_off) {
    char line[SHELL_LINE_CAPACITY];
    size_t length = 0;
    size_t cursor = 0;
    size_t origin_column;
    size_t origin_row;
    size_t line_limit = sizeof(line) - 1;
    size_t history_browse = SHELL_HISTORY_CAPACITY;
    char history_scratch[SHELL_LINE_CAPACITY];
    EFI_STATUS status;
    EFI_INPUT_KEY key;

    // Establish a fresh prompt and remember its screen origin for line redraws.
    status = output->ClearScreen(output);
    if (EFI_ERROR(status)) {
        serial_log_error("ClearScreen", status);
        return status;
    }
    serial_write_string("[kernel] Shell ready\n");
    status = console_write_ascii(output, FilesystemMounted
                                            ? "Filesystem ready at /ROOT/USR. "
                                              "Type help for commands.\r\n"
                                            : "FAT16 data disk unavailable.\r\n");
    if (EFI_ERROR(status)) {
        serial_log_error("print startup message", status);
        return status;
    }
    status = console_write_ascii(
        output, e1000_has_ipv4_address()
                    ? "Network ready; use ip to view the DHCP configuration.\r\n"
                    : "Network unavailable; use ip to check adapter status.\r\n");
    if (EFI_ERROR(status)) {
        serial_log_error("print network startup status", status);
        return status;
    }
    status = print_prompt(output);
    if (EFI_ERROR(status)) {
        serial_log_error("print prompt", status);
        return status;
    }
    console_get_cursor_position(&origin_column, &origin_row);
    line[0] = '\0';

    for (;;) {
        status = input->ReadKeyStroke(input, &key);
        if (status == EFI_NOT_READY) {
            stall(10000);
            continue;
        }
        if (EFI_ERROR(status)) {
            serial_log_error("ReadKeyStroke", status);
            return status;
        }

        // Enter executes; other keys update the editable line and redraw it in place.
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            line[length] = '\0';
            remember_shell_command(line);
            status = redraw_shell_line(output, line, length, length,
                                       origin_column, origin_row);
            if (EFI_ERROR(status)) {
                serial_log_error("move command cursor before execution",
                                 status);
                return status;
            }
            status = console_write_ascii(output, "\r\n");
            if (EFI_ERROR(status)) {
                serial_log_error("write newline", status);
                return status;
            }
            status = run_command(line, input, output, stall, power_off);
            if (EFI_ERROR(status)) {
                return status;
            }
            length = 0;
            cursor = 0;
            history_browse = SHELL_HISTORY_CAPACITY;
            line[0] = '\0';
            console_get_cursor_position(&origin_column, &origin_row);
            continue;
        }

        if (key.ScanCode == EFI_SCAN_UP || key.ScanCode == EFI_SCAN_DOWN) {
            if (ShellHistoryCount == 0) {
                continue;
            }
            if (key.ScanCode == EFI_SCAN_UP) {
                if (history_browse == SHELL_HISTORY_CAPACITY) {
                    for (size_t index = 0; index <= length; ++index) {
                        history_scratch[index] = line[index];
                    }
                    history_browse = ShellHistoryCount - 1;
                } else if (history_browse > 0) {
                    --history_browse;
                }
            } else if (history_browse != SHELL_HISTORY_CAPACITY) {
                if (history_browse + 1 >= ShellHistoryCount) {
                    history_browse = SHELL_HISTORY_CAPACITY;
                } else {
                    ++history_browse;
                }
            } else {
                continue;
            }
            const char *selected = history_browse == SHELL_HISTORY_CAPACITY
                                       ? history_scratch
                                       : ShellHistory[(ShellHistoryNext +
                                                       SHELL_HISTORY_CAPACITY -
                                                       ShellHistoryCount +
                                                       history_browse) %
                                                      SHELL_HISTORY_CAPACITY];
            length = 0;
            while (selected[length] != '\0' && length < line_limit) {
                line[length] = selected[length];
                ++length;
            }
            line[length] = '\0';
            cursor = length;
            status = redraw_shell_line(output, line, length, cursor,
                                       origin_column, origin_row);
            if (EFI_ERROR(status)) {
                serial_log_error("redraw command history", status);
                return status;
            }
            continue;
        }

        if (key.ScanCode == EFI_SCAN_LEFT || key.ScanCode == EFI_SCAN_RIGHT ||
            key.ScanCode == EFI_SCAN_HOME || key.ScanCode == EFI_SCAN_END) {
            if (key.ScanCode == EFI_SCAN_LEFT && cursor > 0) {
                --cursor;
            } else if (key.ScanCode == EFI_SCAN_RIGHT && cursor < length) {
                ++cursor;
            } else if (key.ScanCode == EFI_SCAN_HOME) {
                cursor = 0;
            } else if (key.ScanCode == EFI_SCAN_END) {
                cursor = length;
            }
            status = redraw_shell_line(output, line, length, cursor,
                                       origin_column, origin_row);
            if (EFI_ERROR(status)) {
                serial_log_error("move command cursor", status);
                return status;
            }
            continue;
        }

        if (key.ScanCode == EFI_SCAN_DELETE) {
            if (cursor < length) {
                for (size_t index = cursor; index < length; ++index) {
                    line[index] = line[index + 1];
                }
                --length;
                status = redraw_shell_line(output, line, length, cursor,
                                           origin_column, origin_row);
                if (EFI_ERROR(status)) {
                    serial_log_error("delete command character", status);
                    return status;
                }
            }
            continue;
        }

        if (key.UnicodeChar == '\t') {
            status = complete_shell_line(output, line, &length, &cursor,
                                         &origin_column, &origin_row,
                                         &line_limit);
            if (EFI_ERROR(status)) {
                serial_log_error("complete shell input", status);
                return status;
            }
            continue;
        }

        if (key.UnicodeChar == 3) {
            length = 0;
            cursor = 0;
            line[0] = '\0';
            history_browse = SHELL_HISTORY_CAPACITY;
            status = redraw_shell_line(output, line, length, cursor,
                                       origin_column, origin_row);
            if (EFI_ERROR(status)) {
                serial_log_error("clear shell input", status);
                return status;
            }
            continue;
        }

        if (key.UnicodeChar == '\b') {
            if (cursor > 0) {
                for (size_t index = cursor - 1; index < length; ++index) {
                    line[index] = line[index + 1];
                }
                --cursor;
                --length;
                status = redraw_shell_line(output, line, length, cursor,
                                           origin_column, origin_row);
                if (EFI_ERROR(status)) {
                    serial_log_error("erase input character", status);
                    return status;
                }
            }
            continue;
        }

        if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            if (length >= line_limit) {
                serial_write_string("[kernel] input line full; character ignored\n");
                continue;
            }
            for (size_t index = length; index > cursor; --index) {
                line[index] = line[index - 1];
            }
            line[cursor++] = (char)key.UnicodeChar;
            ++length;
            line[length] = '\0';
            history_browse = SHELL_HISTORY_CAPACITY;
            status = redraw_shell_line(output, line, length, cursor,
                                       origin_column, origin_row);
            if (EFI_ERROR(status)) {
                serial_log_error("echo input character", status);
                return status;
            }
        }
    }
}

extern "C" EFI_STATUS EFIAPI kernel_main(const BOOT_INFO *boot_info) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output;
    void *heap_test;

    // Bring up diagnostics first so every later initialization failure is visible.
    serial_initialize();
    serial_write_string("[kernel] Native kernel started\n");
    if (boot_info != NULL) {
        serial_log_hex("[kernel] memory map size=", boot_info->memory_map_size);
        serial_log_hex("[kernel] memory descriptor size=",
                       boot_info->memory_descriptor_size);
        serial_log_hex("[kernel] memory descriptor version=",
                       boot_info->memory_descriptor_version);
        serial_log_hex("[kernel] memory map pointer=", boot_info->memory_map);
    }
    if (!memory_initialize(boot_info)) {
        serial_write_string("[kernel] ERROR: memory map initialization failed\n");
        return EFI_ERROR_BIT | 2;
    }
    heap_test = kernel_allocate(64);
    if (heap_test == NULL) {
        serial_write_string("[kernel] ERROR: kernel heap initialization failed\n");
        return EFI_ERROR_BIT | 2;
    }
    kernel_release(heap_test);
    serial_write_string("[kernel] Physical page allocator and kernel heap ready\n");
    if (!exceptions_initialize()) {
        serial_write_string("[kernel] ERROR: exception handler setup failed\n");
        return EFI_ERROR_BIT | 2;
    }
    if (!console_initialize(boot_info)) {
        serial_write_string("[kernel] ERROR: invalid framebuffer handoff\n");
        return EFI_ERROR_BIT | 2;
    }
    output = console_output();
    if (!keyboard_initialize()) {
        serial_write_string("[kernel] ERROR: PS/2 keyboard initialization failed\n");
        return EFI_ERROR_BIT | 2;
    }
    if (!interrupts_initialize()) {
        serial_write_string("[kernel] ERROR: interrupt setup failed\n");
        return EFI_ERROR_BIT | 2;
    }
    interrupt_enable_irq(1);
    kernel_power_initialize(boot_info->acpi_root_table);
    (void)mount_data_disk();
    E1000_RESULT network_result = e1000_initialize();
    if (network_result == E1000_OK) {
        serial_write_string("[network] Intel e1000 interface initialized\n");
        network_result = e1000_dhcp_acquire(kernel_stall);
        if (network_result == E1000_OK) {
            serial_write_string("[network] DHCP address acquired\n");
        } else {
            serial_write_string("[network] DHCP address acquisition failed\n");
        }
    } else if (network_result == E1000_NOT_FOUND) {
        serial_write_string("[network] supported Intel e1000 adapter not found\n");
    } else {
        serial_write_string("[network] Intel e1000 initialization failed\n");
    }
    return run_shell(keyboard_input(), output, kernel_stall, kernel_power_off);
}
