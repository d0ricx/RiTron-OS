#ifndef KERNEL_MEMORY_H
#define KERNEL_MEMORY_H

#include "bootinfo.h"

#include <stddef.h>
#include <stdint.h>

bool memory_initialize(const BOOT_INFO *boot_info);
void *page_allocate(void);
void page_release(void *page);
void *kernel_allocate(size_t size);
void kernel_release(void *allocation);
size_t memory_free_page_count(void);

#endif
