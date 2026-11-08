#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

#define BOOT_INFO_MAGIC UINT64_C(0x445254424F4F5431)
#define BOOT_INFO_VERSION 1
#define BOOT_INFO_PIXEL_RGB 0
#define BOOT_INFO_PIXEL_BGR 1
#define BOOT_INFO_PIXEL_BITMASK 2

/* Channel masks are used only when GOP reports the bit-mask pixel format. */
typedef struct {
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
} BOOT_PIXEL_MASK;

/*
 * Versioned handoff from the UEFI loader to the kernel. Physical addresses
 * remain identity-mapped at this stage; the kernel owns the passed memory map.
 */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scan_line;
    uint32_t pixel_format;
    BOOT_PIXEL_MASK pixel_mask;
    uint64_t memory_map;
    uint64_t memory_map_size;
    uint64_t memory_descriptor_size;
    uint32_t memory_descriptor_version;
    uint32_t reserved;
    uint64_t acpi_root_table;
} BOOT_INFO;

#endif
