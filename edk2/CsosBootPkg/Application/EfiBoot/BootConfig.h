#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

/* Load csos.cfg from the ESP root and keep it for the kernel. */
/* Missing file is not fatal, the caller continues with defaults. */
/* @param Root Opened ESP volume root. */
/* @param FileName Config path, e.g. L"\\csos.cfg". */
/* @param ConfigAddr Kept copy address, 0 if absent. */
/* @param ConfigSize Kept copy size, 0 if absent. */
/* @retval EFI_SUCCESS Loaded and kept. */
/* @retval EFI_NOT_FOUND File missing. */
/* @retval Others Read or alloc failure. */
EFI_STATUS
LoadBootConfig(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *ConfigAddr,
    OUT UINTN *ConfigSize);

#endif
