/** @file
  EDID base-block parser (pure C, no UEFI dependency).

  Used by EfiBoot to report the panel physical size for DPI scaling.
  Mode selection lives in Setup Utility (user choice, persisted).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  Parse an EDID blob and report the panel physical size.

  Bytes 0x15/0x16 are horizontal/vertical size in centimeters (0 =
  unknown, e.g. projectors); converted to millimeters here.

  @param[in]  Edid  EDID bytes (base block).
  @param[in]  Size  Byte count, must be >= 128.
  @param[out] Wmm   Physical width in mm.
  @param[out] Hmm   Physical height in mm.

  @retval 0   Parsed (both nonzero).
  @retval -1  No usable size (bad blob or unknown dimensions).
**/
int
EdidParsePhysicalSize (
  const uint8_t  *Edid,
  unsigned long  Size,
  uint32_t       *Wmm,
  uint32_t       *Hmm
  );

#ifdef __cplusplus
}
#endif
