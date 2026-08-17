#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include "BootGraphics.h"
#include "ElfLoader.h"
#include "SetupUtility.h"

#define CSOS_BOOT_INFO_ADDR 0x40000ULL
#define CSOS_BOOT_INFO_MAGIC 0xC505B007ULL

/* Background #181818 (same for RGB/BGR when R=G=B) */
#define CSOS_BG_COLOR 0x00181818U

typedef struct
{
    UINT64 magic;
    UINT64 framebuffer_base;
    UINT32 framebuffer_width;
    UINT32 framebuffer_height;
    UINT32 framebuffer_pixels_per_scanline;
    UINT32 framebuffer_pixel_format; /* EFI_GRAPHICS_PIXEL_FORMAT */

    // UEFI Memory Map
    UINT64 memory_map;      // physical address of EFI_MEMORY_DESCRIPTOR*
    UINT64 memory_map_size; // size in bytes
    UINT64 descriptor_size; // size of each descriptor
    UINT32 descriptor_version;

    UINT64 rsdp;
} BootInfo;

typedef VOID (*KERNEL_ENTRY)(BootInfo *);

static EFI_STATUS
InitGraphics(
    OUT BootInfo *Info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
    EFI_STATUS Status;

    Status = gBS->LocateProtocol(
        &gEfiGraphicsOutputProtocolGuid,
        NULL,
        (VOID **)&Gop);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: Locate GOP failed: %r\n", Status);
        return Status;
    }

    if ((Gop->Mode == NULL) || (Gop->Mode->Info == NULL))
    {
        Print(L"EfiBoot: GOP mode info unavailable\n");
        return EFI_UNSUPPORTED;
    }

    Info->framebuffer_base = Gop->Mode->FrameBufferBase;
    Info->framebuffer_width = Gop->Mode->Info->HorizontalResolution;
    Info->framebuffer_height = Gop->Mode->Info->VerticalResolution;
    Info->framebuffer_pixels_per_scanline = Gop->Mode->Info->PixelsPerScanLine;
    Info->framebuffer_pixel_format = (UINT32)Gop->Mode->Info->PixelFormat;

    if (Gop->Mode->Info->PixelFormat == PixelBltOnly)
    {
        return EFI_UNSUPPORTED;
    }

    //
    // Setup Utility already drew the UI; only record BootInfo here.
    // Writing stride*height pixels can exceed FrameBufferSize and corrupt UEFI memory.
    //

    return EFI_SUCCESS;
}

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
    EFI_PHYSICAL_ADDRESS BootInfoPhys;
    UINTN MapKey;
    EFI_MEMORY_DESCRIPTOR *MemMap;
    UINTN MemMapSize;
    UINTN DescriptorSize;
    UINT32 DescriptorVersion;
    UINTN Retry;
    BootInfo *BootInfoPtr;
    CSOS_BOOT_SETUP SetupConfig;
    BOOLEAN EnterSetup;
    BOOLEAN BootRequested;
    BOOLEAN SetupInitialized;

    (VOID) SystemTable;

    ZeroMem(&SetupConfig, sizeof(SetupConfig));
    SetupConfig.PreferEdid = 1;

    SetupInitialized = FALSE;
    Status = CsosSetupUtilityInit(ImageHandle);
    if (!EFI_ERROR(Status))
    {
        SetupInitialized = TRUE;
        CsosSetupUtilityGetConfig(&SetupConfig);
    }
    else
    {
        Print(L"EfiBoot: Setup Utility init failed: %r (continue without GUI)\n", Status);
    }

    Status = BootGraphicsApplyMode(&SetupConfig);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: graphics mode setup failed: %r\n", Status);
    }

    if (SetupInitialized)
    {
        Status = CsosSetupUtilityBeginUi();
        if (EFI_ERROR(Status))
        {
            Print(L"EfiBoot: Setup Utility UI unavailable: %r (continue without GUI)\n", Status);
            SetupInitialized = FALSE;
        }
        else
        {
            EnterSetup = FALSE;
            BootRequested = FALSE;
            if (SetupConfig.AutoBoot != 0)
            {
                Print(L"EfiBoot: press F2 for Setup, auto boot in %u sec\n",
                      SetupConfig.BootTimeout);
                CsosSetupUtilityWaitForKey(SetupConfig.BootTimeout, &EnterSetup);
            }
            else
            {
                EnterSetup = TRUE;
            }

            while (EnterSetup && !BootRequested)
            {
                Status = CsosSetupUtilityRun(&BootRequested);
                if (EFI_ERROR(Status))
                {
                    Print(L"EfiBoot: Setup Utility failed: %r\n", Status);
                    break;
                }

                if (!BootRequested)
                {
                    CsosSetupUtilityWaitForKey(SetupConfig.BootTimeout, &EnterSetup);
                }
            }

            CsosSetupUtilityGetConfig(&SetupConfig);
            BootGraphicsApplyMode(&SetupConfig);
        }
    }

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

    (VOID) KernelBase;

    BootInfoPhys = CSOS_BOOT_INFO_ADDR;
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

    Status = InitGraphics(BootInfoPtr);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: InitGraphics failed: %r (continue without FB)\n", Status);
        BootInfoPtr->framebuffer_base = 0;
    }

    if (SetupInitialized)
    {
        CsosSetupUtilityFree();
        SetupInitialized = FALSE;
    }

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

    ((KERNEL_ENTRY)(UINTN)EntryPoint)(BootInfoPtr);

    return EFI_SUCCESS;
}
