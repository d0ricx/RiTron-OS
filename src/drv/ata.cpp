#include "ata.h"

#include "console.h"

#define ATA_PRIMARY_IO 0x1F0
#define ATA_SECONDARY_IO 0x170
#define ATA_PRIMARY_CONTROL 0x3F6
#define ATA_SECONDARY_CONTROL 0x376
#define ATA_STATUS_ERROR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DEVICE_FAULT 0x20
#define ATA_STATUS_BUSY 0x80
#define ATA_COMMAND_IDENTIFY 0xEC
#define ATA_COMMAND_READ_SECTORS 0x20
#define ATA_COMMAND_WRITE_SECTORS 0x30
#define ATA_COMMAND_CACHE_FLUSH 0xE7
#define ATA_LBA28_MAX 0x0FFFFFFF
#define ATA_SECTOR_SIZE 512
#define ATA_POLL_LIMIT 1000000
#define DATA_DISK_VOLUME_ID UINT32_C(0x44525446)

typedef struct {
    uint16_t io_base;
    uint16_t control_base;
    uint8_t device;
    uint32_t sector_count;
    bool present;
} ATA_DEVICE;

static ATA_DEVICE DataDisk;
static uint8_t SectorBuffer[ATA_SECTOR_SIZE] __attribute__((aligned(4096)));

static void port_out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t port_in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void port_out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint16_t port_in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_delay(uint16_t control_base) {
    (void)port_in8(control_base);
    (void)port_in8(control_base);
    (void)port_in8(control_base);
    (void)port_in8(control_base);
}

static bool wait_not_busy(uint16_t io_base, uint16_t control_base,
                          uint8_t *status_out) {
    for (uint32_t count = 0; count < ATA_POLL_LIMIT; ++count) {
        uint8_t status = port_in8(io_base + 7);
        if (status == 0x00 || status == 0xFF) {
            return false;
        }
        if ((status & ATA_STATUS_BUSY) == 0) {
            if (status_out != NULL) {
                *status_out = status;
            }
            return true;
        }
        io_delay(control_base);
    }
    return false;
}

static bool wait_data_request(const ATA_DEVICE *device) {
    uint8_t status;

    if (!wait_not_busy(device->io_base, device->control_base, &status)) {
        return false;
    }
    return (status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) == 0 &&
           (status & ATA_STATUS_DRQ) != 0;
}

static bool wait_command_complete(const ATA_DEVICE *device) {
    uint8_t status;

    return wait_not_busy(device->io_base, device->control_base, &status) &&
           (status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) == 0;
}

static bool identify_device(uint16_t io_base, uint16_t control_base,
                           uint8_t device, uint32_t *sector_count) {
    uint16_t identify_data[256];
    uint8_t status;

    port_out8(control_base, 0x02);
    port_out8(io_base + 6, (uint8_t)(0xA0 | (device << 4)));
    io_delay(control_base);
    port_out8(io_base + 2, 0);
    port_out8(io_base + 3, 0);
    port_out8(io_base + 4, 0);
    port_out8(io_base + 5, 0);
    port_out8(io_base + 7, ATA_COMMAND_IDENTIFY);
    status = port_in8(io_base + 7);
    if (status == 0 || status == 0xFF) {
        return false;
    }
    if (!wait_not_busy(io_base, control_base, &status) ||
        (status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) != 0 ||
        (status & ATA_STATUS_DRQ) == 0) {
        return false;
    }
    for (uint32_t index = 0; index < 256; ++index) {
        identify_data[index] = port_in16(io_base);
    }
    *sector_count =
        (uint32_t)identify_data[60] | ((uint32_t)identify_data[61] << 16);
    return *sector_count != 0;
}

static bool read_sector(const ATA_DEVICE *device, uint32_t lba,
                        uint8_t *buffer) {
    if (lba > ATA_LBA28_MAX || lba >= device->sector_count) {
        return false;
    }
    port_out8(device->io_base + 6,
              (uint8_t)(0xE0 | (device->device << 4) |
                        ((lba >> 24) & 0x0F)));
    io_delay(device->control_base);
    port_out8(device->io_base + 1, 0);
    port_out8(device->io_base + 2, 1);
    port_out8(device->io_base + 3, (uint8_t)lba);
    port_out8(device->io_base + 4, (uint8_t)(lba >> 8));
    port_out8(device->io_base + 5, (uint8_t)(lba >> 16));
    port_out8(device->io_base + 7, ATA_COMMAND_READ_SECTORS);
    if (!wait_data_request(device)) {
        return false;
    }
    for (uint32_t index = 0; index < ATA_SECTOR_SIZE / 2; ++index) {
        uint16_t value = port_in16(device->io_base);
        buffer[index * 2] = (uint8_t)value;
        buffer[index * 2 + 1] = (uint8_t)(value >> 8);
    }
    return wait_command_complete(device);
}

static bool write_sector(const ATA_DEVICE *device, uint32_t lba,
                         const uint8_t *buffer) {
    if (lba > ATA_LBA28_MAX || lba >= device->sector_count) {
        return false;
    }
    port_out8(device->io_base + 6,
              (uint8_t)(0xE0 | (device->device << 4) |
                        ((lba >> 24) & 0x0F)));
    io_delay(device->control_base);
    port_out8(device->io_base + 1, 0);
    port_out8(device->io_base + 2, 1);
    port_out8(device->io_base + 3, (uint8_t)lba);
    port_out8(device->io_base + 4, (uint8_t)(lba >> 8));
    port_out8(device->io_base + 5, (uint8_t)(lba >> 16));
    port_out8(device->io_base + 7, ATA_COMMAND_WRITE_SECTORS);
    if (!wait_data_request(device)) {
        return false;
    }
    for (uint32_t index = 0; index < ATA_SECTOR_SIZE / 2; ++index) {
        uint16_t value =
            (uint16_t)buffer[index * 2] |
            (uint16_t)((uint16_t)buffer[index * 2 + 1] << 8);
        port_out16(device->io_base, value);
    }
    return wait_command_complete(device);
}

static bool is_data_disk_boot_sector(const uint8_t *sector) {
    uint32_t volume_id = (uint32_t)sector[39] |
                         ((uint32_t)sector[40] << 8) |
                         ((uint32_t)sector[41] << 16) |
                         ((uint32_t)sector[42] << 24);

    if (sector[510] != 0x55 || sector[511] != 0xAA ||
        sector[11] != 0x00 || sector[12] != 0x02 ||
        sector[43] != 'K' || sector[44] != 'E' || sector[45] != 'R' ||
        sector[46] != 'N' || sector[47] != 'E' || sector[48] != 'L' ||
        sector[49] != 'D' || sector[50] != 'A' || sector[51] != 'T' ||
        sector[52] != 'A' || sector[53] != ' ' ||
        sector[54] != 'F' || sector[55] != 'A' || sector[56] != 'T' ||
        sector[57] != '1' || sector[58] != '6' || sector[59] != ' ' ||
        sector[60] != ' ' || sector[61] != ' ' ||
        volume_id != DATA_DISK_VOLUME_ID) {
        return false;
    }
    return true;
}

bool ata_pio_initialize(void) {
    static const uint16_t io_bases[2] = {ATA_PRIMARY_IO, ATA_SECONDARY_IO};
    static const uint16_t control_bases[2] = {ATA_PRIMARY_CONTROL,
                                              ATA_SECONDARY_CONTROL};

    DataDisk.present = false;
    for (uint8_t channel = 0; channel < 2; ++channel) {
        for (uint8_t device = 0; device < 2; ++device) {
            uint32_t sector_count;
            ATA_DEVICE candidate;

            if (!identify_device(io_bases[channel], control_bases[channel],
                                 device, &sector_count)) {
                continue;
            }
            candidate.io_base = io_bases[channel];
            candidate.control_base = control_bases[channel];
            candidate.device = device;
            candidate.sector_count = sector_count;
            candidate.present = true;
            if (read_sector(&candidate, 0, SectorBuffer) &&
                is_data_disk_boot_sector(SectorBuffer)) {
                DataDisk = candidate;
                serial_write_string("[kernel] ATA PIO data disk initialized\n");
                return true;
            }
        }
    }
    serial_write_string("[kernel] ERROR: ATA PIO data disk not found\n");
    return false;
}

bool ata_pio_read_blocks(uint32_t lba, uint32_t block_count, void *buffer) {
    uint8_t *destination = static_cast<uint8_t *>(buffer);

    if (!DataDisk.present || (buffer == NULL && block_count != 0) ||
        (uint64_t)lba + block_count > DataDisk.sector_count) {
        return false;
    }
    for (uint32_t index = 0; index < block_count; ++index) {
        if (!read_sector(&DataDisk, lba + index,
                         destination + (size_t)index * ATA_SECTOR_SIZE)) {
            return false;
        }
    }
    return true;
}

uint32_t ata_pio_sector_count(void) {
    return DataDisk.present ? DataDisk.sector_count : 0;
}

bool ata_pio_write_blocks(uint32_t lba, uint32_t block_count,
                          const void *buffer) {
    const uint8_t *source = static_cast<const uint8_t *>(buffer);

    if (!DataDisk.present || (buffer == NULL && block_count != 0) ||
        (uint64_t)lba + block_count > DataDisk.sector_count) {
        return false;
    }
    for (uint32_t index = 0; index < block_count; ++index) {
        if (!write_sector(&DataDisk, lba + index,
                          source + (size_t)index * ATA_SECTOR_SIZE)) {
            return false;
        }
    }
    return true;
}

bool ata_pio_flush(void) {
    if (!DataDisk.present) {
        return false;
    }
    port_out8(DataDisk.io_base + 6,
              (uint8_t)(0xE0 | (DataDisk.device << 4)));
    io_delay(DataDisk.control_base);
    port_out8(DataDisk.io_base + 7, ATA_COMMAND_CACHE_FLUSH);
    if (!wait_command_complete(&DataDisk)) {
        return false;
    }
    return true;
}
