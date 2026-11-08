#include "ata.h"
#include "cpu.h"
#include "fat16.h"
#include "keyboard.h"
#include "memory.h"
#include "power.h"

#include <stddef.h>
#include <stdint.h>

#define SHELL_LINE_CAPACITY 80
#define SHELL_ARGUMENT_COUNT 3
#define EDITOR_BUFFER_CAPACITY 65536
#define EDITOR_TAB_WIDTH 4

typedef void (*KERNEL_STALL)(size_t);
typedef void (*KERNEL_POWER_OFF)(void);

static bool FilesystemMounted;
static char EditorBuffer[EDITOR_BUFFER_CAPACITY];

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

static int strings_equal(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static void advance_editor_position(char character, size_t columns,
                                    size_t *row, size_t *column) {
    if (character == '\n') {
        ++*row;
        *column = 0;
        return;
    }

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
    visible_rows = rows - 2;
    editor_position_at(EditorBuffer, cursor, columns, &cursor_row,
                       &cursor_column);
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
    *cursor = target + (current_column < target_end - target
                            ? current_column
                            : target_end - target);
}

static EFI_STATUS editor_wait_for_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                                      KERNEL_STALL stall, EFI_INPUT_KEY *key) {
    EFI_STATUS status;
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
        return "Invalid FAT 8.3 name (maximum 8 characters plus 3-character extension).\r\n";
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
    char *position = line;

    while (*position != '\0') {
        while (*position == ' ' || *position == '\t') {
            ++position;
        }
        if (*position == '\0') {
            break;
        }
        if (count == argument_capacity) {
            return argument_capacity + 1;
        }
        arguments[count++] = position;
        while (*position != '\0' && *position != ' ' && *position != '\t') {
            ++position;
        }
        if (*position != '\0') {
            *position++ = '\0';
        }
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
    argument_count = split_arguments(
        line, arguments, sizeof(arguments) / sizeof(arguments[0]));
    if (argument_count == 0) {
        return print_prompt(output);
    }
    if (argument_count > SHELL_ARGUMENT_COUNT) {
        status = console_write_ascii(output, "Too many arguments.\r\n");
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
            "mkfile <path>  cat <file>  rm <file>  open <file>  quit\r\n"
            "Filesystem root: /ROOT. SYSTEM is read-only; save user files in USR.\r\n");
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
        char path[17 * 13 + 2];

        if (argument_count != 1) {
            status = console_write_ascii(output, "Usage: pwd\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        if (!fat16_get_path(path, sizeof(path))) {
            status = console_write_ascii(output, "Current path is too long.\r\n");
            return EFI_ERROR(status) ? status : print_prompt(output);
        }
        status = console_write_ascii(output, path);
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

static EFI_STATUS run_shell(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *input,
                            EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *output,
                            KERNEL_STALL stall,
                            KERNEL_POWER_OFF power_off) {
    char line[SHELL_LINE_CAPACITY];
    size_t length = 0;
    EFI_STATUS status;
    EFI_INPUT_KEY key;

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
    status = print_prompt(output);
    if (EFI_ERROR(status)) {
        serial_log_error("print prompt", status);
        return status;
    }

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

        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            line[length] = '\0';
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
            continue;
        }

        if (key.UnicodeChar == '\b') {
            if (length > 0) {
                --length;
                status = console_write_ascii(output, "\b \b");
                if (EFI_ERROR(status)) {
                    serial_log_error("erase input character", status);
                    return status;
                }
            }
            continue;
        }

        if (key.UnicodeChar >= 0x20 && key.UnicodeChar <= 0x7E) {
            if (length == sizeof(line) - 1) {
                serial_write_string("[kernel] input line full; character ignored\n");
                continue;
            }
            line[length++] = (char)key.UnicodeChar;
            status = console_write_character(output, key.UnicodeChar);
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
    return run_shell(keyboard_input(), output, kernel_stall, kernel_power_off);
}
