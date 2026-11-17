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
  /* Set when the user picked a mode in Setup. */
  /* Old NVRAM reads 0, EfiBoot keeps the firmware mode. */
  UINT8     GopModeSet;
} CSOS_SETUP_CONFIGURATION;

#pragma pack()
