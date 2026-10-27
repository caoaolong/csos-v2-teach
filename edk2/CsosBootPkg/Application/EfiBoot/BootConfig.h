#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

/**
  Load csos.cfg from the ESP root and keep it for the kernel.

  The file content is copied into EfiLoaderData pages (never reclaimed by
  the kernel's PMM, which only frees Conventional/BootServices memory),
  so the kernel can parse it after ExitBootServices. Success is quiet;
  the kernel prints the effective config (boot_config_summary).

  Missing file is NOT fatal: *ConfigAddr is set to 0, *ConfigSize to 0,
  and EFI_NOT_FOUND is returned; the caller continues with defaults.

  @param[in]  Root        Opened ESP volume root.
  @param[in]  FileName    Config path (e.g. L"\\csos.cfg").
  @param[out] ConfigAddr  Physical address of the kept copy (0 if absent).
  @param[out] ConfigSize  Bytes in the kept copy (0 if absent).

  @retval EFI_SUCCESS   Loaded, printed, and kept.
  @retval EFI_NOT_FOUND File does not exist.
  @retval Others        Read/alloc failure.
**/
EFI_STATUS
LoadBootConfig(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *ConfigAddr,
    OUT UINTN *ConfigSize);

#endif
