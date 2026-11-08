#include "uefi.h"

// Firmware entry-point names used by different UEFI loaders. Both converge
// on the same handoff so the boot image has one initialization path.
extern "C" EFI_STATUS EFIAPI bootloader_main(EFI_HANDLE image_handle,
                                              EFI_SYSTEM_TABLE *system_table) {
    return uefi_boot_kernel(image_handle, system_table);
}

extern "C" EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle,
                                      EFI_SYSTEM_TABLE *system_table) {
    return uefi_boot_kernel(image_handle, system_table);
}
