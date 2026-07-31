#ifndef __ELF_LOADER_H__
#define __ELF_LOADER_H__

#include <Uefi.h>
#include <IndustryStandard/Elf64.h>

/**
  Load an ELF64 kernel from a file handle.

  @param[in]   Root       EFI root directory
  @param[in]   FileName   Kernel ELF path (e.g. L"\\kernel.elf")
  @param[out]  EntryPoint Kernel entry address
  @param[out]  KernelBase Lowest loaded physical address

  @retval EFI_SUCCESS     Success
  @retval Others          Failure
**/
EFI_STATUS
LoadElf64(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *EntryPoint,
    OUT EFI_PHYSICAL_ADDRESS *KernelBase);

#endif