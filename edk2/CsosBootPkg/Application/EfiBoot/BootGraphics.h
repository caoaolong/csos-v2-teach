/** @file
  Shared GOP mode selection for EfiBoot and Setup Utility.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include "SetupNvData.h"

/**
  Select and apply the GOP mode for boot based on setup configuration.
  Skips SetMode when the target mode is already active.

  @param[in] Setup  Boot setup options (may be NULL for EDID-preferred defaults).

  @retval EFI_SUCCESS       Mode is active (unchanged or switched).
  @retval EFI_UNSUPPORTED   GOP is unavailable.
**/
EFI_STATUS
BootGraphicsApplyMode (
  IN CONST CSOS_SETUP_CONFIGURATION  *Setup
  );

/**
  Return the GOP mode index that BootGraphicsApplyMode() would select.

  @param[in]  Setup     Boot setup options.
  @param[out] ModeIndex Selected mode index.

  @retval EFI_SUCCESS  ModeIndex is valid.
**/
EFI_STATUS
BootGraphicsSelectMode (
  IN  CONST CSOS_SETUP_CONFIGURATION  *Setup,
  OUT UINT32                          *ModeIndex
  );
