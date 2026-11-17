#ifndef FAT32_H
#define FAT32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*FAT32_READ_BLOCKS)(void *context, uint32_t lba,
                                  uint32_t block_count, void *buffer);
typedef bool (*FAT32_WRITE_BLOCKS)(void *context, uint32_t lba,
                                   uint32_t block_count, const void *buffer);
typedef bool (*FAT32_FLUSH_BLOCKS)(void *context);

/* Caller-provided block I/O; callbacks transfer whole sectors and report errors. */
typedef struct {
    void *context;
    uint64_t block_count;
    uint32_t block_size;
    FAT32_READ_BLOCKS read_blocks;
    FAT32_WRITE_BLOCKS write_blocks;
    FAT32_FLUSH_BLOCKS flush_blocks;
} FAT32_IO;

typedef enum {
    FAT32_OK,
    FAT32_NOT_FOUND,
    FAT32_ALREADY_EXISTS,
    FAT32_NOT_DIRECTORY,
    FAT32_NOT_EMPTY,
    FAT32_NO_SPACE,
    FAT32_INVALID_NAME,
    FAT32_ROOT_DIRECTORY,
    FAT32_CURRENT_DIRECTORY,
    FAT32_PATH_TOO_DEEP,
    FAT32_IO_ERROR,
    FAT32_CORRUPT,
    FAT32_FILE_TOO_LARGE,
    FAT32_READ_ONLY,
    FAT32_IS_DIRECTORY
} FAT32_RESULT;

/* Listing callbacks receive a transient name and its basic directory metadata. */
typedef void (*FAT32_LIST_ENTRY)(void *context, const char *name,
                                 bool is_directory, uint32_t size);

#ifdef __cplusplus
extern "C" {
#endif

/* Mount and validate a FAT32 volume through block callbacks. */
FAT32_RESULT fat32_mount(const FAT32_IO *io);
/* Resolve paths against the current directory; absolute paths begin at root. */
FAT32_RESULT fat32_change_directory(const char *name);
/* Hide the underlying volume root behind a named virtual shell root. */
FAT32_RESULT fat32_set_root_directory(const char *name);
/* Mark an immediate child directory read-only to shell filesystem operations. */
FAT32_RESULT fat32_protect_directory(const char *name);
/* Directory creation/removal respects the protected subtree; recursive removal is opt-in. */
FAT32_RESULT fat32_make_directory(const char *name);
FAT32_RESULT fat32_remove_directory(const char *name, bool recursive);
FAT32_RESULT fat32_make_file(const char *name);
FAT32_RESULT fat32_remove_file(const char *name);
/* Read a file into caller storage; return FILE_TOO_LARGE if capacity is short. */
FAT32_RESULT fat32_read_file(const char *name, char *buffer,
                             uint32_t buffer_capacity, uint32_t *size);
/* Replace by staging and flushing new contents before publishing the entry. */
FAT32_RESULT fat32_write_file(const char *name, const char *buffer,
                              uint32_t size);
/*
 * Regular files only; directories return FAT32_IS_DIRECTORY. Replacement stages
 * and flushes new data before switching the directory entry.
 */
FAT32_RESULT fat32_copy_file(const char *source, const char *destination);
/* Creates and flushes the destination entry before deleting the source name. */
FAT32_RESULT fat32_move_file(const char *source, const char *destination);
/* Callback-based listing; names are valid only for the duration of the callback. */
FAT32_RESULT fat32_list_directory(FAT32_LIST_ENTRY callback, void *context);
/* Writes the current virtual path into a caller buffer; false means too small. */
bool fat32_get_path(char *buffer, size_t buffer_size);
bool fat32_current_directory_is_read_only(void);
/* Resolve a path's parent and test whether it lies in the protected subtree. */
bool fat32_path_is_read_only(const char *path);

#ifdef __cplusplus
}
#endif

#endif
