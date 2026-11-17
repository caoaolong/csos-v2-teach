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
#include "EdidParse.h"

STATIC
BOOLEAN
GetEdidBlob (
  OUT UINT8   **Edid,
  OUT UINT32  *Size
  );

EFI_STATUS
BootGraphicsEdidPhysicalSize (
  OUT UINT32  *WidthMm,
  OUT UINT32  *HeightMm
  )
{
  UINT8   *Edid;
  UINT32  Size;
  uint32_t Wmm;
  uint32_t Hmm;

  if ((WidthMm == NULL) || (HeightMm == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (!GetEdidBlob (&Edid, &Size)) {
    return EFI_NOT_FOUND;
  }
  if (EdidParsePhysicalSize (Edid, Size, &Wmm, &Hmm) != 0) {
    return EFI_NOT_FOUND;
  }
  *WidthMm  = (UINT32)Wmm;
  *HeightMm = (UINT32)Hmm;
  return EFI_SUCCESS;
}

/**
  Locate the EDID blob (Active preferred, Discovered as fallback).
  Shared by the physical-size query.
**/
STATIC
BOOLEAN
GetEdidBlob (
  OUT UINT8   **Edid,
  OUT UINT32  *Size
  )
{
  EFI_STATUS                       Status;
  EFI_EDID_ACTIVE_PROTOCOL         *EdidActive;
  EFI_EDID_DISCOVERED_PROTOCOL     *EdidDiscovered;

  *Edid = NULL;
  *Size = 0;

  Status = gBS->LocateProtocol (
                  &gEfiEdidActiveProtocolGuid,
                  NULL,
                  (VOID **)&EdidActive
                  );
  if (!EFI_ERROR (Status) &&
      (EdidActive->Edid != NULL) &&
      (EdidActive->SizeOfEdid >= 128))
  {
    *Edid = EdidActive->Edid;
    *Size = EdidActive->SizeOfEdid;
    return TRUE;
  }

  Status = gBS->LocateProtocol (
                  &gEfiEdidDiscoveredProtocolGuid,
                  NULL,
                  (VOID **)&EdidDiscovered
                  );
  if (!EFI_ERROR (Status) &&
      (EdidDiscovered->Edid != NULL) &&
      (EdidDiscovered->SizeOfEdid >= 128))
  {
    *Edid = EdidDiscovered->Edid;
    *Size = EdidDiscovered->SizeOfEdid;
    return TRUE;
  }

  return FALSE;
}

EFI_STATUS
BootGraphicsSelectMode (
  IN  CONST CSOS_SETUP_CONFIGURATION  *Setup,
  OUT UINT32                          *ModeIndex
  )
{
  EFI_STATUS                   Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  UINT32                       Mode;

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

  /*
   * No auto-selection here: the user picks a mode in Setup Utility and
   * the choice persists in NVRAM (GopModeSet). Without a stored choice,
   * keep whatever mode the firmware booted with.
   */
  Mode = Gop->Mode->Mode;
  if ((Setup != NULL) && (Setup->GopModeSet != 0) &&
      (Setup->GopMode < Gop->Mode->MaxMode)) {
    Mode = Setup->GopMode;
  }

  Print (L"EfiBoot: GOP mode %u%s\n",
         Mode, (Mode == Gop->Mode->Mode) ? L" (current)" : L" (stored)");
  *ModeIndex = Mode;
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

  Status = Gop->SetMode (Gop, TargetMode);
  if (EFI_ERROR (Status)) {
    Print (L"EfiBoot: SetMode(%u) failed: %r\n", TargetMode, Status);
  }

  return Status;
}
