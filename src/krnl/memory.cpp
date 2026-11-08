#include "memory.h"

#include <stdbool.h>

#define PAGE_SIZE 4096
#define MEMORY_BITMAP_BITS 1048576
#define MEMORY_BITMAP_BYTES (MEMORY_BITMAP_BITS / 8)
#define MINIMUM_MANAGED_ADDRESS 0x100000
#define UEFI_MEMORY_CONVENTIONAL 7
#define HEAP_ALIGNMENT 16

typedef struct {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attributes;
} UEFI_MEMORY_DESCRIPTOR;

typedef struct HEAP_BLOCK {
    size_t size;
    bool free;
    struct HEAP_BLOCK *next;
    struct HEAP_BLOCK *previous;
} HEAP_BLOCK;

static uint8_t PageBitmap[MEMORY_BITMAP_BYTES] __attribute__((aligned(16)));
static size_t FreePages;
static HEAP_BLOCK *HeapHead;
static bool MemoryReady;

static bool page_is_free(size_t page_index) {
    return (PageBitmap[page_index >> 3] & (1u << (page_index & 7))) != 0;
}

static void set_page_free(size_t page_index, bool is_free);

static void *allocate_contiguous_pages(size_t count) {
    size_t run_start = 0;
    size_t run_length = 0;

    if (!MemoryReady || count == 0 || count > FreePages) {
        return NULL;
    }
    for (size_t page_index = MINIMUM_MANAGED_ADDRESS / PAGE_SIZE;
         page_index < MEMORY_BITMAP_BITS; ++page_index) {
        if (page_is_free(page_index)) {
            if (run_length == 0) {
                run_start = page_index;
            }
            if (++run_length == count) {
                for (size_t index = 0; index < count; ++index) {
                    set_page_free(run_start + index, false);
                }
                FreePages -= count;
                return (void *)(uintptr_t)(run_start * PAGE_SIZE);
            }
        } else {
            run_length = 0;
        }
    }
    return NULL;
}

static void set_page_free(size_t page_index, bool is_free) {
    uint8_t mask = (uint8_t)(1u << (page_index & 7));
    if (is_free) {
        PageBitmap[page_index >> 3] |= mask;
    } else {
        PageBitmap[page_index >> 3] &= (uint8_t)~mask;
    }
}

bool memory_initialize(const BOOT_INFO *boot_info) {
    const uint8_t *map;
    uint64_t descriptor_count;

    if (boot_info == NULL || boot_info->memory_map == 0 ||
        boot_info->memory_map_size == 0 ||
        boot_info->memory_descriptor_size < sizeof(UEFI_MEMORY_DESCRIPTOR) ||
        boot_info->memory_descriptor_version == 0 ||
        boot_info->memory_map_size % boot_info->memory_descriptor_size != 0) {
        return false;
    }

    for (size_t index = 0; index < MEMORY_BITMAP_BYTES; ++index) {
        PageBitmap[index] = 0;
    }
    FreePages = 0;
    HeapHead = NULL;
    MemoryReady = false;
    map = (const uint8_t *)(uintptr_t)boot_info->memory_map;
    descriptor_count =
        boot_info->memory_map_size / boot_info->memory_descriptor_size;

    for (uint64_t descriptor_index = 0; descriptor_index < descriptor_count;
         ++descriptor_index) {
        const UEFI_MEMORY_DESCRIPTOR *descriptor =
            (const UEFI_MEMORY_DESCRIPTOR *)(map +
                descriptor_index * boot_info->memory_descriptor_size);
        uint64_t start = descriptor->physical_start;
        uint64_t end;

        if (descriptor->type != UEFI_MEMORY_CONVENTIONAL ||
            descriptor->number_of_pages == 0 ||
            descriptor->number_of_pages > UINT64_MAX / PAGE_SIZE ||
            start > UINT64_MAX -
                        descriptor->number_of_pages * PAGE_SIZE) {
            continue;
        }
        end = start + descriptor->number_of_pages * PAGE_SIZE;
        if (start < MINIMUM_MANAGED_ADDRESS) {
            start = MINIMUM_MANAGED_ADDRESS;
        }
        if (end > (uint64_t)MEMORY_BITMAP_BITS * PAGE_SIZE) {
            end = (uint64_t)MEMORY_BITMAP_BITS * PAGE_SIZE;
        }
        start = (start + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        end &= ~(uint64_t)(PAGE_SIZE - 1);
        for (uint64_t address = start; address < end; address += PAGE_SIZE) {
            size_t page_index = (size_t)(address / PAGE_SIZE);
            if (!page_is_free(page_index)) {
                set_page_free(page_index, true);
                ++FreePages;
            }
        }
    }

    if (FreePages == 0) {
        return false;
    }
    MemoryReady = true;
    return true;
}

void *page_allocate(void) {
    return allocate_contiguous_pages(1);
}

void page_release(void *page) {
    uintptr_t address = (uintptr_t)page;
    size_t page_index;

    if (!MemoryReady || page == NULL || (address & (PAGE_SIZE - 1)) != 0 ||
        address < MINIMUM_MANAGED_ADDRESS ||
        address >= (uint64_t)MEMORY_BITMAP_BITS * PAGE_SIZE) {
        return;
    }
    page_index = address / PAGE_SIZE;
    if (!page_is_free(page_index)) {
        set_page_free(page_index, true);
        ++FreePages;
    }
}

static size_t align_heap_size(size_t size) {
    return (size + HEAP_ALIGNMENT - 1) & ~(size_t)(HEAP_ALIGNMENT - 1);
}

static bool expand_heap(size_t minimum_size) {
    size_t pages_needed;
    size_t total_bytes;
    void *first_page;

    if (minimum_size > SIZE_MAX - sizeof(HEAP_BLOCK)) {
        return false;
    }
    pages_needed =
        (minimum_size + sizeof(HEAP_BLOCK) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages_needed > SIZE_MAX / PAGE_SIZE) {
        return false;
    }
    total_bytes = pages_needed * PAGE_SIZE;
    first_page = allocate_contiguous_pages(pages_needed);
    if (first_page == NULL) {
        return false;
    }

    HEAP_BLOCK *block = (HEAP_BLOCK *)first_page;
    block->size = total_bytes - sizeof(HEAP_BLOCK);
    block->free = true;
    block->previous = NULL;
    block->next = HeapHead;
    if (HeapHead != NULL) {
        HeapHead->previous = block;
    }
    HeapHead = block;
    return true;
}

void *kernel_allocate(size_t size) {
    HEAP_BLOCK *block;
    size_t required;

    if (!MemoryReady || size == 0 || size > SIZE_MAX - HEAP_ALIGNMENT) {
        return NULL;
    }
    required = align_heap_size(size);
    for (;;) {
        for (block = HeapHead; block != NULL; block = block->next) {
            if (!block->free || block->size < required) {
                continue;
            }
            if (block->size >= required + sizeof(HEAP_BLOCK) + HEAP_ALIGNMENT) {
                    HEAP_BLOCK *remainder =
                    (HEAP_BLOCK *)((uint8_t *)(block + 1) + required);
                remainder->size =
                    block->size - required - sizeof(HEAP_BLOCK);
                remainder->free = true;
                remainder->previous = block;
                remainder->next = block->next;
                if (remainder->next != NULL) {
                    remainder->next->previous = remainder;
                }
                block->next = remainder;
                block->size = required;
            }
            block->free = false;
            return block + 1;
        }
        if (!expand_heap(required)) {
            return NULL;
        }
    }
}

void kernel_release(void *allocation) {
    if (!MemoryReady || allocation == NULL) {
        return;
    }
    HEAP_BLOCK *block = (HEAP_BLOCK *)allocation - 1;
    if (block->free) {
        return;
    }
    block->free = true;
    if (block->next != NULL && block->next->free &&
        (uint8_t *)(block + 1) + block->size ==
            (uint8_t *)block->next) {
        HEAP_BLOCK *next = block->next;
        block->size += sizeof(HEAP_BLOCK) + next->size;
        block->next = next->next;
        if (block->next != NULL) {
            block->next->previous = block;
        }
    }
    if (block->previous != NULL && block->previous->free &&
        (uint8_t *)(block->previous + 1) + block->previous->size ==
            (uint8_t *)block) {
        HEAP_BLOCK *previous = block->previous;
        previous->size += sizeof(HEAP_BLOCK) + block->size;
        previous->next = block->next;
        if (previous->next != NULL) {
            previous->next->previous = previous;
        }
    }
}

size_t memory_free_page_count(void) {
    return FreePages;
}
