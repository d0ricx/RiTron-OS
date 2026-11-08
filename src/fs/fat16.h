#ifndef FAT16_H
#define FAT16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*FAT16_READ_BLOCKS)(void *context, uint32_t lba,
                                  uint32_t block_count, void *buffer);
typedef bool (*FAT16_WRITE_BLOCKS)(void *context, uint32_t lba,
                                   uint32_t block_count, const void *buffer);
typedef bool (*FAT16_FLUSH_BLOCKS)(void *context);

/* Caller-provided block I/O; callbacks transfer whole sectors and report errors. */
typedef struct {
    void *context;
    uint64_t block_count;
    uint32_t block_size;
    FAT16_READ_BLOCKS read_blocks;
    FAT16_WRITE_BLOCKS write_blocks;
    FAT16_FLUSH_BLOCKS flush_blocks;
} FAT16_IO;

typedef enum {
    FAT16_OK,
    FAT16_NOT_FOUND,
    FAT16_ALREADY_EXISTS,
    FAT16_NOT_DIRECTORY,
    FAT16_NOT_EMPTY,
    FAT16_NO_SPACE,
    FAT16_INVALID_NAME,
    FAT16_ROOT_DIRECTORY,
    FAT16_CURRENT_DIRECTORY,
    FAT16_PATH_TOO_DEEP,
    FAT16_IO_ERROR,
    FAT16_CORRUPT,
    FAT16_FILE_TOO_LARGE,
    FAT16_READ_ONLY,
    FAT16_IS_DIRECTORY
} FAT16_RESULT;

/* Listing callbacks receive a transient name and its basic directory metadata. */
typedef void (*FAT16_LIST_ENTRY)(void *context, const char *name,
                                 bool is_directory, uint32_t size);

#ifdef __cplusplus
extern "C" {
#endif

/* Mount and validate the FAT16 volume supplied through the block callbacks. */
FAT16_RESULT fat16_mount(const FAT16_IO *io);
/* Resolve paths against the current directory; absolute paths begin at root. */
FAT16_RESULT fat16_change_directory(const char *name);
/* Hide the underlying volume root behind a named virtual shell root. */
FAT16_RESULT fat16_set_root_directory(const char *name);
/* Mark an immediate child directory read-only to shell filesystem operations. */
FAT16_RESULT fat16_protect_directory(const char *name);
/* Directory creation/removal respects the protected subtree; recursive removal is opt-in. */
FAT16_RESULT fat16_make_directory(const char *name);
FAT16_RESULT fat16_remove_directory(const char *name, bool recursive);
FAT16_RESULT fat16_make_file(const char *name);
FAT16_RESULT fat16_remove_file(const char *name);
/* Read a file into caller storage; return FILE_TOO_LARGE if capacity is short. */
FAT16_RESULT fat16_read_file(const char *name, char *buffer,
                             uint32_t buffer_capacity, uint32_t *size);
/* Replace by staging and flushing new contents before publishing the entry. */
FAT16_RESULT fat16_write_file(const char *name, const char *buffer,
                              uint32_t size);
/*
 * Regular files only; directories return FAT16_IS_DIRECTORY. Replacement stages
 * and flushes new data before switching the directory entry.
 */
FAT16_RESULT fat16_copy_file(const char *source, const char *destination);
/* Creates and flushes the destination entry before deleting the source name. */
FAT16_RESULT fat16_move_file(const char *source, const char *destination);
/* Callback-based listing; names are valid only for the duration of the callback. */
FAT16_RESULT fat16_list_directory(FAT16_LIST_ENTRY callback, void *context);
/* Writes the current virtual path into a caller buffer; false means too small. */
bool fat16_get_path(char *buffer, size_t buffer_size);
bool fat16_current_directory_is_read_only(void);
/* Resolve a path's parent and test whether it lies in the protected subtree. */
bool fat16_path_is_read_only(const char *path);

#ifdef __cplusplus
}
#endif

#endif
