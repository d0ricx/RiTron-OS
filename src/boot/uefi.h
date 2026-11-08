#ifndef UEFI_BOOT_H
#define UEFI_BOOT_H

#include "console.h"

typedef void *EFI_HANDLE;
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

#ifdef __cplusplus
extern "C" {
#endif

EFI_STATUS EFIAPI uefi_boot_kernel(EFI_HANDLE image_handle,
                                  EFI_SYSTEM_TABLE *system_table);
EFI_STATUS EFIAPI kernel_main(const BOOT_INFO *boot_info);

#ifdef __cplusplus
}
#endif

#endif
