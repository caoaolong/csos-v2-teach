/** @file
  Shared GOP mode selection for EfiBoot and Setup Utility.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/GraphicsOutput.h>

#include "BootGraphics.h"

STATIC
BOOLEAN
GetEdidPreferredResolution (
  OUT UINT32  *Width,
  OUT UINT32  *Height
  )
{
  EFI_STATUS                       Status;
  EFI_EDID_ACTIVE_PROTOCOL         *EdidActive;
  EFI_EDID_DISCOVERED_PROTOCOL     *EdidDiscovered;
  UINT8                            *Edid;
  UINT32                           Size;
  UINT32                           X;
  UINT32                           Y;

  Edid = NULL;
  Size = 0;

  Status = gBS->LocateProtocol (
                  &gEfiEdidActiveProtocolGuid,
                  NULL,
                  (VOID **)&EdidActive
                  );
  if (!EFI_ERROR (Status) &&
      (EdidActive->Edid != NULL) &&
      (EdidActive->SizeOfEdid >= 128))
  {
    Edid = EdidActive->Edid;
    Size = EdidActive->SizeOfEdid;
  } else {
    Status = gBS->LocateProtocol (
                    &gEfiEdidDiscoveredProtocolGuid,
                    NULL,
                    (VOID **)&EdidDiscovered
                    );
    if (!EFI_ERROR (Status) &&
        (EdidDiscovered->Edid != NULL) &&
        (EdidDiscovered->SizeOfEdid >= 128))
    {
      Edid = EdidDiscovered->Edid;
      Size = EdidDiscovered->SizeOfEdid;
    }
  }

  if ((Edid == NULL) || (Size < 128)) {
    return FALSE;
  }

  if ((Edid[0] != 0x00) || (Edid[1] != 0xFF)) {
    return FALSE;
  }

  if ((Edid[54] == 0x00) && (Edid[55] == 0x00)) {
    return FALSE;
  }

  X = (UINT32)Edid[56] | (((UINT32)(Edid[58] & 0xF0)) << 4);
  Y = (UINT32)Edid[59] | (((UINT32)(Edid[61] & 0xF0)) << 4);
  if ((X < 640) || (Y < 480)) {
    return FALSE;
  }

  *Width  = X;
  *Height = Y;
  return TRUE;
}

EFI_STATUS
BootGraphicsSelectMode (
  IN  CONST CSOS_SETUP_CONFIGURATION  *Setup,
  OUT UINT32                          *ModeIndex
  )
{
  EFI_STATUS                            Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo;
  UINTN                                 SizeOfInfo;
  UINT32                                Mode;
  UINT32                                BestMode;
  UINT32                                BestScore;
  UINT32                                Score;
  UINT32                                TargetW;
  UINT32                                TargetH;
  BOOLEAN                               HasTarget;

  if (ModeIndex == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (EFI_ERROR (Status) || (Gop->Mode == NULL)) {
    return EFI_UNSUPPORTED;
  }

  BestMode = Gop->Mode->Mode;
  if ((Setup != NULL) && (Setup->PreferEdid == 0)) {
    if (Setup->GopMode < Gop->Mode->MaxMode) {
      BestMode = Setup->GopMode;
    }

    *ModeIndex = BestMode;
    return EFI_SUCCESS;
  }

  HasTarget = GetEdidPreferredResolution (&TargetW, &TargetH);
  if (!HasTarget) {
    *ModeIndex = BestMode;
    return EFI_SUCCESS;
  }

  BestScore = 0xFFFFFFFFu;
  for (Mode = 0; Mode < Gop->Mode->MaxMode; Mode++) {
    Status = Gop->QueryMode (Gop, Mode, &SizeOfInfo, &ModeInfo);
    if (EFI_ERROR (Status) || (ModeInfo == NULL)) {
      continue;
    }

    if (ModeInfo->PixelFormat == PixelBltOnly) {
      FreePool (ModeInfo);
      continue;
    }

    Score = (ModeInfo->HorizontalResolution > TargetW
                 ? ModeInfo->HorizontalResolution - TargetW
                 : TargetW - ModeInfo->HorizontalResolution) +
            (ModeInfo->VerticalResolution > TargetH
                 ? ModeInfo->VerticalResolution - TargetH
                 : TargetH - ModeInfo->VerticalResolution);

    if (Score < BestScore) {
      BestScore = Score;
      BestMode  = Mode;
    }

    FreePool (ModeInfo);
  }

  *ModeIndex = BestMode;
  return EFI_SUCCESS;
}

EFI_STATUS
BootGraphicsApplyMode (
  IN CONST CSOS_SETUP_CONFIGURATION  *Setup
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINT32                        TargetMode;
  UINT32                        TargetW;
  UINT32                        TargetH;

  Status = BootGraphicsSelectMode (Setup, &TargetMode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (EFI_ERROR (Status) || (Gop->Mode == NULL)) {
    return EFI_UNSUPPORTED;
  }

  if (Gop->Mode->Mode == TargetMode) {
    return EFI_SUCCESS;
  }

  if ((Setup == NULL) || (Setup->PreferEdid != 0)) {
    if (GetEdidPreferredResolution (&TargetW, &TargetH)) {
      Print (L"EfiBoot: EDID preferred %ux%u, GOP mode %u\n", TargetW, TargetH, TargetMode);
    }
  } else {
    Print (L"EfiBoot: manual GOP mode %u\n", TargetMode);
  }

  Status = Gop->SetMode (Gop, TargetMode);
  if (EFI_ERROR (Status)) {
    Print (L"EfiBoot: SetMode(%u) failed: %r\n", TargetMode, Status);
  }

  return Status;
}
