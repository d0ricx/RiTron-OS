#include "uefi.h"

#include "bootinfo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EFI_LOCATE_BY_PROTOCOL 2
#define EFI_LOADER_DATA 2
#define EFI_MEMORY_MAP_CAPACITY (1024 * 1024)
#define EFI_EXIT_BOOT_SERVICES_INDEX 26
#define EFI_GET_MEMORY_MAP_INDEX 4
#define EFI_ALLOCATE_POOL_INDEX 5
#define EFI_FREE_POOL_INDEX 6
#define EFI_HANDLE_PROTOCOL_INDEX 16
#define EFI_LOCATE_HANDLE_BUFFER_INDEX 36

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelFormat;
    BOOT_PIXEL_MASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    size_t SizeOfInfo;
    uint64_t FrameBufferBase;
    size_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Header;
    uint16_t *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    void *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    void *BootServices;
    size_t NumberOfTableEntries;
    void *ConfigurationTable;
};

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef EFI_STATUS(EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE, EFI_GUID *,
                                               void **);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    uint32_t, EFI_GUID *, void *, size_t *, EFI_HANDLE **);
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_POOL)(uint32_t, size_t, void **);
typedef EFI_STATUS(EFIAPI *EFI_FREE_POOL)(void *);
typedef EFI_STATUS(EFIAPI *EFI_GET_MEMORY_MAP)(
    size_t *, void *, size_t *, size_t *, uint32_t *);
typedef EFI_STATUS(EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, size_t);
static EFI_GUID GraphicsOutputGuid = {
    0x9042A9DE, 0x23DC, 0x4A38,
    {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
static EFI_GUID Acpi20TableGuid = {
    0x8868E871, 0xE4F1, 0x11D3,
    {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static EFI_GUID Acpi10TableGuid = {
    0xEB9D2D30, 0x2D88, 0x11D3,
    {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};
static BOOT_INFO BootInfo;

static bool guid_matches(const EFI_GUID *left, const EFI_GUID *right) {
    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3) {
        return false;
    }
    for (size_t index = 0; index < sizeof(left->Data4); ++index) {
        if (left->Data4[index] != right->Data4[index]) {
            return false;
        }
    }
    return true;
}

static void **boot_service_table(EFI_SYSTEM_TABLE *system_table) {
    return (void **)((uint8_t *)system_table->BootServices +
                     sizeof(EFI_TABLE_HEADER));
}

static EFI_STATUS locate_graphics_output(EFI_SYSTEM_TABLE *system_table,
                                         EFI_GRAPHICS_OUTPUT_PROTOCOL **gop) {
    EFI_HANDLE_PROTOCOL handle_protocol = (EFI_HANDLE_PROTOCOL)
        boot_service_table(system_table)[EFI_HANDLE_PROTOCOL_INDEX];
    EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer = (EFI_LOCATE_HANDLE_BUFFER)
        boot_service_table(system_table)[EFI_LOCATE_HANDLE_BUFFER_INDEX];
    EFI_FREE_POOL free_pool = (EFI_FREE_POOL)
        boot_service_table(system_table)[EFI_FREE_POOL_INDEX];
    EFI_HANDLE *handles = NULL;
    size_t handle_count = 0;
    EFI_STATUS status;

    *gop = NULL;
    if (system_table->ConsoleOutHandle != NULL) {
        status = handle_protocol(system_table->ConsoleOutHandle,
                                 &GraphicsOutputGuid, (void **)gop);
        if (!EFI_ERROR(status) && *gop != NULL) {
            return EFI_SUCCESS;
        }
    }

    status = locate_handle_buffer(EFI_LOCATE_BY_PROTOCOL,
                                  &GraphicsOutputGuid, NULL, &handle_count,
                                  &handles);
    if (EFI_ERROR(status)) {
        return status;
    }
    for (size_t index = 0; index < handle_count; ++index) {
        status = handle_protocol(handles[index], &GraphicsOutputGuid,
                                 (void **)gop);
        if (!EFI_ERROR(status) && *gop != NULL) {
            break;
        }
    }
    if (handles != NULL) {
        EFI_STATUS free_status = free_pool(handles);
        if (EFI_ERROR(free_status)) {
            return free_status;
        }
    }
    return *gop != NULL ? EFI_SUCCESS : EFI_ERROR_BIT | 2;
}

static uint64_t locate_acpi_root(EFI_SYSTEM_TABLE *system_table) {
    EFI_CONFIGURATION_TABLE *tables =
        (EFI_CONFIGURATION_TABLE *)system_table->ConfigurationTable;
    uint64_t acpi_root = 0;
    uint64_t fallback_root = 0;

    for (size_t index = 0; index < system_table->NumberOfTableEntries; ++index) {
        if (tables[index].VendorTable == NULL) {
            continue;
        }
        if (guid_matches(&tables[index].VendorGuid, &Acpi20TableGuid)) {
            acpi_root = (uint64_t)(uintptr_t)tables[index].VendorTable;
        } else if (guid_matches(&tables[index].VendorGuid,
                                &Acpi10TableGuid)) {
            fallback_root = (uint64_t)(uintptr_t)tables[index].VendorTable;
        }
    }
    return acpi_root != 0 ? acpi_root : fallback_root;
}

static EFI_STATUS prepare_boot_info(EFI_SYSTEM_TABLE *system_table) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS status = locate_graphics_output(system_table, &gop);

    if (EFI_ERROR(status) || gop->Mode == NULL || gop->Mode->Info == NULL ||
        gop->Mode->FrameBufferBase == 0) {
        return EFI_ERROR(status) ? status : EFI_ERROR_BIT | 2;
    }
    if (gop->Mode->Info->PixelFormat > BOOT_INFO_PIXEL_BITMASK) {
        return EFI_ERROR_BIT | 2;
    }

    BootInfo.magic = BOOT_INFO_MAGIC;
    BootInfo.version = BOOT_INFO_VERSION;
    BootInfo.structure_size = sizeof(BootInfo);
    BootInfo.framebuffer_base = gop->Mode->FrameBufferBase;
    BootInfo.framebuffer_size = gop->Mode->FrameBufferSize;
    BootInfo.width = gop->Mode->Info->HorizontalResolution;
    BootInfo.height = gop->Mode->Info->VerticalResolution;
    BootInfo.pixels_per_scan_line = gop->Mode->Info->PixelsPerScanLine;
    BootInfo.pixel_format = gop->Mode->Info->PixelFormat;
    BootInfo.pixel_mask.red_mask = gop->Mode->Info->PixelInformation.red_mask;
    BootInfo.pixel_mask.green_mask = gop->Mode->Info->PixelInformation.green_mask;
    BootInfo.pixel_mask.blue_mask = gop->Mode->Info->PixelInformation.blue_mask;
    BootInfo.pixel_mask.reserved_mask =
        gop->Mode->Info->PixelInformation.reserved_mask;
    BootInfo.acpi_root_table = locate_acpi_root(system_table);
    return EFI_SUCCESS;
}

static EFI_STATUS exit_boot_services(EFI_HANDLE image_handle,
                                     EFI_SYSTEM_TABLE *system_table) {
    void **services = boot_service_table(system_table);
    EFI_ALLOCATE_POOL allocate_pool =
        (EFI_ALLOCATE_POOL)services[EFI_ALLOCATE_POOL_INDEX];
    EFI_GET_MEMORY_MAP get_memory_map =
        (EFI_GET_MEMORY_MAP)services[EFI_GET_MEMORY_MAP_INDEX];
    EFI_EXIT_BOOT_SERVICES exit_services =
        (EFI_EXIT_BOOT_SERVICES)services[EFI_EXIT_BOOT_SERVICES_INDEX];
    void *memory_map = NULL;
    size_t allocated_size = EFI_MEMORY_MAP_CAPACITY;
    EFI_STATUS status =
        allocate_pool(EFI_LOADER_DATA, allocated_size, &memory_map);

    if (EFI_ERROR(status) || memory_map == NULL) {
        return EFI_ERROR(status) ? status : EFI_ERROR_BIT | 9;
    }
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        size_t memory_map_size = allocated_size;
        size_t map_key = 0;
        size_t descriptor_size = 0;
        uint32_t descriptor_version = 0;

        status = get_memory_map(&memory_map_size, memory_map, &map_key,
                                &descriptor_size, &descriptor_version);
        if (EFI_ERROR(status)) {
            return status;
        }
        BootInfo.memory_map = (uint64_t)(uintptr_t)memory_map;
        BootInfo.memory_map_size = memory_map_size;
        BootInfo.memory_descriptor_size = descriptor_size;
        BootInfo.memory_descriptor_version = descriptor_version;
        status = exit_services(image_handle, map_key);
        if (!EFI_ERROR(status)) {
            return EFI_SUCCESS;
        }
        if ((uint32_t)status != 2) {
            return status;
        }
    }
    return status;
}

extern "C" EFI_STATUS EFIAPI uefi_boot_kernel(
    EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    EFI_STATUS status;

    if (system_table == NULL || system_table->BootServices == NULL) {
        return EFI_ERROR_BIT | 2;
    }
    status = prepare_boot_info(system_table);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = exit_boot_services(image_handle, system_table);
    if (EFI_ERROR(status)) {
        return status;
    }
    (void)kernel_main(&BootInfo);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
