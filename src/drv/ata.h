#ifndef ATA_PIO_H
#define ATA_PIO_H

#include <stdbool.h>
#include <stdint.h>

/* Locate the project's FAT16 disk by volume signature, not by device order. */
bool ata_pio_initialize(void);
uint32_t ata_pio_sector_count(void);
/* Transfer 512-byte sectors using bounded legacy ATA LBA28 PIO operations. */
bool ata_pio_read_blocks(uint32_t lba, uint32_t block_count, void *buffer);
bool ata_pio_write_blocks(uint32_t lba, uint32_t block_count,
                          const void *buffer);
/* Wait for the device's write cache to report a completed flush. */
bool ata_pio_flush(void);

#endif
