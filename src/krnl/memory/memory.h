#ifndef KERNEL_MEMORY_H
#define KERNEL_MEMORY_H

#include "bootinfo.h"

#include <stddef.h>
#include <stdint.h>

/* Initialize the page bitmap from conventional UEFI memory-map entries. */
bool memory_initialize(const BOOT_INFO *boot_info);
/* Allocate/release one page-aligned physical page from the managed range. */
void *page_allocate(void);
void page_release(void *page);
/* Allocate/release heap storage; allocations are aligned to 16 bytes. */
void *kernel_allocate(size_t size);
void kernel_release(void *allocation);
/* Number of currently free pages in the bitmap-managed physical range. */
size_t memory_free_page_count(void);

#endif
