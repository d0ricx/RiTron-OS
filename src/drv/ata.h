#ifndef ATA_PIO_H
#define ATA_PIO_H

#include <stdbool.h>
#include <stdint.h>

bool ata_pio_initialize(void);
uint32_t ata_pio_sector_count(void);
bool ata_pio_read_blocks(uint32_t lba, uint32_t block_count, void *buffer);
bool ata_pio_write_blocks(uint32_t lba, uint32_t block_count,
                          const void *buffer);
bool ata_pio_flush(void);

#endif
