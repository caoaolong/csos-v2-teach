#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "ElfLoader.h"

#define CSOS_BOOT_INFO_ADDR 0x40000ULL
#define CSOS_BOOT_INFO_MAGIC 0xC505B007ULL

typedef struct
{
    UINT64 magic;
    // UEFI Memory Map
    UINT64 memory_map;      // physical address of EFI_MEMORY_DESCRIPTOR*
    UINT64 memory_map_size; // size in bytes
    UINT64 descriptor_size; // size of each descriptor
    UINT32 descriptor_version;

    // reserved for future extension
    UINT64 rsdp;
} BootInfo;

BootInfo *BootInfoPtr;

typedef VOID (*KERNEL_ENTRY)(BootInfo *);

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
    EFI_PHYSICAL_ADDRESS BootInfoPhys = CSOS_BOOT_INFO_ADDR;
    Status = gBS->AllocatePages(
        AllocateAddress,
        EfiLoaderData,
        1,
        &BootInfoPhys);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: AllocatePages(boot_info @ 0x%lx) failed: %r\n",
              CSOS_BOOT_INFO_ADDR, Status);
        return Status;
    }

    BootInfoPtr = (BootInfo *)(UINTN)BootInfoPhys;
    ZeroMem(BootInfoPtr, sizeof(BootInfo));
    BootInfoPtr->magic = CSOS_BOOT_INFO_MAGIC;

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

        BootInfoPtr->memory_map = (UINT64)(UINTN)MemMap;
        BootInfoPtr->memory_map_size = (UINT64)MemMapSize;
        BootInfoPtr->descriptor_size = (UINT64)DescriptorSize;
        BootInfoPtr->descriptor_version = DescriptorVersion;

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
    ((KERNEL_ENTRY)(UINTN)EntryPoint)(BootInfoPtr);

    return EFI_SUCCESS;
}