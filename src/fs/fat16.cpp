#include "fat16.h"

// This module mounts one FAT16 volume at a time. Directory and FAT sectors are
// read-modify-written through callbacks so storage is replaceable in tests.
#define FAT16_SECTOR_SIZE 512
#define FAT16_ROOT_CLUSTER 0
#define FAT16_ATTRIBUTE_DIRECTORY 0x10
#define FAT16_ATTRIBUTE_VOLUME 0x08
#define FAT16_ATTRIBUTE_LONG_NAME 0x0F
#define FAT16_END_OF_CHAIN_MIN 0xFFF8
#define FAT16_END_OF_CHAIN 0xFFFF
#define FAT16_DATA_DISK_VOLUME_ID UINT32_C(0x44525446)
#define FAT16_DATA_DISK_LABEL "KERNELDATA "
#define FAT16_MAX_DIRECTORY_DEPTH 16
#define FAT16_MAX_DELETE_DEPTH 32
#define FAT16_MAX_LFN_ENTRIES 21
#define FAT16_MAX_LFN_LENGTH 255
#define FAT16_MAX_NAME_LENGTH (FAT16_MAX_LFN_LENGTH + 1)

typedef struct {
    /* Root is the fixed FAT16 root; non-root directories follow a cluster chain. */
    uint32_t cluster;
    bool root;
} FAT16_DIRECTORY;

typedef struct {
    /* An entry may be addressed later without retaining its containing sector. */
    uint32_t lba;
    uint8_t entry_index;
} FAT16_ENTRY_LOCATION;

typedef struct {
    /* Cursor state lets directory scans remain bounded in stack storage. */
    FAT16_DIRECTORY directory;
    uint32_t cluster;
    uint32_t sector_in_cluster;
    uint32_t lba;
    uint32_t root_sector_index;
    uint32_t visited_clusters;
    uint8_t entry_index;
    bool finished;
    uint8_t sector[FAT16_SECTOR_SIZE];
} FAT16_DIRECTORY_CURSOR;

static FAT16_IO filesystem_io;
static uint32_t filesystem_total_sectors;
static uint32_t filesystem_root_start;
static uint32_t filesystem_root_sectors;
static uint32_t filesystem_data_start;
static uint32_t filesystem_fat_start;
static uint32_t filesystem_fat_sectors;
static uint32_t filesystem_cluster_count;
static uint8_t filesystem_fat_count;
static uint8_t filesystem_sectors_per_cluster;
static uint32_t current_directory_stack[FAT16_MAX_DIRECTORY_DEPTH + 1];
static char current_directory_names[FAT16_MAX_DIRECTORY_DEPTH]
                                   [FAT16_MAX_NAME_LENGTH];
static uint32_t current_directory_depth;
static bool filesystem_virtual_root_active;
static uint32_t filesystem_virtual_root_cluster;
static char filesystem_virtual_root_name[FAT16_MAX_NAME_LENGTH];
static uint32_t filesystem_read_only_cluster;
static uint32_t filesystem_read_only_parent_cluster;
// Sector staging is aligned for the ATA driver's DMA-safe/sector-aligned buffer path.
static uint8_t io_sector[FAT16_SECTOR_SIZE] __attribute__((aligned(4096)));

static void bytes_copy(void *destination, const void *source, size_t count) {
    uint8_t *to = static_cast<uint8_t *>(destination);
    const uint8_t *from = static_cast<const uint8_t *>(source);

    while (count-- != 0) {
        *to++ = *from++;
    }
}

static void bytes_set(void *destination, uint8_t value, size_t count) {
    uint8_t *to = static_cast<uint8_t *>(destination);

    while (count-- != 0) {
        *to++ = value;
    }
}

static bool bytes_equal(const void *left, const void *right, size_t count) {
    const uint8_t *left_bytes = static_cast<const uint8_t *>(left);
    const uint8_t *right_bytes = static_cast<const uint8_t *>(right);

    while (count-- != 0) {
        if (*left_bytes++ != *right_bytes++) {
            return false;
        }
    }
    return true;
}

static size_t string_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    // FAT data clusters are numbered from 2; cluster 2 starts at data_start.
    return filesystem_data_start +
           (cluster - 2) * filesystem_sectors_per_cluster;
}

static bool read_sector(uint32_t lba, uint8_t *buffer) {
    // Route all sectors through aligned storage before copying to arbitrary callers.
    if (lba >= filesystem_total_sectors ||
        !filesystem_io.read_blocks(filesystem_io.context, lba, 1, io_sector)) {
        return false;
    }
    bytes_copy(buffer, io_sector, sizeof(io_sector));
    return true;
}

static bool write_sector(uint32_t lba, const uint8_t *buffer) {
    if (lba >= filesystem_total_sectors) {
        return false;
    }
    // Keep driver alignment requirements independent of caller buffer alignment.
    bytes_copy(io_sector, buffer, sizeof(io_sector));
    return filesystem_io.write_blocks(filesystem_io.context, lba, 1,
                                      io_sector);
}

static FAT16_RESULT flush_device(void) {
    return filesystem_io.flush_blocks(filesystem_io.context) ? FAT16_OK
                                                              : FAT16_IO_ERROR;
}

static bool valid_cluster(uint32_t cluster) {
    return cluster >= 2 && cluster < filesystem_cluster_count + 2;
}

static FAT16_RESULT change_directory_component(const char *name);

static bool current_directory_is_volume_root(void) {
    return current_directory_depth == 0 && !filesystem_virtual_root_active;
}

static bool current_directory_is_read_only(void) {
    if (filesystem_read_only_cluster == 0) {
        return false;
    }
    for (uint32_t index = 0; index <= current_directory_depth; ++index) {
        if (current_directory_stack[index] == filesystem_read_only_cluster) {
            return true;
        }
    }
    return false;
}

static FAT16_RESULT get_fat_entry(uint32_t cluster, uint16_t *value) {
    uint32_t byte_offset;
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (!valid_cluster(cluster)) {
        return FAT16_CORRUPT;
    }
    // FAT16 stores one little-endian 16-bit link per cluster number.
    byte_offset = cluster * 2;
    if (byte_offset / FAT16_SECTOR_SIZE >= filesystem_fat_sectors ||
        !read_sector(filesystem_fat_start + byte_offset / FAT16_SECTOR_SIZE,
                     sector)) {
        return FAT16_IO_ERROR;
    }
    *value = read_u16(sector + byte_offset % FAT16_SECTOR_SIZE);
    return FAT16_OK;
}

static FAT16_RESULT set_fat_entry(uint32_t cluster, uint16_t value) {
    uint32_t byte_offset;
    uint32_t fat_index;
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (!valid_cluster(cluster)) {
        return FAT16_CORRUPT;
    }
    byte_offset = cluster * 2;
    if (byte_offset / FAT16_SECTOR_SIZE >= filesystem_fat_sectors) {
        return FAT16_CORRUPT;
    }
    // Keep every advertised FAT copy in sync before returning success.
    for (fat_index = 0; fat_index < filesystem_fat_count; ++fat_index) {
        uint32_t lba = filesystem_fat_start +
                       fat_index * filesystem_fat_sectors +
                       byte_offset / FAT16_SECTOR_SIZE;
        if (!read_sector(lba, sector)) {
            return FAT16_IO_ERROR;
        }
        write_u16(sector + byte_offset % FAT16_SECTOR_SIZE, value);
        if (!write_sector(lba, sector)) {
            return FAT16_IO_ERROR;
        }
    }
    return FAT16_OK;
}

static FAT16_RESULT allocate_cluster(uint32_t *cluster_out) {
    uint32_t fat_sector_index;
    uint8_t sector[FAT16_SECTOR_SIZE];

    // Scan for a free data cluster, reserve it, and zero it before exposing it.
    for (fat_sector_index = 0; fat_sector_index < filesystem_fat_sectors;
         ++fat_sector_index) {
        uint32_t entry_index;
        uint32_t first_cluster = fat_sector_index *
                                 (FAT16_SECTOR_SIZE / sizeof(uint16_t));

        if (!read_sector(filesystem_fat_start + first_cluster * 2 /
                                                 FAT16_SECTOR_SIZE,
                         sector)) {
            return FAT16_IO_ERROR;
        }
        for (entry_index = 0; entry_index < FAT16_SECTOR_SIZE / 2;
             ++entry_index) {
            uint32_t cluster = first_cluster + entry_index;
            FAT16_RESULT result;

            if (cluster < 2 || cluster >= filesystem_cluster_count + 2) {
                continue;
            }
            if (read_u16(sector + entry_index * 2) != 0) {
                continue;
            }
            result = set_fat_entry(cluster, FAT16_END_OF_CHAIN);
            if (result != FAT16_OK) {
                return result;
            }
            for (uint32_t sector_index = 0;
                 sector_index < filesystem_sectors_per_cluster;
                 ++sector_index) {
                bytes_set(sector, 0, sizeof(sector));
                if (!write_sector(cluster_to_lba(cluster) + sector_index,
                                  sector)) {
                    (void)set_fat_entry(cluster, 0);
                    return FAT16_IO_ERROR;
                }
            }
            *cluster_out = cluster;
            return FAT16_OK;
        }
    }
    return FAT16_NO_SPACE;
}

static FAT16_RESULT free_cluster_chain(uint32_t first_cluster) {
    uint32_t cluster = first_cluster;

    // Bound traversal by the volume size so a corrupt/cyclic chain cannot loop forever.
    for (uint32_t count = 0; count < filesystem_cluster_count; ++count) {
        uint16_t next;
        FAT16_RESULT result;

        if (!valid_cluster(cluster)) {
            return FAT16_CORRUPT;
        }
        result = get_fat_entry(cluster, &next);
        if (result != FAT16_OK) {
            return result;
        }
        if (next != 0 && next < FAT16_END_OF_CHAIN_MIN &&
            !valid_cluster(next)) {
            return FAT16_CORRUPT;
        }
        result = set_fat_entry(cluster, 0);
        if (result != FAT16_OK) {
            return result;
        }
        if (next >= FAT16_END_OF_CHAIN_MIN) {
            return FAT16_OK;
        }
        if (next == 0) {
            return FAT16_CORRUPT;
        }
        cluster = next;
    }
    return FAT16_CORRUPT;
}

static void cursor_initialize(FAT16_DIRECTORY_CURSOR *cursor,
                              FAT16_DIRECTORY directory) {
    // Force the first cursor_next call to load the first sector of this directory.
    bytes_set(cursor, 0, sizeof(*cursor));
    cursor->directory = directory;
    cursor->cluster = directory.cluster;
    cursor->entry_index = FAT16_SECTOR_SIZE / 32;
    cursor->finished = !directory.root && !valid_cluster(directory.cluster);
    if (!directory.root && !cursor->finished) {
        cursor->visited_clusters = 1;
    }
}

static FAT16_RESULT cursor_next(FAT16_DIRECTORY_CURSOR *cursor,
                                uint8_t entry[32],
                                FAT16_ENTRY_LOCATION *location, bool *found) {
    *found = false;
    if (cursor->finished) {
        return FAT16_OK;
    }

    // A sector holds sixteen entries; advance root sectors or follow cluster links.
    if (cursor->entry_index >= FAT16_SECTOR_SIZE / 32) {
        if (cursor->directory.root) {
            if (cursor->root_sector_index >= filesystem_root_sectors) {
                cursor->finished = true;
                return FAT16_OK;
            }
            cursor->lba = filesystem_root_start + cursor->root_sector_index++;
        } else {
            if (cursor->sector_in_cluster >= filesystem_sectors_per_cluster) {
                uint16_t next;
                FAT16_RESULT result = get_fat_entry(cursor->cluster, &next);

                if (result != FAT16_OK) {
                    return result;
                }
                if (next >= FAT16_END_OF_CHAIN_MIN) {
                    cursor->finished = true;
                    return FAT16_OK;
                }
                if (!valid_cluster(next) ||
                    ++cursor->visited_clusters > filesystem_cluster_count) {
                    return FAT16_CORRUPT;
                }
                cursor->cluster = next;
                cursor->sector_in_cluster = 0;
            }
            cursor->lba = cluster_to_lba(cursor->cluster) +
                          cursor->sector_in_cluster++;
        }
        if (!read_sector(cursor->lba, cursor->sector)) {
            return FAT16_IO_ERROR;
        }
        cursor->entry_index = 0;
    }

    bytes_copy(entry, cursor->sector + cursor->entry_index * 32, 32);
    location->lba = cursor->lba;
    location->entry_index = cursor->entry_index++;
    *found = true;
    return FAT16_OK;
}

static bool is_usable_entry(const uint8_t entry[32]) {
    return entry[0] != 0 && entry[0] != 0xE5 &&
           entry[11] != FAT16_ATTRIBUTE_LONG_NAME &&
           (entry[11] & FAT16_ATTRIBUTE_VOLUME) == 0;
}

typedef struct {
    uint16_t characters[FAT16_MAX_LFN_ENTRIES * 13];
    FAT16_ENTRY_LOCATION locations[FAT16_MAX_LFN_ENTRIES];
    uint8_t expected_ordinal;
    uint8_t total_entries;
    uint8_t checksum;
    bool valid;
} FAT16_LFN_STATE;

static void reset_lfn_state(FAT16_LFN_STATE *state) {
    state->expected_ordinal = 0;
    state->total_entries = 0;
    state->checksum = 0;
    state->valid = false;
}

static uint8_t short_name_checksum(const uint8_t name[11]) {
    uint8_t checksum = 0;

    for (uint32_t index = 0; index < 11; ++index) {
        checksum = (uint8_t)(((checksum & 1) ? 0x80 : 0) +
                             (checksum >> 1) + name[index]);
    }
    return checksum;
}

static void consume_lfn_entry(FAT16_LFN_STATE *state,
                             const uint8_t entry[32],
                             FAT16_ENTRY_LOCATION location) {
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    uint8_t ordinal = entry[0] & 0x1F;

    // VFAT entries appear in descending ordinal order; LAST starts a new sequence.
    if ((entry[0] & 0x40) != 0) {
        reset_lfn_state(state);
        state->expected_ordinal = ordinal;
        state->total_entries = ordinal;
        state->checksum = entry[13];
        state->valid = ordinal != 0 && ordinal <= FAT16_MAX_LFN_ENTRIES;
    }
    if (!state->valid || ordinal != state->expected_ordinal ||
        entry[11] != FAT16_ATTRIBUTE_LONG_NAME || entry[12] != 0 ||
        entry[13] != state->checksum || read_u16(entry + 26) != 0) {
        reset_lfn_state(state);
        return;
    }
    state->locations[ordinal - 1] = location;
    for (uint32_t index = 0; index < 13; ++index) {
        state->characters[(ordinal - 1) * 13 + index] =
            read_u16(entry + offsets[index]);
    }
    --state->expected_ordinal;
}

static bool lfn_to_ascii(const FAT16_LFN_STATE *state,
                         const uint8_t short_name[11], char name[256]) {
    uint32_t capacity;
    uint32_t position = 0;

    // Accept an LFN only when its sequence is complete and matches the short alias.
    if (!state->valid || state->expected_ordinal != 0 ||
        state->checksum != short_name_checksum(short_name)) {
        return false;
    }
    capacity = (uint32_t)state->total_entries * 13;
    while (position < capacity) {
        uint16_t character = state->characters[position];

        if (character == 0) {
            break;
        }
        if (character == 0xFFFF || character > 0x7F ||
            position >= FAT16_MAX_LFN_LENGTH) {
            return false;
        }
        name[position] = (char)character;
        ++position;
    }
    if (position == 0 || position > FAT16_MAX_LFN_LENGTH) {
        return false;
    }
    name[position] = '\0';
    return true;
}

static char ascii_upper(char value) {
    return value >= 'a' && value <= 'z'
               ? (char)(value - 'a' + 'A')
               : value;
}

static bool names_equal(const char *left, const char *right) {
    size_t index = 0;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        if (ascii_upper(left[index]) != ascii_upper(right[index])) {
            return false;
        }
        ++index;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static void format_short_name(const uint8_t entry[32], char name[13]);

static FAT16_RESULT find_entry_ex(FAT16_DIRECTORY directory,
                                  const char *name, uint8_t entry_out[32],
                                  FAT16_ENTRY_LOCATION *location, bool *found,
                                  FAT16_ENTRY_LOCATION *lfn_locations,
                                  uint8_t *lfn_count, char *resolved_name) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_RESULT result;
    FAT16_LFN_STATE lfn_state;

    // Retain LFN entry locations only for callers that may delete the whole entry set.
    *found = false;
    if (lfn_count != NULL) {
        *lfn_count = 0;
    }
    reset_lfn_state(&lfn_state);
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION entry_location;
        bool has_entry;

        result = cursor_next(&cursor, entry, &entry_location, &has_entry);
        if (result != FAT16_OK || !has_entry || entry[0] == 0) {
            return result;
        }
        if (entry[11] == FAT16_ATTRIBUTE_LONG_NAME && entry[0] != 0xE5) {
            consume_lfn_entry(&lfn_state, entry, entry_location);
            continue;
        }
        if (is_usable_entry(entry)) {
            char short_name[13];
            char long_name[256];
            bool has_long_name =
                lfn_to_ascii(&lfn_state, entry, long_name);
            bool matches;

            format_short_name(entry, short_name);
            matches = names_equal(name, short_name) ||
                      (has_long_name && names_equal(name, long_name));
            if (matches) {
                bytes_copy(entry_out, entry, 32);
                *location = entry_location;
                if (resolved_name != NULL) {
                    if (has_long_name) {
                        bytes_copy(resolved_name, long_name,
                                   string_length(long_name) + 1);
                    } else {
                        format_short_name(entry, resolved_name);
                    }
                }
                if (lfn_locations != NULL && lfn_count != NULL &&
                    has_long_name) {
                    *lfn_count = lfn_state.total_entries;
                    for (uint32_t index = 0; index < *lfn_count; ++index) {
                        lfn_locations[index] = lfn_state.locations[index];
                    }
                }
                *found = true;
                return FAT16_OK;
            }
        }
        reset_lfn_state(&lfn_state);
    }
}

static FAT16_RESULT find_entry(FAT16_DIRECTORY directory, const char *name,
                               uint8_t entry_out[32],
                               FAT16_ENTRY_LOCATION *location, bool *found) {
    return find_entry_ex(directory, name, entry_out, location, found, NULL,
                         NULL, NULL);
}

static FAT16_RESULT find_entry_short(FAT16_DIRECTORY directory,
                                    const uint8_t short_name[11],
                                    uint8_t entry_out[32],
                                    FAT16_ENTRY_LOCATION *location,
                                    bool *found) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_RESULT result;

    *found = false;
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION entry_location;
        bool has_entry;

        result = cursor_next(&cursor, entry, &entry_location, &has_entry);
        if (result != FAT16_OK || !has_entry || entry[0] == 0) {
            return result;
        }
        if (is_usable_entry(entry) && bytes_equal(entry, short_name, 11)) {
            bytes_copy(entry_out, entry, 32);
            *location = entry_location;
            *found = true;
            return FAT16_OK;
        }
    }
}

static FAT16_RESULT find_free_locations(
    FAT16_DIRECTORY directory, uint8_t count,
    FAT16_ENTRY_LOCATION locations[FAT16_MAX_LFN_ENTRIES + 1]) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_RESULT result;
    uint8_t free_count = 0;
    bool end_marker_seen = false;

    // FAT directory entries must be contiguous, and 0 marks the reusable tail.
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION location;
        bool has_entry;

        result = cursor_next(&cursor, entry, &location, &has_entry);
        if (result != FAT16_OK) {
            return result;
        }
        if (has_entry) {
            if (entry[0] == 0) {
                end_marker_seen = true;
            }
            if (end_marker_seen || entry[0] == 0xE5) {
                locations[free_count++] = location;
                if (free_count == count) {
                    return FAT16_OK;
                }
            } else {
                free_count = 0;
            }
            continue;
        }
        if (directory.root) {
            return FAT16_NO_SPACE;
        }

        uint32_t tail = cursor.cluster;
        uint16_t next;
        result = get_fat_entry(tail, &next);
        if (result != FAT16_OK) {
            return result;
        }
        if (next >= FAT16_END_OF_CHAIN_MIN) {
            uint32_t new_cluster;
            result = allocate_cluster(&new_cluster);
            if (result != FAT16_OK) {
                return result;
            }
            result = set_fat_entry(tail, (uint16_t)new_cluster);
            if (result != FAT16_OK) {
                return result;
            }
            cursor.cluster = new_cluster;
            cursor.sector_in_cluster = 0;
            cursor.entry_index = FAT16_SECTOR_SIZE / 32;
            cursor.finished = false;
            cursor.visited_clusters++;
            continue;
        }
        if (!valid_cluster(next)) {
            return FAT16_CORRUPT;
        }
        return FAT16_CORRUPT;
    }
}

static FAT16_RESULT write_entry(FAT16_ENTRY_LOCATION location,
                                const uint8_t entry[32]) {
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (location.entry_index >= FAT16_SECTOR_SIZE / 32 ||
        !read_sector(location.lba, sector)) {
        return FAT16_IO_ERROR;
    }
    bytes_copy(sector + location.entry_index * 32, entry, 32);
    return write_sector(location.lba, sector) ? FAT16_OK : FAT16_IO_ERROR;
}

static FAT16_RESULT mark_entry_deleted(FAT16_ENTRY_LOCATION location) {
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (location.entry_index >= FAT16_SECTOR_SIZE / 32 ||
        !read_sector(location.lba, sector)) {
        return FAT16_IO_ERROR;
    }
    sector[location.entry_index * 32] = 0xE5;
    return write_sector(location.lba, sector) ? FAT16_OK : FAT16_IO_ERROR;
}

static bool valid_short_name_character(char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-' || character == '$' || character == '~' ||
           character == '!' || character == '#' || character == '&' ||
           character == '\'' || character == '(' || character == ')' ||
           character == '@' || character == '^' || character == '`' ||
           character == '{' || character == '}';
}

static FAT16_RESULT make_short_name(const char *name, uint8_t short_name[11]) {
    size_t base_length = 0;
    size_t extension_length = 0;
    bool in_extension = false;
    bool saw_character = false;

    bytes_set(short_name, ' ', 11);
    if (name == NULL || name[0] == '\0') {
        return FAT16_INVALID_NAME;
    }

    for (size_t index = 0; name[index] != '\0'; ++index) {
        char character = name[index];

        if (character == '.') {
            if (in_extension || base_length == 0) {
                return FAT16_INVALID_NAME;
            }
            in_extension = true;
            continue;
        }
        if (character >= 'a' && character <= 'z') {
            character = (char)(character - 'a' + 'A');
        }
        if (!valid_short_name_character(character)) {
            return FAT16_INVALID_NAME;
        }
        saw_character = true;
        if (in_extension) {
            if (++extension_length > 3) {
                return FAT16_INVALID_NAME;
            }
            short_name[8 + extension_length - 1] = (uint8_t)character;
        } else {
            if (++base_length > 8) {
                return FAT16_INVALID_NAME;
            }
            short_name[base_length - 1] = (uint8_t)character;
        }
    }
    return saw_character ? FAT16_OK : FAT16_INVALID_NAME;
}

static bool entry_is_dot(const uint8_t entry[32]) {
    return entry[0] == '.' &&
           (entry[1] == ' ' || (entry[1] == '.' && entry[2] == ' '));
}

static void format_short_name(const uint8_t entry[32], char name[13]) {
    size_t position = 0;
    size_t base_end = 8;

    while (base_end > 0 && entry[base_end - 1] == ' ') {
        --base_end;
    }
    for (size_t index = 0; index < base_end; ++index) {
        name[position++] = (char)entry[index];
    }
    size_t extension_end = 11;
    while (extension_end > 8 && entry[extension_end - 1] == ' ') {
        --extension_end;
    }
    if (extension_end > 8) {
        name[position++] = '.';
        for (size_t index = 8; index < extension_end; ++index) {
            name[position++] = (char)entry[index];
        }
    }
    name[position] = '\0';
}

static FAT16_RESULT validate_long_name(const char *name, size_t *length_out) {
    size_t length = 0;

    if (name == NULL || name[0] == '\0') {
        return FAT16_INVALID_NAME;
    }
    // This implementation stores printable ASCII in VFAT's UTF-16 slots only.
    while (name[length] != '\0') {
        uint8_t character = (uint8_t)name[length];
        if (length == FAT16_MAX_LFN_LENGTH || character < 0x20 ||
            character >= 0x7F || character == '"' || character == '*' ||
            character == '/' || character == ':' || character == '<' ||
            character == '>' || character == '?' || character == '\\' ||
            character == '|') {
            return FAT16_INVALID_NAME;
        }
        ++length;
    }
    if (length == 0 || name[length - 1] == ' ' || name[length - 1] == '.' ||
        (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) {
        return FAT16_INVALID_NAME;
    }
    if (length_out != NULL) {
        *length_out = length;
    }
    return FAT16_OK;
}

static bool alias_character(char character) {
    return valid_short_name_character(ascii_upper(character));
}

static char sanitize_alias_character(char character) {
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    return alias_character(character) ? character : '_';
}

static FAT16_RESULT make_entry_name(FAT16_DIRECTORY directory,
                                   const char *name, uint8_t short_name[11],
                                   uint8_t *lfn_count) {
    size_t length;
    uint8_t existing[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_RESULT result;
    bool found;

    // Preserve valid 8.3 names; otherwise generate a collision-checked BASE~N alias.
    result = validate_long_name(name, &length);
    if (result != FAT16_OK) {
        return result;
    }
    result = find_entry(directory, name, existing, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (found) {
        return FAT16_ALREADY_EXISTS;
    }

    result = make_short_name(name, short_name);
    if (result == FAT16_OK) {
        result = find_entry_short(directory, short_name, existing, &location,
                                  &found);
        if (result != FAT16_OK) {
            return result;
        }
        if (found) {
            return FAT16_ALREADY_EXISTS;
        }
        *lfn_count = 0;
        return FAT16_OK;
    }

    size_t last_dot = length;
    for (size_t index = 0; index < length; ++index) {
        if (name[index] == '.') {
            last_dot = index;
        }
    }
    char base[256];
    char extension[4];
    size_t base_length = 0;
    size_t extension_length = 0;
    for (size_t index = 0; index < last_dot; ++index) {
        if (name[index] == ' ' || name[index] == '.') {
            continue;
        }
        base[base_length++] = sanitize_alias_character(name[index]);
    }
    if (base_length == 0) {
        base[base_length++] = '_';
    }
    for (size_t index = last_dot + (last_dot < length ? 1 : 0);
         index < length && extension_length < 3; ++index) {
        if (name[index] == ' ' || name[index] == '.') {
            continue;
        }
        extension[extension_length++] = sanitize_alias_character(name[index]);
    }

    for (uint32_t suffix = 1; suffix <= filesystem_cluster_count; ++suffix) {
        char digits[10];
        uint32_t digit_count = 0;
        uint32_t value = suffix;
        uint32_t prefix_length;

        do {
            digits[digit_count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value != 0 && digit_count < sizeof(digits));
        if (digit_count >= 7) {
            return FAT16_NO_SPACE;
        }
        bytes_set(short_name, ' ', 11);
        prefix_length = 8 - digit_count - 1;
        if (prefix_length > base_length) {
            prefix_length = (uint32_t)base_length;
        }
        bytes_copy(short_name, base, prefix_length);
        short_name[prefix_length++] = '~';
        while (digit_count != 0) {
            short_name[prefix_length++] = (uint8_t)digits[--digit_count];
        }
        bytes_copy(short_name + 8, extension, extension_length);
        result = find_entry_short(directory, short_name, existing, &location,
                                  &found);
        if (result != FAT16_OK) {
            return result;
        }
        if (!found) {
            *lfn_count =
                (uint8_t)((length + 1 + 12) / 13);
            return FAT16_OK;
        }
    }
    return FAT16_NO_SPACE;
}

static void create_lfn_entry(uint8_t entry[32], const char *name,
                             size_t length, uint8_t ordinal,
                             uint8_t total_entries,
                             const uint8_t short_name[11]) {
    static const uint8_t offsets[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    uint32_t character_offset = (uint32_t)(ordinal - 1) * 13;

    // Pad unused UTF-16 slots with 0xFFFF and terminate the name with zero.
    bytes_set(entry, 0xFF, 32);
    entry[0] = ordinal | (ordinal == total_entries ? 0x40 : 0);
    entry[11] = FAT16_ATTRIBUTE_LONG_NAME;
    entry[12] = 0;
    entry[13] = short_name_checksum(short_name);
    write_u16(entry + 26, 0);
    for (uint32_t index = 0; index < 13; ++index) {
        uint32_t position = character_offset + index;
        uint16_t character = position < length
                                 ? (uint8_t)name[position]
                                 : (position == length ? 0 : 0xFFFF);
        write_u16(entry + offsets[index], character);
    }
}

static void create_short_entry(uint8_t entry[32], const uint8_t name[11],
                               uint8_t attributes, uint32_t first_cluster,
                               uint32_t size);

static FAT16_RESULT write_named_entry(
    FAT16_DIRECTORY directory, const char *name, uint8_t attributes,
    uint32_t first_cluster, uint32_t size, FAT16_ENTRY_LOCATION *short_location,
    uint8_t short_name_out[11]) {
    FAT16_ENTRY_LOCATION locations[FAT16_MAX_LFN_ENTRIES + 1];
    FAT16_ENTRY_LOCATION short_location_local;
    uint8_t short_name[11];
    uint8_t lfn_count;
    uint8_t entry[32];
    size_t name_length;
    FAT16_RESULT result = make_entry_name(directory, name, short_name,
                                         &lfn_count);

    if (result != FAT16_OK) {
        return result;
    }
    result = validate_long_name(name, &name_length);
    if (result != FAT16_OK) {
        return result;
    }
    // Write LFN slots first and the short entry last, so it remains the commit point.
    result = find_free_locations(directory, lfn_count + 1, locations);
    if (result != FAT16_OK) {
        return result;
    }
    for (uint8_t index = 0; index < lfn_count; ++index) {
        create_lfn_entry(entry, name, name_length,
                         (uint8_t)(lfn_count - index), lfn_count, short_name);
        result = write_entry(locations[index], entry);
        if (result != FAT16_OK) {
            return result;
        }
    }
    short_location_local = locations[lfn_count];
    create_short_entry(entry, short_name, attributes, first_cluster, size);
    result = write_entry(short_location_local, entry);
    if (result == FAT16_OK) {
        if (short_location != NULL) {
            *short_location = short_location_local;
        }
        if (short_name_out != NULL) {
            bytes_copy(short_name_out, short_name, 11);
        }
    }
    return result;
}

static void create_short_entry(uint8_t entry[32], const uint8_t name[11],
                               uint8_t attributes, uint32_t first_cluster,
                               uint32_t size = 0) {
    bytes_set(entry, 0, 32);
    bytes_copy(entry, name, 11);
    entry[11] = attributes;
    write_u16(entry + 26, (uint16_t)first_cluster);
    write_u16(entry + 28, (uint16_t)size);
    write_u16(entry + 30, (uint16_t)(size >> 16));
}

static FAT16_RESULT initialize_directory_cluster(uint32_t cluster,
                                                  uint32_t parent_cluster) {
    uint8_t sector[FAT16_SECTOR_SIZE];
    uint8_t dot[32];
    uint8_t dotdot[32];

    // New directories begin with the required self and parent references.
    bytes_set(sector, 0, sizeof(sector));
    create_short_entry(dot, (const uint8_t *)".          ",
                       FAT16_ATTRIBUTE_DIRECTORY, cluster);
    create_short_entry(dotdot, (const uint8_t *)"..         ",
                       FAT16_ATTRIBUTE_DIRECTORY, parent_cluster);
    bytes_copy(sector, dot, sizeof(dot));
    bytes_copy(sector + sizeof(dot), dotdot, sizeof(dotdot));
    if (!write_sector(cluster_to_lba(cluster), sector)) {
        return FAT16_IO_ERROR;
    }
    for (uint32_t index = 1; index < filesystem_sectors_per_cluster; ++index) {
        bytes_set(sector, 0, sizeof(sector));
        if (!write_sector(cluster_to_lba(cluster) + index, sector)) {
            return FAT16_IO_ERROR;
        }
    }
    return FAT16_OK;
}

static FAT16_RESULT directory_is_empty(uint32_t cluster, bool *is_empty) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_DIRECTORY directory = {cluster, false};

    // Deleted, volume-label, LFN, and dot entries do not make a directory non-empty.
    *is_empty = true;
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION location;
        bool has_entry;
        FAT16_RESULT result =
            cursor_next(&cursor, entry, &location, &has_entry);

        (void)location;
        if (result != FAT16_OK || !has_entry || entry[0] == 0) {
            return result;
        }
        if (is_usable_entry(entry) && !entry_is_dot(entry)) {
            *is_empty = false;
            return FAT16_OK;
        }
    }
}

static FAT16_RESULT remove_directory_contents(uint32_t cluster,
                                             uint32_t depth) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_DIRECTORY directory = {cluster, false};

    // Recursive deletion shares the caller's transaction limits via a fixed depth cap.
    if (depth >= FAT16_MAX_DELETE_DEPTH) {
        return FAT16_PATH_TOO_DEEP;
    }
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION location;
        bool has_entry;
        FAT16_RESULT result =
            cursor_next(&cursor, entry, &location, &has_entry);

        if (result != FAT16_OK || !has_entry || entry[0] == 0) {
            return result;
        }
        if (entry[0] == 0xE5) {
            continue;
        }
        if (entry[11] == FAT16_ATTRIBUTE_LONG_NAME) {
            result = mark_entry_deleted(location);
            if (result != FAT16_OK) {
                return result;
            }
            continue;
        }
        if (!is_usable_entry(entry) || entry_is_dot(entry)) {
            continue;
        }

        uint32_t first_cluster = read_u16(entry + 26);
        if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
            if (!valid_cluster(first_cluster)) {
                return FAT16_CORRUPT;
            }
            result = remove_directory_contents(first_cluster, depth + 1);
            if (result != FAT16_OK) {
                return result;
            }
        } else if (first_cluster != 0) {
            result = free_cluster_chain(first_cluster);
            if (result != FAT16_OK) {
                return result;
            }
        }
        result = mark_entry_deleted(location);
        if (result != FAT16_OK) {
            return result;
        }
    }
}

static FAT16_RESULT add_directory_entry(FAT16_DIRECTORY directory,
                                        const char *name,
                                        uint8_t attributes,
                                        uint32_t first_cluster,
                                        uint32_t size = 0) {
    uint8_t existing[32];
    FAT16_ENTRY_LOCATION location;
    bool found;
    // Check long-name and alias collisions before writing any directory slots.
    FAT16_RESULT result =
        find_entry(directory, name, existing, &location, &found);

    if (result != FAT16_OK || found) {
        return result != FAT16_OK ? result : FAT16_ALREADY_EXISTS;
    }
    return write_named_entry(directory, name, attributes, first_cluster, size,
                             NULL, NULL);
}

extern "C" FAT16_RESULT fat16_mount(const FAT16_IO *io) {
    uint8_t boot_sector[FAT16_SECTOR_SIZE];
    uint32_t total_sectors;
    uint32_t fat_sectors;
    uint32_t root_sectors;
    uint32_t data_start;
    uint32_t cluster_count;
    uint16_t bytes_per_sector;
    uint16_t root_entries;
    uint16_t reserved_sectors;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;

    // Require read, write, and flush callbacks because mutations depend on ordering.
    if (io == NULL || io->read_blocks == NULL || io->write_blocks == NULL ||
        io->flush_blocks == NULL || io->block_size != FAT16_SECTOR_SIZE ||
        io->block_count == 0) {
        return FAT16_CORRUPT;
    }
    if (io->block_count > UINT32_MAX) {
        return FAT16_CORRUPT;
    }
    bytes_copy(&filesystem_io, io, sizeof(filesystem_io));
    filesystem_total_sectors = (uint32_t)io->block_count;
    if (!filesystem_io.read_blocks(filesystem_io.context, 0, 1, io_sector)) {
        return FAT16_IO_ERROR;
    }
    bytes_copy(boot_sector, io_sector, sizeof(boot_sector));

    // Decode the BIOS parameter block only after the initial sector read succeeds.
    bytes_per_sector = read_u16(boot_sector + 11);
    sectors_per_cluster = boot_sector[13];
    reserved_sectors = read_u16(boot_sector + 14);
    fat_count = boot_sector[16];
    root_entries = read_u16(boot_sector + 17);
    total_sectors = read_u16(boot_sector + 19);
    if (total_sectors == 0) {
        total_sectors = read_u32(boot_sector + 32);
    }
    fat_sectors = read_u16(boot_sector + 22);
    root_sectors = ((uint32_t)root_entries * 32 + FAT16_SECTOR_SIZE - 1) /
                   FAT16_SECTOR_SIZE;
    data_start = reserved_sectors + (uint32_t)fat_count * fat_sectors +
                 root_sectors;

    if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA ||
        bytes_per_sector != FAT16_SECTOR_SIZE ||
        sectors_per_cluster == 0 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || fat_count > 2 ||
        root_entries == 0 || root_entries % (FAT16_SECTOR_SIZE / 32) != 0 ||
        fat_sectors == 0 || total_sectors > io->block_count ||
        total_sectors <= data_start ||
        read_u32(boot_sector + 39) != FAT16_DATA_DISK_VOLUME_ID ||
        !bytes_equal(boot_sector + 43, FAT16_DATA_DISK_LABEL, 11)) {
        return FAT16_CORRUPT;
    }

    // FAT type is inferred from data-cluster count, not the boot-sector label text.
    cluster_count =
        (total_sectors - data_start) / sectors_per_cluster;
    if (cluster_count < 4085 || cluster_count > 65524 ||
        ((uint64_t)cluster_count + 2) * 2 >
            (uint64_t)fat_sectors * FAT16_SECTOR_SIZE) {
        return FAT16_CORRUPT;
    }

    filesystem_total_sectors = total_sectors;
    filesystem_fat_start = reserved_sectors;
    filesystem_fat_sectors = fat_sectors;
    filesystem_root_start = reserved_sectors +
                            (uint32_t)fat_count * fat_sectors;
    filesystem_root_sectors = root_sectors;
    filesystem_data_start = data_start;
    filesystem_cluster_count = cluster_count;
    filesystem_fat_count = fat_count;
    filesystem_sectors_per_cluster = sectors_per_cluster;
    current_directory_depth = 0;
    filesystem_virtual_root_active = false;
    filesystem_virtual_root_cluster = 0;
    filesystem_virtual_root_name[0] = '\0';
    filesystem_read_only_cluster = 0;
    filesystem_read_only_parent_cluster = 0;
    current_directory_stack[0] = FAT16_ROOT_CLUSTER;
    return FAT16_OK;
}

static FAT16_RESULT change_directory_component(const char *name) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY parent;
    char resolved_name[FAT16_MAX_NAME_LENGTH];
    bool found;
    FAT16_RESULT result;

    // Keep a stack of cluster IDs and display names for pwd and parent traversal.
    if (name == NULL || name[0] == '\0') {
        return FAT16_INVALID_NAME;
    }
    if (name[0] == '.' && name[1] == '\0') {
        return FAT16_OK;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        if (current_directory_depth > 0) {
            --current_directory_depth;
        }
        return FAT16_OK;
    }
    if (current_directory_depth >= FAT16_MAX_DIRECTORY_DEPTH) {
        return FAT16_PATH_TOO_DEEP;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry_ex(parent, name, entry, &location, &found, NULL, NULL,
                           resolved_name);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) == 0) {
        return FAT16_NOT_DIRECTORY;
    }

    current_directory_stack[current_directory_depth + 1] =
        read_u16(entry + 26);
    bytes_copy(current_directory_names[current_directory_depth], resolved_name,
               string_length(resolved_name) + 1);
    ++current_directory_depth;
    return FAT16_OK;
}

static bool path_component_equals_root(const char *component) {
    size_t index = 0;

    while (component[index] != '\0' &&
           filesystem_virtual_root_name[index] != '\0') {
        char left = component[index];
        char right = filesystem_virtual_root_name[index];
        if (left >= 'a' && left <= 'z') {
            left = (char)(left - 'a' + 'A');
        }
        if (right >= 'a' && right <= 'z') {
            right = (char)(right - 'a' + 'A');
        }
        if (left != right) {
            return false;
        }
        ++index;
    }
    return component[index] == '\0' &&
           filesystem_virtual_root_name[index] == '\0';
}

static FAT16_RESULT change_directory_path_in_current(const char *path) {
    const char *position;
    bool absolute;
    bool first_component = true;

    if (path == NULL || path[0] == '\0') {
        return FAT16_INVALID_NAME;
    }
    // Accept either separator so paths work consistently from host and shell input.
    absolute = path[0] == '/' || path[0] == '\\';
    if (absolute) {
        current_directory_depth = 0;
        current_directory_stack[0] =
            filesystem_virtual_root_active ? filesystem_virtual_root_cluster
                                           : FAT16_ROOT_CLUSTER;
    }
    position = path;
    while (*position != '\0') {
        char component[FAT16_MAX_NAME_LENGTH];
        size_t length = 0;

        while (*position == '/' || *position == '\\') {
            ++position;
        }
        if (*position == '\0') {
            break;
        }
        while (*position != '\0' && *position != '/' && *position != '\\') {
            if (length == FAT16_MAX_LFN_LENGTH) {
                return FAT16_INVALID_NAME;
            }
            component[length++] = *position++;
        }
        component[length] = '\0';
        while (*position == '/' || *position == '\\') {
            ++position;
        }
        if (absolute && first_component && filesystem_virtual_root_active &&
            path_component_equals_root(component)) {
            first_component = false;
            continue;
        }
        first_component = false;
        FAT16_RESULT result = change_directory_component(component);
        if (result != FAT16_OK) {
            return result;
        }
    }
    return FAT16_OK;
}

extern "C" FAT16_RESULT fat16_set_root_directory(const char *name) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY volume_root = {FAT16_ROOT_CLUSTER, true};
    FAT16_RESULT result;
    bool found;
    char resolved_name[FAT16_MAX_NAME_LENGTH];

    // Root redirection is a one-time mount policy and must happen before traversal.
    if (filesystem_virtual_root_active || current_directory_depth != 0) {
        return FAT16_CORRUPT;
    }
    result = find_entry_ex(volume_root, name, entry, &location, &found, NULL,
                           NULL, resolved_name);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) == 0) {
        return FAT16_NOT_DIRECTORY;
    }

    filesystem_virtual_root_cluster = read_u16(entry + 26);
    if (!valid_cluster(filesystem_virtual_root_cluster)) {
        return FAT16_CORRUPT;
    }
    bytes_copy(filesystem_virtual_root_name, resolved_name,
               string_length(resolved_name) + 1);
    filesystem_virtual_root_active = true;
    current_directory_stack[0] = filesystem_virtual_root_cluster;
    return FAT16_OK;
}

extern "C" FAT16_RESULT fat16_protect_directory(const char *name) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;
    bool found;

    // Protection is tracked by cluster identity, so aliases cannot bypass it.
    if (!filesystem_virtual_root_active) {
        return FAT16_CORRUPT;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(parent, name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) == 0) {
        return FAT16_NOT_DIRECTORY;
    }
    filesystem_read_only_cluster = read_u16(entry + 26);
    filesystem_read_only_parent_cluster =
        current_directory_stack[current_directory_depth];
    if (!valid_cluster(filesystem_read_only_cluster)) {
        filesystem_read_only_cluster = 0;
        filesystem_read_only_parent_cluster = 0;
        return FAT16_CORRUPT;
    }
    return FAT16_OK;
}

static FAT16_RESULT make_directory_in_current(const char *name) {
    uint32_t new_cluster;
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;
    uint8_t short_name[11];
    uint8_t lfn_count;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = make_entry_name(parent, name, short_name, &lfn_count);
    if (result != FAT16_OK) {
        return result;
    }
    // Initialize and flush the new directory's dot entries before linking its parent.
    result = allocate_cluster(&new_cluster);
    if (result != FAT16_OK) {
        return result;
    }
    result = initialize_directory_cluster(
        new_cluster, current_directory_is_volume_root()
                         ? FAT16_ROOT_CLUSTER
                         : current_directory_stack[current_directory_depth]);
    if (result != FAT16_OK) {
        (void)set_fat_entry(new_cluster, 0);
        return result;
    }
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }
    result = add_directory_entry(parent, name,
                                 FAT16_ATTRIBUTE_DIRECTORY, new_cluster);
    if (result != FAT16_OK) {
        // The directory entry write may have committed despite an I/O error.
        return result;
    }
    return flush_device();
}

static FAT16_RESULT make_file_in_current(const char *name) {
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = add_directory_entry(parent, name, 0x20, 0);
    return result == FAT16_OK ? flush_device() : result;
}

static FAT16_RESULT read_file_in_current(const char *name, char *buffer,
                                         uint32_t buffer_capacity,
                                         uint32_t *size) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY directory;
    FAT16_RESULT result;
    bool found;
    uint32_t remaining;
    uint32_t cluster;
    uint32_t visited_clusters = 0;
    uint32_t cluster_size =
        (uint32_t)filesystem_sectors_per_cluster * FAT16_SECTOR_SIZE;
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (size == NULL || (buffer == NULL && buffer_capacity != 0)) {
        return FAT16_INVALID_NAME;
    }
    *size = 0;
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(directory, name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return FAT16_NOT_DIRECTORY;
    }

    // The directory size is authoritative; only read enough clusters to satisfy it.
    *size = read_u32(entry + 28);
    if (*size > buffer_capacity) {
        return FAT16_FILE_TOO_LARGE;
    }
    remaining = *size;
    cluster = read_u16(entry + 26);
    while (remaining != 0) {
        uint32_t bytes_in_cluster;

        if (!valid_cluster(cluster) ||
            ++visited_clusters > filesystem_cluster_count) {
            return FAT16_CORRUPT;
        }
        bytes_in_cluster =
            remaining < cluster_size ? remaining : cluster_size;
        for (uint32_t sector_index = 0;
             sector_index < filesystem_sectors_per_cluster &&
             bytes_in_cluster != 0;
             ++sector_index) {
            uint32_t bytes_in_sector =
                bytes_in_cluster < FAT16_SECTOR_SIZE
                    ? bytes_in_cluster
                    : FAT16_SECTOR_SIZE;
            if (!read_sector(cluster_to_lba(cluster) + sector_index, sector)) {
                return FAT16_IO_ERROR;
            }
            bytes_copy(buffer + (*size - remaining), sector, bytes_in_sector);
            remaining -= bytes_in_sector;
            bytes_in_cluster -= bytes_in_sector;
        }
        if (remaining != 0) {
            uint16_t next;
            result = get_fat_entry(cluster, &next);
            if (result != FAT16_OK) {
                return result;
            }
            if (!valid_cluster(next)) {
                return FAT16_CORRUPT;
            }
            cluster = next;
        }
    }
    return FAT16_OK;
}

static FAT16_RESULT write_file_in_current(const char *name,
                                          const char *buffer,
                                          uint32_t size) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY directory;
    FAT16_RESULT result;
    bool found;
    uint32_t old_first_cluster = 0;
    uint32_t new_first_cluster = 0;
    uint32_t previous_cluster = 0;
    uint32_t written = 0;
    uint32_t cluster_size =
        (uint32_t)filesystem_sectors_per_cluster * FAT16_SECTOR_SIZE;
    uint8_t sector[FAT16_SECTOR_SIZE];

    if (buffer == NULL && size != 0) {
        return FAT16_INVALID_NAME;
    }
    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(directory, name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    result = validate_long_name(name, NULL);
    if (result != FAT16_OK) {
        return result;
    }
    if (found) {
        if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
            return FAT16_NOT_DIRECTORY;
        }
        old_first_cluster = read_u16(entry + 26);
    } else {
        uint8_t generated_alias[11];
        uint8_t lfn_count;
        result = make_entry_name(directory, name, generated_alias, &lfn_count);
        if (result != FAT16_OK) {
            return result;
        }
    }

    // Copy-on-write ordering: stage data and a new chain before changing the name.
    while (written < size) {
        uint32_t new_cluster;
        uint32_t bytes_in_cluster =
            size - written < cluster_size ? size - written : cluster_size;

        result = allocate_cluster(&new_cluster);
        if (result != FAT16_OK) {
            if (new_first_cluster != 0) {
                (void)free_cluster_chain(new_first_cluster);
            }
            return result;
        }
        if (new_first_cluster == 0) {
            new_first_cluster = new_cluster;
        } else {
            result = set_fat_entry(previous_cluster, (uint16_t)new_cluster);
            if (result != FAT16_OK) {
                // The link write may have reached disk; keep the staged
                // chain intact rather than freeing a possibly linked cluster.
                return result;
            }
        }
        previous_cluster = new_cluster;

        for (uint32_t sector_index = 0;
             sector_index < filesystem_sectors_per_cluster &&
             bytes_in_cluster != 0;
             ++sector_index) {
            uint32_t bytes_in_sector =
                bytes_in_cluster < FAT16_SECTOR_SIZE
                    ? bytes_in_cluster
                    : FAT16_SECTOR_SIZE;
            bytes_set(sector, 0, sizeof(sector));
            bytes_copy(sector, buffer + written, bytes_in_sector);
            if (!write_sector(cluster_to_lba(new_cluster) + sector_index,
                              sector)) {
                (void)free_cluster_chain(new_first_cluster);
                return FAT16_IO_ERROR;
            }
            written += bytes_in_sector;
            bytes_in_cluster -= bytes_in_sector;
        }
    }

    // The old entry remains intact unless every new data cluster is durable.
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }

    // Switch the directory entry only after the replacement chain is flushed.
    if (found) {
        write_u16(entry + 26, (uint16_t)new_first_cluster);
        write_u16(entry + 28, (uint16_t)size);
        write_u16(entry + 30, (uint16_t)(size >> 16));
        result = write_entry(location, entry);
    } else {
        result = write_named_entry(directory, name, 0x20, new_first_cluster,
                                   size, NULL, NULL);
    }
    // The directory write may have reached disk even when the device reports
    // an error; retain both chains until the commit is known to be durable.
    if (result != FAT16_OK) {
        return result;
    }
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }
    // Reclaim the old chain only after the new directory entry is durable.
    if (old_first_cluster != 0) {
        result = free_cluster_chain(old_first_cluster);
        if (result != FAT16_OK) {
            return result;
        }
        result = flush_device();
    }
    return result;
}

static FAT16_RESULT remove_directory_in_current(const char *name,
                                                bool recursive) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_ENTRY_LOCATION lfn_locations[FAT16_MAX_LFN_ENTRIES];
    uint8_t lfn_count;
    FAT16_DIRECTORY parent;
    uint32_t target_cluster;
    bool found;
    bool is_empty;
    FAT16_RESULT result;

    // Delete contents before releasing the directory cluster itself.
    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    if (current_directory_depth == 0 &&
        (name == NULL || name[0] == '\0')) {
        return FAT16_ROOT_DIRECTORY;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry_ex(parent, name, entry, &location, &found,
                           lfn_locations, &lfn_count, NULL);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) == 0) {
        return FAT16_NOT_DIRECTORY;
    }
    target_cluster = read_u16(entry + 26);
    if (!valid_cluster(target_cluster)) {
        return FAT16_CORRUPT;
    }
    if (filesystem_virtual_root_active &&
        target_cluster == filesystem_virtual_root_cluster) {
        return FAT16_ROOT_DIRECTORY;
    }
    if (target_cluster == filesystem_read_only_cluster ||
        target_cluster == filesystem_read_only_parent_cluster) {
        return FAT16_READ_ONLY;
    }
    for (uint32_t index = 1; index <= current_directory_depth; ++index) {
        if (current_directory_stack[index] == target_cluster) {
            return FAT16_CURRENT_DIRECTORY;
        }
    }
    if (!recursive) {
        result = directory_is_empty(target_cluster, &is_empty);
        if (result != FAT16_OK) {
            return result;
        }
        if (!is_empty) {
            return FAT16_NOT_EMPTY;
        }
    } else {
        result = remove_directory_contents(target_cluster, 0);
        if (result != FAT16_OK) {
            return result;
        }
    }
    result = mark_entry_deleted(location);
    if (result != FAT16_OK) {
        return result;
    }
    for (uint8_t index = 0; index < lfn_count; ++index) {
        result = mark_entry_deleted(lfn_locations[index]);
        if (result != FAT16_OK) {
            return result;
        }
    }
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }
    result = free_cluster_chain(target_cluster);
    return result == FAT16_OK ? flush_device() : result;
}

extern "C" FAT16_RESULT fat16_list_directory(FAT16_LIST_ENTRY callback,
                                              void *context) {
    FAT16_DIRECTORY directory;
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_LFN_STATE lfn_state;

    // Keep callback-based listing allocation-free; each emitted name is stack-local.
    if (callback == NULL) {
        return FAT16_CORRUPT;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    reset_lfn_state(&lfn_state);
    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        FAT16_ENTRY_LOCATION location;
        bool has_entry;
        FAT16_RESULT result =
            cursor_next(&cursor, entry, &location, &has_entry);

        (void)location;
        if (result != FAT16_OK || !has_entry || entry[0] == 0) {
            return result;
        }
        if (entry[11] == FAT16_ATTRIBUTE_LONG_NAME && entry[0] != 0xE5) {
            consume_lfn_entry(&lfn_state, entry, location);
            continue;
        }
        if (!is_usable_entry(entry) || entry_is_dot(entry)) {
            reset_lfn_state(&lfn_state);
            continue;
        }
        char name[FAT16_MAX_NAME_LENGTH];
        if (!lfn_to_ascii(&lfn_state, entry, name)) {
            format_short_name(entry, name);
        }
        callback(context, name,
                 (entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0,
                 read_u32(entry + 28));
        reset_lfn_state(&lfn_state);
    }
}

extern "C" bool fat16_get_path(char *buffer, size_t buffer_size) {
    size_t position = 0;

    // Reconstruct from the saved directory names, preserving the virtual root label.
    if (buffer == NULL || buffer_size < 2) {
        return false;
    }
    buffer[position++] = '\\';
    if (filesystem_virtual_root_active) {
        const char *name = filesystem_virtual_root_name;
        while (*name != '\0') {
            if (position + 1 >= buffer_size) {
                return false;
            }
            buffer[position++] = *name++;
        }
        if (current_directory_depth > 0) {
            if (position + 1 >= buffer_size) {
                return false;
            }
            buffer[position++] = '\\';
        }
    }
    for (uint32_t index = 0; index < current_directory_depth; ++index) {
        if (position + 1 >= buffer_size) {
            return false;
        }
        const char *name = current_directory_names[index];
        while (*name != '\0') {
            if (position + 1 >= buffer_size) {
                return false;
            }
            buffer[position++] = *name++;
        }
        if (index + 1 < current_directory_depth) {
            if (position + 1 >= buffer_size) {
                return false;
            }
            buffer[position++] = '\\';
        }
    }
    buffer[position] = '\0';
    return true;
}

typedef struct {
    uint32_t depth;
    uint32_t clusters[FAT16_MAX_DIRECTORY_DEPTH + 1];
} FAT16_DIRECTORY_STATE;

static char saved_directory_names[FAT16_MAX_DIRECTORY_DEPTH]
                                 [FAT16_MAX_NAME_LENGTH];

static void save_directory_state(FAT16_DIRECTORY_STATE *state) {
    state->depth = current_directory_depth;
    bytes_copy(state->clusters, current_directory_stack,
               sizeof(current_directory_stack));
    bytes_copy(saved_directory_names, current_directory_names,
               sizeof(current_directory_names));
}

static void restore_directory_state(const FAT16_DIRECTORY_STATE *state) {
    current_directory_depth = state->depth;
    bytes_copy(current_directory_stack, state->clusters,
               sizeof(current_directory_stack));
    bytes_copy(current_directory_names, saved_directory_names,
               sizeof(current_directory_names));
}

static FAT16_RESULT resolve_parent_path(
    const char *path, char basename[FAT16_MAX_NAME_LENGTH]) {
    const char *position;
    bool absolute;
    bool first_component = true;

    if (path == NULL || path[0] == '\0') {
        return FAT16_INVALID_NAME;
    }
    // Resolve every component except the basename, leaving the current-directory
    // stack positioned at the parent for the caller's final operation.
    absolute = path[0] == '/' || path[0] == '\\';
    if (absolute) {
        current_directory_depth = 0;
        current_directory_stack[0] =
            filesystem_virtual_root_active ? filesystem_virtual_root_cluster
                                           : FAT16_ROOT_CLUSTER;
    }
    position = path;
    while (*position != '\0') {
        char component[FAT16_MAX_NAME_LENGTH];
        size_t length = 0;
        bool had_separator = false;

        while (*position == '/' || *position == '\\') {
            ++position;
        }
        if (*position == '\0') {
            return FAT16_INVALID_NAME;
        }
        had_separator = false;
        while (*position != '\0' && *position != '/' && *position != '\\') {
            if (length == FAT16_MAX_LFN_LENGTH) {
                return FAT16_INVALID_NAME;
            }
            component[length++] = *position++;
        }
        component[length] = '\0';
        while (*position == '/' || *position == '\\') {
            had_separator = true;
            ++position;
        }
        if (*position == '\0') {
            if (had_separator) {
                return FAT16_INVALID_NAME;
            }
            bytes_copy(basename, component, length + 1);
            return FAT16_OK;
        }
        if (absolute && first_component && filesystem_virtual_root_active &&
            path_component_equals_root(component)) {
            first_component = false;
            continue;
        }
        first_component = false;
        FAT16_RESULT result = change_directory_component(component);
        if (result != FAT16_OK) {
            return result;
        }
    }
    return FAT16_INVALID_NAME;
}

static bool path_is_virtual_root(const char *path) {
    const char *position;
    char component[FAT16_MAX_NAME_LENGTH];
    size_t length = 0;

    if (path == NULL || (path[0] != '/' && path[0] != '\\')) {
        return false;
    }
    position = path;
    while (*position == '/' || *position == '\\') {
        ++position;
    }
    if (*position == '\0') {
        return true;
    }
    while (*position != '\0' && *position != '/' && *position != '\\') {
        if (length == FAT16_MAX_LFN_LENGTH) {
            return false;
        }
        component[length++] = *position++;
    }
    component[length] = '\0';
    while (*position == '/' || *position == '\\') {
        ++position;
    }
    return *position == '\0' && path_component_equals_root(component);
}

static FAT16_RESULT remove_file_in_current(const char *name) {
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_ENTRY_LOCATION lfn_locations[FAT16_MAX_LFN_ENTRIES];
    uint8_t lfn_count;
    FAT16_DIRECTORY directory;
    FAT16_RESULT result;
    bool found;
    uint32_t first_cluster;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry_ex(directory, name, entry, &location, &found,
                           lfn_locations, &lfn_count, NULL);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return FAT16_IS_DIRECTORY;
    }
    // Remove the name from disk before freeing clusters that it may still reference.
    first_cluster = read_u16(entry + 26);
    result = mark_entry_deleted(location);
    if (result != FAT16_OK) {
        return result;
    }
    for (uint8_t index = 0; index < lfn_count; ++index) {
        result = mark_entry_deleted(lfn_locations[index]);
        if (result != FAT16_OK) {
            return result;
        }
    }
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }
    if (first_cluster != 0) {
        result = free_cluster_chain(first_cluster);
        if (result != FAT16_OK) {
            return result;
        }
    }
    return flush_device();
}

static FAT16_RESULT duplicate_file_chain(uint32_t source_first_cluster,
                                         uint32_t size,
                                         uint32_t *destination_first_cluster) {
    uint32_t source_cluster = source_first_cluster;
    uint32_t destination_previous = 0;
    uint32_t remaining = size;
    uint32_t visited = 0;
    uint32_t cluster_size =
        (uint32_t)filesystem_sectors_per_cluster * FAT16_SECTOR_SIZE;
    uint8_t sector[FAT16_SECTOR_SIZE];

    // Copy only the file-size bytes; the final source cluster may contain slack.
    *destination_first_cluster = 0;
    if (size == 0) {
        return FAT16_OK;
    }
    while (remaining != 0) {
        uint32_t destination_cluster;
        uint32_t bytes_in_cluster =
            remaining < cluster_size ? remaining : cluster_size;
        FAT16_RESULT result;

        if (!valid_cluster(source_cluster) ||
            ++visited > filesystem_cluster_count) {
            return FAT16_CORRUPT;
        }
        result = allocate_cluster(&destination_cluster);
        if (result != FAT16_OK) {
            if (*destination_first_cluster != 0) {
                (void)free_cluster_chain(*destination_first_cluster);
            }
            return result;
        }
        if (*destination_first_cluster == 0) {
            *destination_first_cluster = destination_cluster;
        } else {
            result = set_fat_entry(destination_previous,
                                   (uint16_t)destination_cluster);
            if (result != FAT16_OK) {
                return result;
            }
        }
        destination_previous = destination_cluster;
        for (uint32_t sector_index = 0;
             sector_index < filesystem_sectors_per_cluster &&
             bytes_in_cluster != 0;
             ++sector_index) {
            uint32_t bytes_in_sector =
                bytes_in_cluster < FAT16_SECTOR_SIZE
                    ? bytes_in_cluster
                    : FAT16_SECTOR_SIZE;

            if (!read_sector(cluster_to_lba(source_cluster) + sector_index,
                             sector)) {
                (void)free_cluster_chain(*destination_first_cluster);
                *destination_first_cluster = 0;
                return FAT16_IO_ERROR;
            }
            if (!write_sector(cluster_to_lba(destination_cluster) +
                                  sector_index,
                              sector)) {
                (void)free_cluster_chain(*destination_first_cluster);
                *destination_first_cluster = 0;
                return FAT16_IO_ERROR;
            }
            remaining -= bytes_in_sector;
            bytes_in_cluster -= bytes_in_sector;
        }
        if (remaining != 0) {
            uint16_t next;
            result = get_fat_entry(source_cluster, &next);
            if (result != FAT16_OK) {
                (void)free_cluster_chain(*destination_first_cluster);
                *destination_first_cluster = 0;
                return result;
            }
            if (!valid_cluster(next)) {
                (void)free_cluster_chain(*destination_first_cluster);
                *destination_first_cluster = 0;
                return FAT16_CORRUPT;
            }
            source_cluster = next;
        }
    }
    return FAT16_OK;
}

static bool directories_equal(FAT16_DIRECTORY left, FAT16_DIRECTORY right) {
    return left.root == right.root && left.cluster == right.cluster;
}

static FAT16_RESULT delete_entry_set(FAT16_ENTRY_LOCATION short_location,
                                     FAT16_ENTRY_LOCATION *lfn_locations,
                                     uint8_t lfn_count) {
    FAT16_RESULT result = mark_entry_deleted(short_location);

    if (result != FAT16_OK) {
        return result;
    }
    for (uint8_t index = 0; index < lfn_count; ++index) {
        result = mark_entry_deleted(lfn_locations[index]);
        if (result != FAT16_OK) {
            return result;
        }
    }
    return flush_device();
}

extern "C" FAT16_RESULT fat16_copy_file(const char *source,
                                        const char *destination) {
    FAT16_DIRECTORY_STATE original;
    FAT16_DIRECTORY source_directory;
    FAT16_DIRECTORY destination_directory;
    char source_name[FAT16_MAX_NAME_LENGTH];
    char destination_name[FAT16_MAX_NAME_LENGTH];
    uint8_t destination_alias[11];
    uint8_t destination_lfn_count;
    uint8_t source_entry[32];
    uint8_t destination_entry[32];
    FAT16_ENTRY_LOCATION destination_location;
    bool destination_found;
    FAT16_ENTRY_LOCATION source_location;
    bool source_found;
    FAT16_RESULT result;
    uint32_t destination_first_cluster;

    // Save/restore cwd because resolving each path temporarily changes global state.
    if (source == NULL || destination == NULL) {
        return FAT16_INVALID_NAME;
    }
    save_directory_state(&original);
    result = resolve_parent_path(source, source_name);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    source_directory.root = current_directory_is_volume_root();
    source_directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(source_directory, source_name, source_entry,
                        &source_location, &source_found);
    if (result != FAT16_OK || !source_found) {
        restore_directory_state(&original);
        return result != FAT16_OK ? result : FAT16_NOT_FOUND;
    }
    if ((source_entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        restore_directory_state(&original);
        return FAT16_IS_DIRECTORY;
    }
    uint32_t source_first_cluster = read_u16(source_entry + 26);
    uint32_t source_size = read_u32(source_entry + 28);

    restore_directory_state(&original);
    result = resolve_parent_path(destination, destination_name);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    if (current_directory_is_read_only()) {
        restore_directory_state(&original);
        return FAT16_READ_ONLY;
    }
    destination_directory.root = current_directory_is_volume_root();
    destination_directory.cluster =
        current_directory_stack[current_directory_depth];
    result = find_entry(destination_directory, destination_name,
                        destination_entry, &destination_location,
                        &destination_found);
    if (result != FAT16_OK || destination_found) {
        restore_directory_state(&original);
        if (result != FAT16_OK) {
            return result;
        }
        return (destination_entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0
                   ? FAT16_IS_DIRECTORY
                   : FAT16_ALREADY_EXISTS;
    }
    result = make_entry_name(destination_directory, destination_name,
                             destination_alias, &destination_lfn_count);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    // Duplicate and flush data before creating the destination directory entry.
    result = duplicate_file_chain(source_first_cluster, source_size,
                                  &destination_first_cluster);
    if (result == FAT16_OK) {
        result = flush_device();
    }
    if (result == FAT16_OK) {
        result = write_named_entry(destination_directory, destination_name,
                                   source_entry[11], destination_first_cluster,
                                   source_size, NULL, NULL);
    }
    if (result == FAT16_OK) {
        result = flush_device();
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_move_file(const char *source,
                                        const char *destination) {
    FAT16_DIRECTORY_STATE original;
    FAT16_DIRECTORY source_directory;
    FAT16_DIRECTORY destination_directory;
    char source_name[FAT16_MAX_NAME_LENGTH];
    char destination_name[FAT16_MAX_NAME_LENGTH];
    uint8_t source_entry[32];
    uint8_t destination_entry[32];
    FAT16_ENTRY_LOCATION source_location;
    FAT16_ENTRY_LOCATION destination_location;
    FAT16_ENTRY_LOCATION source_lfn_locations[FAT16_MAX_LFN_ENTRIES];
    uint8_t source_lfn_count;
    bool source_found;
    bool destination_found;
    FAT16_RESULT result;

    // A move is metadata-only: publish a second name before deleting the first.
    if (source == NULL || destination == NULL) {
        return FAT16_INVALID_NAME;
    }
    save_directory_state(&original);
    result = resolve_parent_path(source, source_name);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    if (current_directory_is_read_only()) {
        restore_directory_state(&original);
        return FAT16_READ_ONLY;
    }
    source_directory.root = current_directory_is_volume_root();
    source_directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry_ex(source_directory, source_name, source_entry,
                           &source_location, &source_found,
                           source_lfn_locations, &source_lfn_count, NULL);
    if (result != FAT16_OK || !source_found) {
        restore_directory_state(&original);
        return result != FAT16_OK ? result : FAT16_NOT_FOUND;
    }
    if ((source_entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        restore_directory_state(&original);
        return FAT16_IS_DIRECTORY;
    }

    restore_directory_state(&original);
    result = resolve_parent_path(destination, destination_name);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    if (current_directory_is_read_only()) {
        restore_directory_state(&original);
        return FAT16_READ_ONLY;
    }
    destination_directory.root = current_directory_is_volume_root();
    destination_directory.cluster =
        current_directory_stack[current_directory_depth];
    result = find_entry(destination_directory, destination_name,
                        destination_entry, &destination_location,
                        &destination_found);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
        return result;
    }
    if (destination_found) {
        if ((destination_entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
            restore_directory_state(&original);
            return FAT16_IS_DIRECTORY;
        }
        if (directories_equal(source_directory, destination_directory) &&
            source_location.lba == destination_location.lba &&
            source_location.entry_index == destination_location.entry_index) {
            restore_directory_state(&original);
            return FAT16_OK;
        }
        restore_directory_state(&original);
        return FAT16_ALREADY_EXISTS;
    }

    // If source deletion fails, both names remain rather than losing the file.
    result = write_named_entry(destination_directory, destination_name,
                               source_entry[11], read_u16(source_entry + 26),
                               read_u32(source_entry + 28), NULL, NULL);
    if (result == FAT16_OK) {
        result = flush_device();
    }
    if (result == FAT16_OK) {
        result = delete_entry_set(source_location, source_lfn_locations,
                                  source_lfn_count);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_change_directory(const char *path) {
    FAT16_DIRECTORY_STATE original;
    FAT16_RESULT result;

    // A partial path failure must not strand the caller in an intermediate folder.
    save_directory_state(&original);
    result = change_directory_path_in_current(path);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
    }
    return result;
}

extern "C" FAT16_RESULT fat16_make_directory(const char *path) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    // Mutators resolve parent paths temporarily, then restore the caller's cwd.
    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = make_directory_in_current(basename);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_make_file(const char *path) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = make_file_in_current(basename);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_remove_file(const char *path) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = remove_file_in_current(basename);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_read_file(const char *path, char *buffer,
                                         uint32_t buffer_capacity,
                                         uint32_t *size) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    // All public path-based operations use the same parent-resolution convention.
    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = read_file_in_current(basename, buffer, buffer_capacity, size);
    } else if (size != NULL) {
        *size = 0;
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_write_file(const char *path,
                                          const char *buffer,
                                          uint32_t size) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = write_file_in_current(basename, buffer, size);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" FAT16_RESULT fat16_remove_directory(const char *path,
                                                bool recursive) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    FAT16_RESULT result;

    // The shell-visible root is a policy boundary, not a removable FAT directory.
    if (filesystem_virtual_root_active && path_is_virtual_root(path)) {
        return FAT16_ROOT_DIRECTORY;
    }
    save_directory_state(&original);
    result = resolve_parent_path(path, basename);
    if (result == FAT16_OK) {
        result = remove_directory_in_current(basename, recursive);
    }
    restore_directory_state(&original);
    return result;
}

extern "C" bool fat16_current_directory_is_read_only(void) {
    return current_directory_is_read_only();
}

extern "C" bool fat16_path_is_read_only(const char *path) {
    FAT16_DIRECTORY_STATE original;
    char basename[FAT16_MAX_NAME_LENGTH];
    bool read_only;

    save_directory_state(&original);
    FAT16_RESULT result = resolve_parent_path(path, basename);
    read_only = result == FAT16_OK && current_directory_is_read_only();
    restore_directory_state(&original);
    return read_only;
}
