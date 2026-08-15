#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include "ElfLoader.h"

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

    // reserved for future extension
    UINT64 rsdp;
} BootInfo;

BootInfo *BootInfoPtr;

typedef VOID (*KERNEL_ENTRY)(BootInfo *);

/**
  Read preferred timing from EDID (first Detailed Timing Descriptor).
  Returns TRUE if a usable target resolution was found.
**/
static BOOLEAN
GetEdidPreferredResolution(
    OUT UINT32 *Width,
    OUT UINT32 *Height)
{
    EFI_STATUS Status;
    EFI_EDID_ACTIVE_PROTOCOL *EdidActive;
    EFI_EDID_DISCOVERED_PROTOCOL *EdidDiscovered;
    UINT8 *Edid;
    UINT32 Size;
    UINT32 X;
    UINT32 Y;

    Edid = NULL;
    Size = 0;

    Status = gBS->LocateProtocol(
        &gEfiEdidActiveProtocolGuid,
        NULL,
        (VOID **)&EdidActive);
    if (!EFI_ERROR(Status) &&
        (EdidActive->Edid != NULL) &&
        (EdidActive->SizeOfEdid >= 128))
    {
        Edid = EdidActive->Edid;
        Size = EdidActive->SizeOfEdid;
    }
    else
    {
        Status = gBS->LocateProtocol(
            &gEfiEdidDiscoveredProtocolGuid,
            NULL,
            (VOID **)&EdidDiscovered);
        if (!EFI_ERROR(Status) &&
            (EdidDiscovered->Edid != NULL) &&
            (EdidDiscovered->SizeOfEdid >= 128))
        {
            Edid = EdidDiscovered->Edid;
            Size = EdidDiscovered->SizeOfEdid;
        }
    }

    if ((Edid == NULL) || (Size < 128))
    {
        return FALSE;
    }

    /* EDID header magic: 00 FF ... */
    if ((Edid[0] != 0x00) || (Edid[1] != 0xFF))
    {
        return FALSE;
    }

    /* First DTD starts at offset 54; 00 00 means not a timing block */
    if ((Edid[54] == 0x00) && (Edid[55] == 0x00))
    {
        return FALSE;
    }

    X = (UINT32)Edid[56] | (((UINT32)(Edid[58] & 0xF0)) << 4);
    Y = (UINT32)Edid[59] | (((UINT32)(Edid[61] & 0xF0)) << 4);
    if ((X < 640) || (Y < 480))
    {
        return FALSE;
    }

    *Width = X;
    *Height = Y;
    return TRUE;
}

static EFI_STATUS
InitGraphics(
    OUT BootInfo *Info)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ModeInfo;
    UINTN SizeOfInfo;
    UINT32 Mode;
    UINT32 m;
    UINT32 *Fb;
    UINTN PixelCount;
    UINTN i;
    UINT32 BestMode;
    UINT32 BestScore;
    UINT32 Score;
    UINT32 TargetW;
    UINT32 TargetH;
    BOOLEAN HasTarget;

    Status = gBS->LocateProtocol(
        &gEfiGraphicsOutputProtocolGuid,
        NULL,
        (VOID **)&Gop);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: Locate GOP failed: %r\n", Status);
        return Status;
    }

    /*
      Prefer GOP mode closest to display preferred timing (EDID).
      If EDID is unavailable, keep the current mode.
    */
    BestMode = Gop->Mode->Mode;
    HasTarget = GetEdidPreferredResolution(&TargetW, &TargetH);
    if (HasTarget)
    {
        Print(L"EfiBoot: EDID preferred %ux%u\n", TargetW, TargetH);

        BestScore = 0xFFFFFFFFu;
        for (m = 0; m < Gop->Mode->MaxMode; m++)
        {
            Status = Gop->QueryMode(Gop, m, &SizeOfInfo, &ModeInfo);
            if (EFI_ERROR(Status) || (ModeInfo == NULL))
            {
                continue;
            }

            /* Need a linear framebuffer for later FB fill */
            if (ModeInfo->PixelFormat == PixelBltOnly)
            {
                FreePool(ModeInfo);
                continue;
            }

            Score = (ModeInfo->HorizontalResolution > TargetW
                         ? ModeInfo->HorizontalResolution - TargetW
                         : TargetW - ModeInfo->HorizontalResolution) +
                    (ModeInfo->VerticalResolution > TargetH
                         ? ModeInfo->VerticalResolution - TargetH
                         : TargetH - ModeInfo->VerticalResolution);

            if (Score < BestScore)
            {
                BestScore = Score;
                BestMode = m;
            }

            FreePool(ModeInfo);
        }
    }
    else
    {
        Print(L"EfiBoot: no EDID preferred timing, keep mode %u\n", BestMode);
    }

    Mode = BestMode;
    Status = Gop->SetMode(Gop, Mode);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: SetMode(%u) failed: %r\n", Mode, Status);
        return Status;
    }

    Info->framebuffer_base = Gop->Mode->FrameBufferBase;
    Info->framebuffer_width = Gop->Mode->Info->HorizontalResolution;
    Info->framebuffer_height = Gop->Mode->Info->VerticalResolution;
    Info->framebuffer_pixels_per_scanline = Gop->Mode->Info->PixelsPerScanLine;
    Info->framebuffer_pixel_format = (UINT32)Gop->Mode->Info->PixelFormat;

    /* Fill as 32bpp; bit-mask modes are treated as 4 bytes/pixel */
    if (Gop->Mode->Info->PixelFormat == PixelBltOnly)
    {
        Print(L"EfiBoot: PixelBltOnly, skip FB fill\n");
        return EFI_UNSUPPORTED;
    }

    Fb = (UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
    PixelCount = (UINTN)Info->framebuffer_pixels_per_scanline *
                 (UINTN)Info->framebuffer_height;
    for (i = 0; i < PixelCount; i++)
    {
        Fb[i] = CSOS_BG_COLOR;
    }

    /* Last step: clear only, no Print (ConOut would draw on the FB) */
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

    Status = InitGraphics(BootInfoPtr);
    if (EFI_ERROR(Status))
    {
        Print(L"EfiBoot: InitGraphics failed: %r (continue without FB)\n", Status);
        BootInfoPtr->framebuffer_base = 0;
    }
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