/** @file
  Shared GOP mode selection for EfiBoot and Setup Utility.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include "SetupNvData.h"

/* Select and apply the GOP boot mode. */
/* Skips SetMode when the target is already active. */
/* @param Setup Boot setup options, may be NULL. */
/* @retval EFI_SUCCESS Mode active. */
/* @retval EFI_UNSUPPORTED No GOP. */
EFI_STATUS
BootGraphicsApplyMode (
  IN CONST CSOS_SETUP_CONFIGURATION  *Setup
  );

/* Return the GOP mode that ApplyMode would select. */
/* @param Setup Boot setup options. */
/* @param ModeIndex Selected mode index. */
/* @retval EFI_SUCCESS ModeIndex valid. */
EFI_STATUS
BootGraphicsSelectMode (
  IN  CONST CSOS_SETUP_CONFIGURATION  *Setup,
  OUT UINT32                          *ModeIndex
  );

/* Report panel physical size from EDID in mm, 0 means unknown. */
/* @param WidthMm Physical width in mm, 0 if unavailable. */
/* @param HeightMm Physical height in mm, 0 if unavailable. */
/* @retval EFI_SUCCESS Size reported, both nonzero. */
/* @retval EFI_NOT_FOUND No usable EDID or unknown size. */
/* @retval EFI_INVALID_PARAMETER NULL output pointer. */
EFI_STATUS
BootGraphicsEdidPhysicalSize (
  OUT UINT32  *WidthMm,
  OUT UINT32  *HeightMm
  );
