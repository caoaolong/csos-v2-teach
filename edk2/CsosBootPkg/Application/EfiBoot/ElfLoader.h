#ifndef __ELF_LOADER_H__
#define __ELF_LOADER_H__

#include <Uefi.h>
#include <IndustryStandard/Elf64.h>

/* Load ELF64 kernel from a file handle. */
/* @param Root EFI root directory. */
/* @param FileName Kernel ELF path, e.g. L"\\kernel.elf". */
/* @param EntryPoint Kernel entry address. */
/* @param KernelBase Lowest loaded physical address. */
/* @retval EFI_SUCCESS Success, others on failure. */
EFI_STATUS
LoadElf64(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *EntryPoint,
    OUT EFI_PHYSICAL_ADDRESS *KernelBase);

#endif