/** @file
  EDID base-block parser (pure C, no UEFI dependency).

  Used by EfiBoot to report the panel physical size for DPI scaling.
  Mode selection lives in Setup Utility (user choice, persisted).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "EdidParse.h"

#define EDID_BASE_SIZE  128
#define EDID_PHYS_W_OFF 0x15
#define EDID_PHYS_H_OFF 0x16

static const uint8_t  mEdidMagic[8] = {
  0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

int
EdidParsePhysicalSize (
  const uint8_t  *Edid,
  unsigned long  Size,
  uint32_t       *Wmm,
  uint32_t       *Hmm
  )
{
  unsigned long  i;
  unsigned int   Sum;

  if ((Edid == NULL) || (Wmm == NULL) || (Hmm == NULL)) {
    return -1;
  }
  if (Size < EDID_BASE_SIZE) {
    return -1;
  }
  for (i = 0; i < 8; i++) {
    if (Edid[i] != mEdidMagic[i]) {
      return -1;
    }
  }
  Sum = 0;
  for (i = 0; i < EDID_BASE_SIZE; i++) {
    Sum += Edid[i];
  }
  if ((Sum & 0xFFu) != 0) {
    return -1;
  }
  /* Centimeters -> millimeters; 0 means unknown (projector/aspect-only). */
  if ((Edid[EDID_PHYS_W_OFF] == 0) || (Edid[EDID_PHYS_H_OFF] == 0)) {
    return -1;
  }
  *Wmm = (uint32_t)Edid[EDID_PHYS_W_OFF] * 10u;
  *Hmm = (uint32_t)Edid[EDID_PHYS_H_OFF] * 10u;
  return 0;
}
