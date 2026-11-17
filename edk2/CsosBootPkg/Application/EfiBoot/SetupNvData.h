/** @file
  CsosBoot Setup configuration structure.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Guid/CsosBootSetup.h>

#define MAXSIZE_GOP_MODE_DESC      24
#define CSOS_SETUP_DEFAULT_TIMEOUT  5
#define CSOS_SETUP_MIN_TIMEOUT      3
#define CSOS_SETUP_MAX_TIMEOUT      60

#pragma pack(1)

typedef struct {
  UINT16    BootTimeout;
  UINT8     AutoBoot;
  UINT8     Reserved0;
  UINT32    GopMode;
  UINT16    CurrentGopModeDesc[MAXSIZE_GOP_MODE_DESC];
  /* Appended at the end: whether the user explicitly picked a mode in
   * Setup. Old NVRAM lacks this byte and reads back as 0 ("not set"),
   * in which case EfiBoot keeps the firmware's current mode. */
  UINT8     GopModeSet;
} CSOS_SETUP_CONFIGURATION;

#pragma pack()
