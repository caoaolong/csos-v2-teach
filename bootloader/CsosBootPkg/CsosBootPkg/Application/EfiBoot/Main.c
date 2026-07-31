#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "ElfLoader.h"

typedef VOID (*KERNEL_ENTRY)(VOID);

EFI_STATUS
EFIAPI
UefiMain(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    EFI_FILE_PROTOCOL *Root;
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS EntryPoint;
    EFI_PHYSICAL_ADDRESS KernelBase;
    UINTN MapKey;
    EFI_MEMORY_DESCRIPTOR *MemMap;
    UINTN MemMapSize;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;
    UINTN Retry;

    (VOID) SystemTable;

    Print(L"EfiBoot: starting\n");

    //
    // Open filesystem from the volume that loaded this image
    //
    Status = gBS->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&LoadedImage);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: LoadedImage failed: %r\n", Status);
        return Status;
    }

    Status = gBS->HandleProtocol(
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&Fs);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: SimpleFileSystem failed: %r\n", Status);
        return Status;
    }

    Status = Fs->OpenVolume(Fs, &Root);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: OpenVolume failed: %r\n", Status);
        return Status;
    }

    Status = LoadElf64(Root, L"\\kernel.elf", &EntryPoint, &KernelBase);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: LoadElf64 failed: %r\n", Status);
        return Status;
    }

    Print(L"EfiBoot: kernel loaded at 0x%lx, entry 0x%lx\n", KernelBase, EntryPoint);

    (VOID) KernelBase;

    //
    // ExitBootServices & jump to kernel (retry if map key changes)
    //
    for (Retry = 0; Retry < 5; Retry++)
    {
        MemMap = NULL;
        MemMapSize = 0;
        Status = gBS->GetMemoryMap(
            &MemMapSize,
            MemMap,
            &MapKey,
            &DescriptorSize,
            &DescriptorVersion);
        if (Status != EFI_BUFFER_TOO_SMALL)
        {
            Print(L"EfiBoot: GetMemoryMap size failed: %r\n", Status);
            return Status;
        }

        //
        // Extra descriptors: map may grow between GetMemoryMap and ExitBootServices
        //
        MemMapSize += SIZE_4KB;
        MemMap = AllocatePool(MemMapSize);
        if (MemMap == NULL)
        {
            return EFI_OUT_OF_RESOURCES;
        }

        Status = gBS->GetMemoryMap(
            &MemMapSize,
            MemMap,
            &MapKey,
            &DescriptorSize,
            &DescriptorVersion);
        if (EFI_ERROR(Status))
        {
            FreePool(MemMap);
            Print(L"EfiBoot: GetMemoryMap failed: %r\n", Status);
            return Status;
        }

        Status = gBS->ExitBootServices(ImageHandle, MapKey);
        if (!EFI_ERROR(Status))
        {
            break;
        }

        FreePool(MemMap);
    }

    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: ExitBootServices failed: %r\n", Status);
        return Status;
    }

    // 跳转进入内核
    ((KERNEL_ENTRY)(UINTN)EntryPoint)();

    return EFI_SUCCESS;
}