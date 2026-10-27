#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/SimpleFileSystem.h>
#include "BootConfig.h"

#define BOOT_CONFIG_MAX_SIZE 16384

EFI_STATUS
LoadBootConfig(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *ConfigAddr,
    OUT UINTN *ConfigSize)
{
    EFI_FILE_PROTOCOL *File;
    EFI_STATUS Status;
    CHAR8 *Buf;
    UINTN Total;
    UINTN Chunk;
    UINTN Pages;
    EFI_PHYSICAL_ADDRESS KeptAddr;

    if (Root == NULL || FileName == NULL || ConfigAddr == NULL || ConfigSize == NULL)
    {
        return EFI_INVALID_PARAMETER;
    }

    *ConfigAddr = 0;
    *ConfigSize = 0;

    Status = Root->Open(Root, &File, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: csos.cfg not found (%r), using defaults\n", Status);
        return EFI_NOT_FOUND;
    }

    Buf = AllocatePool(BOOT_CONFIG_MAX_SIZE + 1);
    if (Buf == NULL)
    {
        File->Close(File);
        return EFI_OUT_OF_RESOURCES;
    }

    Total = 0;
    Status = File->SetPosition(File, 0);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: csos.cfg SetPosition failed: %r\n", Status);
        goto Done;
    }

    while (Total < BOOT_CONFIG_MAX_SIZE)
    {
        Chunk = BOOT_CONFIG_MAX_SIZE - Total;
        Status = File->Read(File, &Chunk, Buf + Total);
        if (EFI_ERROR(Status))
        {
            Print(L"EfiBoot: csos.cfg Read failed: %r\n", Status);
            goto Done;
        }
        if (Chunk == 0)
        {
            break; // EOF
        }
        Total += Chunk;
    }
    Buf[Total] = '\0';
    Status = EFI_SUCCESS;

    if (Total == 0)
    {
        goto Done;
    }

    Pages = EFI_SIZE_TO_PAGES(Total);
    KeptAddr = 0;
    Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, Pages, &KeptAddr);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: csos.cfg keep failed: %r (continue without config)\n", Status);
        goto Done;
    }

    CopyMem((VOID *)(UINTN)KeptAddr, Buf, Total);
    *ConfigAddr = KeptAddr;
    *ConfigSize = Total;
    Status = EFI_SUCCESS;

Done:
    FreePool(Buf);
    File->Close(File);
    return Status;
}
