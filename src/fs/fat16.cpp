#include "fat16.h"

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

typedef struct {
    uint32_t cluster;
    bool root;
} FAT16_DIRECTORY;

typedef struct {
    uint32_t lba;
    uint8_t entry_index;
} FAT16_ENTRY_LOCATION;

typedef struct {
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
static char current_directory_names[FAT16_MAX_DIRECTORY_DEPTH][13];
static uint32_t current_directory_depth;
static bool filesystem_virtual_root_active;
static uint32_t filesystem_virtual_root_cluster;
static char filesystem_virtual_root_name[13];
static uint32_t filesystem_read_only_cluster;
static uint32_t filesystem_read_only_parent_cluster;
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
    return filesystem_data_start +
           (cluster - 2) * filesystem_sectors_per_cluster;
}

static bool read_sector(uint32_t lba, uint8_t *buffer) {
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

static FAT16_RESULT find_entry(FAT16_DIRECTORY directory,
                              const uint8_t name[11], uint8_t entry_out[32],
                              FAT16_ENTRY_LOCATION *location, bool *found) {
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
        if (is_usable_entry(entry) && bytes_equal(entry, name, 11)) {
            bytes_copy(entry_out, entry, 32);
            *location = entry_location;
            *found = true;
            return FAT16_OK;
        }
    }
}

static FAT16_RESULT find_free_location(FAT16_DIRECTORY directory,
                                       FAT16_ENTRY_LOCATION *location) {
    FAT16_DIRECTORY_CURSOR cursor;
    FAT16_ENTRY_LOCATION last_location = {0, 0};
    FAT16_RESULT result;

    cursor_initialize(&cursor, directory);
    for (;;) {
        uint8_t entry[32];
        bool has_entry;

        result = cursor_next(&cursor, entry, &last_location, &has_entry);
        if (result != FAT16_OK) {
            return result;
        }
        if (!has_entry || entry[0] == 0 || entry[0] == 0xE5) {
            if (has_entry) {
                *location = last_location;
                return FAT16_OK;
            }
            break;
        }
    }

    if (directory.root) {
        return FAT16_NO_SPACE;
    }

    uint32_t tail = directory.cluster;
    for (uint32_t count = 0; count < filesystem_cluster_count; ++count) {
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
                (void)set_fat_entry(new_cluster, 0);
                return result;
            }
            location->lba = cluster_to_lba(new_cluster);
            location->entry_index = 0;
            return FAT16_OK;
        }
        if (!valid_cluster(next)) {
            return FAT16_CORRUPT;
        }
        tail = next;
    }
    return FAT16_CORRUPT;
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
                                        const uint8_t name[11],
                                        uint8_t attributes,
                                        uint32_t first_cluster,
                                        uint32_t size = 0) {
    uint8_t existing[32];
    uint8_t new_entry[32];
    FAT16_ENTRY_LOCATION location;
    bool found;
    FAT16_RESULT result = find_entry(directory, name, existing, &location,
                                     &found);

    if (result != FAT16_OK) {
        return result;
    }
    if (found) {
        return FAT16_ALREADY_EXISTS;
    }
    result = find_free_location(directory, &location);
    if (result != FAT16_OK) {
        return result;
    }
    create_short_entry(new_entry, name, attributes, first_cluster, size);
    return write_entry(location, new_entry);
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
    uint8_t short_name[11];
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY parent;
    bool found;
    FAT16_RESULT result;

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
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(parent, short_name, entry, &location, &found);
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
    format_short_name(entry, current_directory_names[current_directory_depth]);
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
    absolute = path[0] == '/' || path[0] == '\\';
    if (absolute) {
        current_directory_depth = 0;
        current_directory_stack[0] =
            filesystem_virtual_root_active ? filesystem_virtual_root_cluster
                                           : FAT16_ROOT_CLUSTER;
    }
    position = path;
    while (*position != '\0') {
        char component[13];
        size_t length = 0;

        while (*position == '/' || *position == '\\') {
            ++position;
        }
        if (*position == '\0') {
            break;
        }
        while (*position != '\0' && *position != '/' && *position != '\\') {
            if (length == sizeof(component) - 1) {
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
    uint8_t short_name[11];
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY volume_root = {FAT16_ROOT_CLUSTER, true};
    FAT16_RESULT result;
    bool found;

    if (filesystem_virtual_root_active || current_directory_depth != 0) {
        return FAT16_CORRUPT;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    result = find_entry(volume_root, short_name, entry, &location, &found);
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
    format_short_name(entry, filesystem_virtual_root_name);
    filesystem_virtual_root_active = true;
    current_directory_stack[0] = filesystem_virtual_root_cluster;
    return FAT16_OK;
}

extern "C" FAT16_RESULT fat16_protect_directory(const char *name) {
    uint8_t short_name[11];
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;
    bool found;

    if (!filesystem_virtual_root_active) {
        return FAT16_CORRUPT;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(parent, short_name, entry, &location, &found);
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
    uint8_t short_name[11];
    uint32_t new_cluster;
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
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
    result = add_directory_entry(parent, short_name,
                                 FAT16_ATTRIBUTE_DIRECTORY, new_cluster);
    if (result != FAT16_OK) {
        (void)set_fat_entry(new_cluster, 0);
        return result;
    }
    return flush_device();
}

static FAT16_RESULT make_file_in_current(const char *name) {
    uint8_t short_name[11];
    FAT16_DIRECTORY parent;
    FAT16_RESULT result;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = add_directory_entry(parent, short_name, 0x20, 0);
    return result == FAT16_OK ? flush_device() : result;
}

static FAT16_RESULT read_file_in_current(const char *name, char *buffer,
                                         uint32_t buffer_capacity,
                                         uint32_t *size) {
    uint8_t short_name[11];
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
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(directory, short_name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return FAT16_NOT_DIRECTORY;
    }

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
    uint8_t short_name[11];
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
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(directory, short_name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (found) {
        if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
            return FAT16_NOT_DIRECTORY;
        }
        old_first_cluster = read_u16(entry + 26);
    } else {
        result = find_free_location(directory, &location);
        if (result != FAT16_OK) {
            return result;
        }
    }

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
                (void)set_fat_entry(new_cluster, 0);
                (void)free_cluster_chain(new_first_cluster);
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

    if (found) {
        write_u16(entry + 26, (uint16_t)new_first_cluster);
        write_u16(entry + 28, (uint16_t)size);
        write_u16(entry + 30, (uint16_t)(size >> 16));
        result = write_entry(location, entry);
    } else {
        uint8_t new_entry[32];
        create_short_entry(new_entry, short_name, 0x20, new_first_cluster,
                           size);
        result = write_entry(location, new_entry);
    }
    if (result != FAT16_OK) {
        if (new_first_cluster != 0) {
            (void)free_cluster_chain(new_first_cluster);
        }
        return result;
    }
    result = flush_device();
    if (result != FAT16_OK) {
        return result;
    }
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
    uint8_t short_name[11];
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY parent;
    uint32_t target_cluster;
    bool found;
    bool is_empty;
    FAT16_RESULT result;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    if (current_directory_depth == 0 &&
        (name == NULL || name[0] == '\0')) {
        return FAT16_ROOT_DIRECTORY;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    parent.root = current_directory_is_volume_root();
    parent.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(parent, short_name, entry, &location, &found);
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
    result = free_cluster_chain(target_cluster);
    if (result != FAT16_OK) {
        return result;
    }
    result = mark_entry_deleted(location);
    return result == FAT16_OK ? flush_device() : result;
}

extern "C" FAT16_RESULT fat16_list_directory(FAT16_LIST_ENTRY callback,
                                              void *context) {
    FAT16_DIRECTORY directory;
    FAT16_DIRECTORY_CURSOR cursor;

    if (callback == NULL) {
        return FAT16_CORRUPT;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
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
        if (!is_usable_entry(entry) || entry_is_dot(entry)) {
            continue;
        }
        char name[13];
        format_short_name(entry, name);
        callback(context, name,
                 (entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0,
                 read_u32(entry + 28));
    }
}

extern "C" bool fat16_get_path(char *buffer, size_t buffer_size) {
    size_t position = 0;

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
    char names[FAT16_MAX_DIRECTORY_DEPTH][13];
} FAT16_DIRECTORY_STATE;

static void save_directory_state(FAT16_DIRECTORY_STATE *state) {
    state->depth = current_directory_depth;
    bytes_copy(state->clusters, current_directory_stack,
               sizeof(current_directory_stack));
    bytes_copy(state->names, current_directory_names,
               sizeof(current_directory_names));
}

static void restore_directory_state(const FAT16_DIRECTORY_STATE *state) {
    current_directory_depth = state->depth;
    bytes_copy(current_directory_stack, state->clusters,
               sizeof(current_directory_stack));
    bytes_copy(current_directory_names, state->names,
               sizeof(current_directory_names));
}

static FAT16_RESULT resolve_parent_path(const char *path, char basename[13]) {
    const char *position;
    bool absolute;
    bool first_component = true;

    if (path == NULL || path[0] == '\0') {
        return FAT16_INVALID_NAME;
    }
    absolute = path[0] == '/' || path[0] == '\\';
    if (absolute) {
        current_directory_depth = 0;
        current_directory_stack[0] =
            filesystem_virtual_root_active ? filesystem_virtual_root_cluster
                                           : FAT16_ROOT_CLUSTER;
    }
    position = path;
    while (*position != '\0') {
        char component[13];
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
            if (length == sizeof(component) - 1) {
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
    char component[13];
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
        if (length == sizeof(component) - 1) {
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
    uint8_t short_name[11];
    uint8_t entry[32];
    FAT16_ENTRY_LOCATION location;
    FAT16_DIRECTORY directory;
    FAT16_RESULT result;
    bool found;
    uint32_t first_cluster;

    if (current_directory_is_read_only()) {
        return FAT16_READ_ONLY;
    }
    result = make_short_name(name, short_name);
    if (result != FAT16_OK) {
        return result;
    }
    directory.root = current_directory_is_volume_root();
    directory.cluster = current_directory_stack[current_directory_depth];
    result = find_entry(directory, short_name, entry, &location, &found);
    if (result != FAT16_OK) {
        return result;
    }
    if (!found) {
        return FAT16_NOT_FOUND;
    }
    if ((entry[11] & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return FAT16_IS_DIRECTORY;
    }
    first_cluster = read_u16(entry + 26);
    if (first_cluster != 0) {
        result = free_cluster_chain(first_cluster);
        if (result != FAT16_OK) {
            return result;
        }
    }
    result = mark_entry_deleted(location);
    return result == FAT16_OK ? flush_device() : result;
}

extern "C" FAT16_RESULT fat16_change_directory(const char *path) {
    FAT16_DIRECTORY_STATE original;
    FAT16_RESULT result;

    save_directory_state(&original);
    result = change_directory_path_in_current(path);
    if (result != FAT16_OK) {
        restore_directory_state(&original);
    }
    return result;
}

extern "C" FAT16_RESULT fat16_make_directory(const char *path) {
    FAT16_DIRECTORY_STATE original;
    char basename[13];
    FAT16_RESULT result;

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
    char basename[13];
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
    char basename[13];
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
    char basename[13];
    FAT16_RESULT result;

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
    char basename[13];
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
    char basename[13];
    FAT16_RESULT result;

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
    char basename[13];
    bool read_only;

    save_directory_state(&original);
    FAT16_RESULT result = resolve_parent_path(path, basename);
    read_only = result == FAT16_OK && current_directory_is_read_only();
    restore_directory_state(&original);
    return read_only;
}
