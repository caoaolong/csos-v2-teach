#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/GraphicsOutput.h>

STATIC
CONST CHAR16 *
PixelFormatToString (
  IN EFI_GRAPHICS_PIXEL_FORMAT  Format
  )
{
  switch (Format) {
    case PixelRedGreenBlueReserved8BitPerColor:
      return L"RGBX8";
    case PixelBlueGreenRedReserved8BitPerColor:
      return L"BGRX8";
    case PixelBitMask:
      return L"BitMask";
    case PixelBltOnly:
      return L"BltOnly";
    default:
      return L"Unknown";
  }
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                            Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
  UINTN                                 SizeOfInfo;
  UINT32                                Mode;
  UINT32                                CurrentMode;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (EFI_ERROR (Status)) {
    Print (L"GopMode: Locate GOP failed: %r\n", Status);
    return Status;
  }

  if ((Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    Print (L"GopMode: GOP Mode info unavailable\n");
    return EFI_UNSUPPORTED;
  }

  CurrentMode = Gop->Mode->Mode;
  Print (L"GopMode: MaxMode=%u CurrentMode=%u\n", Gop->Mode->MaxMode, CurrentMode);
  Print (
    L"GopMode: Current %ux%u Format=%s PPSL=%u FB=0x%lx Size=0x%x\n",
    Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,
    PixelFormatToString (Gop->Mode->Info->PixelFormat),
    Gop->Mode->Info->PixelsPerScanLine,
    Gop->Mode->FrameBufferBase,
    Gop->Mode->FrameBufferSize
    );
  Print (L"\n");
  Print (L"Idx  Resolution      Format   PPSL     Note\n");
  Print (L"---- --------------- -------- -------- --------\n");

  for (Mode = 0; Mode < Gop->Mode->MaxMode; Mode++) {
    Info       = NULL;
    SizeOfInfo = 0;
    Status     = Gop->QueryMode (Gop, Mode, &SizeOfInfo, &Info);
    if (EFI_ERROR (Status) || (Info == NULL)) {
      Print (L"%4u  <QueryMode failed: %r>\n", Mode, Status);
      continue;
    }

    Print (
      L"%4u  %4ux%-10u %-8s %8u%s\n",
      Mode,
      Info->HorizontalResolution,
      Info->VerticalResolution,
      PixelFormatToString (Info->PixelFormat),
      Info->PixelsPerScanLine,
      (Mode == CurrentMode) ? L" *" : L""
      );

    FreePool (Info);
  }

  Print (L"\nGopMode: done (* = current mode)\n");
  return EFI_SUCCESS;
}
