/** @file
  CsosBoot Setup Utility public interface.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Uefi.h>
#include "SetupNvData.h"

typedef CSOS_SETUP_CONFIGURATION  CSOS_BOOT_SETUP;

/* Load persisted setup config and enumerate GOP modes. */
/* Call BootGraphicsApplyMode first, then BeginUi. */
/* @param ImageHandle EfiBoot image handle. */
/* @retval EFI_SUCCESS Config loaded. */
/* @retval EFI_UNSUPPORTED No GOP. */
EFI_STATUS
CsosSetupUtilityInit (
  IN EFI_HANDLE  ImageHandle
  );

/* Init graphical UI on the current GOP mode. */
/* Call after BootGraphicsApplyMode. */
/* @retval EFI_SUCCESS UI ready. */
/* @retval EFI_UNSUPPORTED No graphics or font. */
EFI_STATUS
CsosSetupUtilityBeginUi (
  VOID
  );

/* Show setup GUI and wait for user action. */
/* @param BootRequested TRUE if user chooses to boot. */
/* @retval EFI_SUCCESS Session finished. */
EFI_STATUS
CsosSetupUtilityRun (
  OUT BOOLEAN  *BootRequested
  );

/* Wait for boot timeout, F2 enters setup. */
/* @param TimeoutSeconds Seconds to wait, 0 skips. */
/* @param EnterSetup TRUE if user pressed F2. */
/* @retval EFI_SUCCESS Wait done. */
EFI_STATUS
CsosSetupUtilityWaitForKey (
  IN  UINT16   TimeoutSeconds,
  OUT BOOLEAN  *EnterSetup
  );

/* Return the current setup config. */
/* @param Config Config buffer. */
VOID
CsosSetupUtilityGetConfig (
  OUT CSOS_BOOT_SETUP  *Config
  );

/* Release resources from Init and BeginUi. */
VOID
CsosSetupUtilityFree (
  VOID
  );

/* Show fatal boot error, then cold-reset. */
/* Uses graphics if available, else ConOut. Waits 10s for a key. */
/* @param Detail Failed step, e.g. L"LOAD KERNEL.ELF". */
/* @param Status Failing EFI_STATUS. */
VOID
CsosShowFatalError (
  IN CONST CHAR16  *Detail,
  IN EFI_STATUS    Status
  );
