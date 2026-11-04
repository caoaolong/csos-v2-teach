/** @file
  CsosBoot Setup Utility public interface.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include "SetupNvData.h"

typedef CSOS_SETUP_CONFIGURATION  CSOS_BOOT_SETUP;

/**
  Load persisted setup configuration and enumerate GOP modes.

  Does not switch GOP mode or draw UI. Call BootGraphicsApplyMode() first,
  then CsosSetupUtilityBeginUi().

  @param[in] ImageHandle  EfiBoot image handle.

  @retval EFI_SUCCESS       Configuration loaded.
  @retval EFI_UNSUPPORTED   GOP is unavailable.
**/
EFI_STATUS
CsosSetupUtilityInit (
  IN EFI_HANDLE  ImageHandle
  );

/**
  Initialize the graphical UI on the current GOP mode.

  Must be called after BootGraphicsApplyMode().

  @retval EFI_SUCCESS       UI backend ready.
  @retval EFI_UNSUPPORTED   Graphics or font unavailable.
**/
EFI_STATUS
CsosSetupUtilityBeginUi (
  VOID
  );

/**
  Display the Setup Utility GUI and wait for user action.

  @param[out] BootRequested  TRUE when the user chooses to continue booting.

  @retval EFI_SUCCESS  Setup session finished.
**/
EFI_STATUS
CsosSetupUtilityRun (
  OUT BOOLEAN  *BootRequested
  );

/**
  Wait for boot timeout. F2 enters setup; timeout continues boot.

  @param[in]  TimeoutSeconds  Seconds to wait (0 skips waiting).
  @param[out] EnterSetup      TRUE when the user presses F2.

  @retval EFI_SUCCESS  Wait completed.
**/
EFI_STATUS
CsosSetupUtilityWaitForKey (
  IN  UINT16   TimeoutSeconds,
  OUT BOOLEAN  *EnterSetup
  );

/**
  Return the current setup configuration.

  @param[out] Config  Configuration buffer.
**/
VOID
CsosSetupUtilityGetConfig (
  OUT CSOS_BOOT_SETUP  *Config
  );

/**
  Release resources installed by CsosSetupUtilityInit()/BeginUi().
**/
VOID
CsosSetupUtilityFree (
  VOID
  );

/**
  Show a full-screen fatal boot error, then cold-reset the machine.

  Draws a dark error page (title + step + status code) when graphics is
  available, otherwise prints to ConOut. Waits up to 10 seconds for any
  key, then resets unconditionally. Intended for errors that make it
  impossible to continue booting (missing kernel, bad memory map, ...).

  @param[in] Detail   Short failed-step description, e.g. L"LOAD KERNEL.ELF".
  @param[in] Status   The failing EFI_STATUS.
**/
VOID
CsosShowFatalError (
  IN CONST CHAR16  *Detail,
  IN EFI_STATUS    Status
  );
