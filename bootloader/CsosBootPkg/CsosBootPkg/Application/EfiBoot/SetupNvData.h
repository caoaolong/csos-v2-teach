/** @file
  CsosBoot Setup configuration structure.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

#include <Guid/CsosBootSetup.h>

#define MAXSIZE_GOP_MODE_DESC      24
#define CSOS_SETUP_DEFAULT_TIMEOUT  5

#pragma pack(1)

typedef struct {
  UINT16    BootTimeout;
  UINT8     AutoBoot;
  UINT8     PreferEdid;
  UINT32    GopMode;
  UINT16    CurrentGopModeDesc[MAXSIZE_GOP_MODE_DESC];
} CSOS_SETUP_CONFIGURATION;

#pragma pack()
