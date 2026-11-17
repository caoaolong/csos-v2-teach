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

/* Parse EDID blob, report panel size in mm. */
/* Zero means unknown size. */
/* @param Edid EDID bytes, base block. */
/* @param Size Byte count, at least 128. */
/* @param Wmm Physical width in mm. */
/* @param Hmm Physical height in mm. */
/* @retval 0 Parsed, both nonzero. */
/* @retval -1 No usable size. */
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
