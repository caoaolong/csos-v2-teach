/** @file
  CsosBoot modern graphical Setup Utility (GOP framebuffer UI).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/CsosBootSetup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiFont.h>
#include <Protocol/SimpleTextIn.h>

#include "SetupNvData.h"
#include "SetupUtility.h"
#include <GfxLayout.h>

#pragma warning (push)
#pragma warning (disable : 4819)
#include <LOGO.h>
#pragma warning (pop)

#define CSOS_SETUP_VARIABLE_NAME  L"CsosBootSetup"

#define STR_SUBTITLE        L"Boot Configuration"
#define STR_BOOT_TIMEOUT    L"Boot Timeout (sec)"
#define STR_AUTO_BOOT       L"Auto Boot"
#define STR_PREFER_EDID     L"Prefer EDID Resolution"
#define STR_GOP_MODE        L"GOP Display Mode"
#define STR_BTN_SAVE_BOOT   L"Save & Boot"
#define STR_BTN_BOOT        L"Boot Now"
#define STR_BTN_BACK        L"Back"
#define STR_HELP            L"Up/Down Select   Left/Right Change   Enter OK   Esc Back"
#define STR_BOOT_WAIT_HINT  L"Enter F2 to BIOS or wait to automatic boot."

#define GFX_FONT_SIZE       19

typedef struct {
  UINT32    X;
  UINT32    Y;
} GOP_MODE;

typedef enum {
  FocusBootTimeout = 0,
  FocusAutoBoot,
  FocusPreferEdid,
  FocusGopMode,
  FocusActionSaveBoot,
  FocusActionBoot,
  FocusActionBack,
  FocusMax
} SETUP_FOCUS;

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_HII_FONT_PROTOCOL         *mHiiFont;
STATIC CSOS_BOOT_SETUP               mSetupConfig;
STATIC UINTN                         mNumGopModes;
STATIC GOP_MODE                      *mGopModes;
STATIC BOOLEAN                       mGfxReady;
STATIC SETUP_FOCUS                   mFocus;

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrBg;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrSurface;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrBorder;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrAccent;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrAccentDim;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrText;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrMuted;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrDisabled;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrFocus;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrBtn;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mClrBtnHover;

STATIC
VOID
GfxFillRect (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Color
  );

STATIC
VOID
GfxDrawText (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Fg,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Bg,
  IN CONST CHAR16                     *Text
  );

STATIC
VOID
GfxDrawTextCentered (
  IN UINTN                            Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Fg,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Bg,
  IN CONST CHAR16                     *Text
  );

#define RGB(R, G, B)  { (R), (G), (B), 0 }

STATIC
VOID
InitPalette (
  VOID
  )
{
  mClrBg        = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (15, 15, 18);
  mClrSurface   = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (30, 30, 36);
  mClrBorder    = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (48, 48, 58);
  mClrAccent    = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (59, 130, 246);
  mClrAccentDim = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (37, 78, 138);
  mClrText      = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (244, 244, 245);
  mClrMuted     = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (161, 161, 170);
  mClrDisabled  = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (82, 82, 91);
  mClrFocus     = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (39, 39, 42);
  mClrBtn       = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (39, 39, 46);
  mClrBtnHover  = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)RGB (63, 63, 70);
}

STATIC
VOID
SetupLoadDefaults (
  OUT CSOS_BOOT_SETUP  *Config
  )
{
  ZeroMem (Config, sizeof (*Config));
  Config->BootTimeout = CSOS_SETUP_DEFAULT_TIMEOUT;
  Config->AutoBoot    = 1;
  Config->PreferEdid  = 1;
  Config->GopMode     = 0;
  UnicodeSPrint (
    Config->CurrentGopModeDesc,
    sizeof (Config->CurrentGopModeDesc),
    L"Unset"
    );
}

STATIC
EFI_STATUS
SetupLoadFromVariable (
  OUT CSOS_BOOT_SETUP  *Config
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  SetupLoadDefaults (Config);

  Size   = sizeof (*Config);
  Status = gRT->GetVariable (
                  CSOS_SETUP_VARIABLE_NAME,
                  &gCsosBootSetupFormsetGuid,
                  NULL,
                  &Size,
                  Config
                  );
  if (EFI_ERROR (Status)) {
    SetupLoadDefaults (Config);
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetupSaveToVariable (
  IN CONST CSOS_BOOT_SETUP  *Config
  )
{
  return gRT->SetVariable (
                CSOS_SETUP_VARIABLE_NAME,
                &gCsosBootSetupFormsetGuid,
                EFI_VARIABLE_NON_VOLATILE |
                EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof (*Config),
                (VOID *)Config
                );
}

STATIC
EFI_STATUS
QueryGopModes (
  OUT UINTN      *NumGopModes,
  OUT GOP_MODE   **GopModes
  )
{
  EFI_STATUS  Status;
  UINT32      ModeNumber;

  *NumGopModes = 0;
  *GopModes    = NULL;

  if (mGop == NULL) {
    return EFI_UNSUPPORTED;
  }

  if ((mGop->Mode == NULL) || (mGop->Mode->MaxMode == 0)) {
    return EFI_UNSUPPORTED;
  }

  *NumGopModes = mGop->Mode->MaxMode;
  *GopModes    = AllocatePool (*NumGopModes * sizeof (GOP_MODE));
  if (*GopModes == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (ModeNumber = 0; ModeNumber < mGop->Mode->MaxMode; ModeNumber++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
    UINTN                                 SizeOfInfo;

    Status = mGop->QueryMode (mGop, ModeNumber, &SizeOfInfo, &Info);
    if (EFI_ERROR (Status)) {
      FreePool (*GopModes);
      *GopModes    = NULL;
      *NumGopModes = 0;
      return Status;
    }

    (*GopModes)[ModeNumber].X = Info->HorizontalResolution;
    (*GopModes)[ModeNumber].Y = Info->VerticalResolution;
    FreePool (Info);
  }

  return EFI_SUCCESS;
}

STATIC
VOID
UpdateCurrentGopModeDesc (
  IN OUT CSOS_BOOT_SETUP  *Config
  )
{
  if ((mGopModes == NULL) || (mNumGopModes == 0)) {
    UnicodeSPrint (
      Config->CurrentGopModeDesc,
      sizeof (Config->CurrentGopModeDesc),
      L"N/A"
      );
    return;
  }

  if (Config->GopMode >= mNumGopModes) {
    Config->GopMode = 0;
  }

  UnicodeSPrint (
    Config->CurrentGopModeDesc,
    sizeof (Config->CurrentGopModeDesc),
    L"%ux%u",
    mGopModes[Config->GopMode].X,
    mGopModes[Config->GopMode].Y
    );
}

STATIC
EFI_STATUS
GfxInit (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mGfxReady) {
    return EFI_SUCCESS;
  }

  InitPalette ();

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&mGop
                  );
  if (EFI_ERROR (Status) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->LocateProtocol (
                  &gEfiHiiFontProtocolGuid,
                  NULL,
                  (VOID **)&mHiiFont
                  );
  if (EFI_ERROR (Status)) {
    mHiiFont = NULL;
  }

  GfxFillRect (
    0,
    0,
    mGop->Mode->Info->HorizontalResolution,
    mGop->Mode->Info->VerticalResolution,
    &mClrBg
    );

  mGfxReady = TRUE;
  return EFI_SUCCESS;
}

STATIC
VOID
GfxFillRect (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Color
  )
{
  if ((mGop == NULL) || (Width == 0) || (Height == 0)) {
    return;
  }

  mGop->Blt (
         mGop,
         Color,
         EfiBltVideoFill,
         0,
         0,
         X,
         Y,
         Width,
         Height,
         0
         );
}

STATIC
VOID
GfxDrawBorder (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Color
  )
{
  GfxFillRect (X, Y, Width, 1, Color);
  GfxFillRect (X, Y + Height - 1, Width, 1, Color);
  GfxFillRect (X, Y, 1, Height, Color);
  GfxFillRect (X + Width - 1, Y, 1, Height, Color);
}

STATIC
VOID
GfxDrawText (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Fg,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Bg,
  IN CONST CHAR16                     *Text
  )
{
  EFI_STATUS             Status;
  EFI_IMAGE_OUTPUT       *Blt;
  EFI_FONT_DISPLAY_INFO  FontInfo;

  if ((mHiiFont == NULL) || (mGop == NULL) || (Text == NULL)) {
    return;
  }

  Blt = AllocateZeroPool (sizeof (*Blt));
  if (Blt == NULL) {
    return;
  }

  ZeroMem (&FontInfo, sizeof (FontInfo));
  CopyMem (&FontInfo.ForegroundColor, Fg, sizeof (*Fg));
  if (Bg != NULL) {
    CopyMem (&FontInfo.BackgroundColor, Bg, sizeof (*Bg));
  }

  FontInfo.FontInfo.FontSize   = GFX_FONT_SIZE;
  FontInfo.FontInfo.FontStyle = EFI_HII_FONT_STYLE_NORMAL;
  FontInfo.FontInfo.FontName[0] = L'\0';
  FontInfo.FontInfoMask         = EFI_FONT_INFO_SYS_SIZE | EFI_FONT_INFO_SYS_STYLE;

  Blt->Width        = (UINT16)mGop->Mode->Info->HorizontalResolution;
  Blt->Height       = (UINT16)mGop->Mode->Info->VerticalResolution;
  Blt->Image.Screen = mGop;

  Status = mHiiFont->StringToImage (
                       mHiiFont,
                       EFI_HII_IGNORE_IF_NO_GLYPH |
                       EFI_HII_DIRECT_TO_SCREEN |
                       EFI_HII_IGNORE_LINE_BREAK,
                       (EFI_STRING)Text,
                       &FontInfo,
                       &Blt,
                       X,
                       Y,
                       NULL,
                       NULL,
                       NULL
                       );
  (VOID)Status;
  FreePool (Blt);
}

STATIC
VOID
GfxDrawTextCentered (
  IN UINTN                            Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Fg,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Bg,
  IN CONST CHAR16                     *Text
  )
{
  EFI_STATUS             Status;
  EFI_IMAGE_OUTPUT       *MeasureBlt;
  EFI_FONT_DISPLAY_INFO  FontInfo;
  EFI_HII_ROW_INFO       *RowInfo;
  UINTN                  RowInfoSize;
  UINTN                  TextWidth;
  UINTN                  TextX;
  UINTN                  ScreenW;

  if ((mHiiFont == NULL) || (mGop == NULL) || (Text == NULL)) {
    return;
  }

  MeasureBlt   = NULL;
  RowInfo      = NULL;
  RowInfoSize  = 0;

  ZeroMem (&FontInfo, sizeof (FontInfo));
  CopyMem (&FontInfo.ForegroundColor, Fg, sizeof (*Fg));
  if (Bg != NULL) {
    CopyMem (&FontInfo.BackgroundColor, Bg, sizeof (*Bg));
  }

  FontInfo.FontInfo.FontSize    = GFX_FONT_SIZE;
  FontInfo.FontInfo.FontStyle   = EFI_HII_FONT_STYLE_NORMAL;
  FontInfo.FontInfo.FontName[0] = L'\0';
  FontInfo.FontInfoMask         = EFI_FONT_INFO_SYS_SIZE | EFI_FONT_INFO_SYS_STYLE;

  Status = mHiiFont->StringToImage (
                       mHiiFont,
                       EFI_HII_IGNORE_IF_NO_GLYPH | EFI_HII_IGNORE_LINE_BREAK,
                       (EFI_STRING)Text,
                       &FontInfo,
                       &MeasureBlt,
                       0,
                       0,
                       &RowInfo,
                       &RowInfoSize,
                       NULL
                       );
  ScreenW = mGop->Mode->Info->HorizontalResolution;
  if (EFI_ERROR (Status) || (RowInfo == NULL) || (RowInfoSize == 0)) {
    GfxDrawText (40, Y, Fg, Bg, Text);
    goto Done;
  }

  TextWidth = RowInfo[0].LineWidth;
  TextX     = (TextWidth < ScreenW) ? ((ScreenW - TextWidth) / 2) : 0;
  GfxDrawText (TextX, Y, Fg, Bg, Text);

Done:
  if ((MeasureBlt != NULL) && (MeasureBlt->Image.Bitmap != NULL)) {
    FreePool (MeasureBlt->Image.Bitmap);
  }

  if (MeasureBlt != NULL) {
    FreePool (MeasureBlt);
  }

  if (RowInfo != NULL) {
    FreePool (RowInfo);
  }
}

STATIC
VOID
GfxDrawLogo (
  IN UINTN  X,
  IN UINTN  Y
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Blt;
  UINTN                          Index;
  UINT32                         Pixel;

  if (mGop == NULL) {
    return;
  }

  Blt = AllocatePool ((UINTN)LOGO_WIDTH * (UINTN)LOGO_HEIGHT * sizeof (*Blt));
  if (Blt == NULL) {
    return;
  }

  for (Index = 0; Index < (UINTN)LOGO_WIDTH * (UINTN)LOGO_HEIGHT; Index++) {
    Pixel = LOGO_pixels[Index];
    if (Pixel == 0) {
      Blt[Index] = mClrBg;
    } else {
      Blt[Index].Red      = (UINT8)((Pixel >> 16) & 0xFF);
      Blt[Index].Green    = (UINT8)((Pixel >> 8) & 0xFF);
      Blt[Index].Blue     = (UINT8)(Pixel & 0xFF);
      Blt[Index].Reserved = 0;
    }
  }

  mGop->Blt (
         mGop,
         Blt,
         EfiBltBufferToVideo,
         0,
         0,
         X,
         Y,
         (UINTN)LOGO_WIDTH,
         (UINTN)LOGO_HEIGHT,
         0
         );
  FreePool (Blt);
}

STATIC
VOID
GfxDrawToggle (
  IN UINTN    X,
  IN UINTN    Y,
  IN UINTN    Width,
  IN UINTN    Height,
  IN BOOLEAN  On,
  IN BOOLEAN  Disabled
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Track;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Knob;
  UINTN                          KnobX;

  Track = Disabled ? &mClrDisabled : (On ? &mClrAccent : &mClrBorder);
  Knob  = Disabled ? &mClrMuted : &mClrText;

  GfxFillRect (X, Y, Width, Height, Track);
  KnobX = On ? (X + Width - Height + 2) : (X + 2);
  GfxFillRect (KnobX, Y + 2, Height - 4, Height - 4, Knob);
}

STATIC
VOID
GfxDrawButton (
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height,
  IN CONST CHAR16                     *Label,
  IN BOOLEAN                          Primary,
  IN BOOLEAN                          Focused
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Bg;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Fg;
  UINTN                          TextX;
  UINTN                          TextY;

  if (Focused) {
    Bg = Primary ? &mClrAccent : &mClrBtnHover;
    GfxDrawBorder (X, Y, Width, Height, &mClrAccent);
  } else {
    Bg = Primary ? &mClrAccentDim : &mClrBtn;
    GfxDrawBorder (X, Y, Width, Height, &mClrBorder);
  }

  GfxFillRect (X + 1, Y + 1, Width - 2, Height - 2, Bg);
  Fg    = &mClrText;
  TextX = X + 12;
  TextY = Y + (Height > GFX_FONT_SIZE ? (Height - GFX_FONT_SIZE) / 2 : 4);
  GfxDrawText (TextX, TextY, Fg, Bg, Label);
}

STATIC
VOID
DrawSetupScreen (
  VOID
  )
{
  UINTN                          W;
  UINTN                          H;
  UINTN                          CardW;
  UINTN                          CardH;
  UINTN                          CardX;
  UINTN                          CardY;
  UINTN                          RowY;
  UINTN                          RowH;
  UINTN                          LabelX;
  UINTN                          ValueX;
  UINTN                          BtnY;
  UINTN                          BtnW;
  UINTN                          BtnH;
  UINTN                          BtnGap;
  UINTN                          BtnX;
  CHAR16                         Buf[48];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *RowBg;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *LabelClr;
  BOOLEAN                        GopDisabled;

  if (!mGfxReady) {
    return;
  }

  W = mGop->Mode->Info->HorizontalResolution;
  H = mGop->Mode->Info->VerticalResolution;
  GfxFillRect (0, 0, W, H, &mClrBg);

  CardW = (W < 860) ? (W - 48) : 760;
  CardH = 430;
  CardX = (W - CardW) / 2;
  CardY = (H - CardH) / 2;

  GfxFillRect (CardX, CardY, CardW, CardH, &mClrSurface);
  GfxDrawBorder (CardX, CardY, CardW, CardH, &mClrBorder);
  GfxFillRect (CardX, CardY, CardW, 4, &mClrAccent);

  GfxDrawText (CardX + 28, CardY + 22, &mClrText, &mClrSurface, L"CsosBoot Setup");
  GfxDrawText (CardX + 28, CardY + 48, &mClrMuted, &mClrSurface, STR_SUBTITLE);

  RowH    = 52;
  RowY    = CardY + 88;
  LabelX  = CardX + 28;
  ValueX  = CardX + CardW - 260;
  GopDisabled = (mSetupConfig.PreferEdid != 0);

  //
  // Boot timeout row
  //
  RowBg   = (mFocus == FocusBootTimeout) ? &mClrFocus : &mClrSurface;
  LabelClr = &mClrText;
  GfxFillRect (CardX + 12, RowY, CardW - 24, RowH, RowBg);
  if (mFocus == FocusBootTimeout) {
    GfxFillRect (CardX + 12, RowY, 3, RowH, &mClrAccent);
  }

  GfxDrawText (LabelX, RowY + 16, LabelClr, RowBg, STR_BOOT_TIMEOUT);
  UnicodeSPrint (Buf, sizeof (Buf), L"%u", mSetupConfig.BootTimeout);
  GfxDrawText (ValueX + 80, RowY + 16, &mClrAccent, RowBg, Buf);
  GfxDrawBorder (ValueX, RowY + 12, 48, 28, &mClrBorder);
  GfxDrawText (ValueX + 16, RowY + 16, &mClrMuted, RowBg, L"-");
  GfxDrawBorder (ValueX + 140, RowY + 12, 48, 28, &mClrBorder);
  GfxDrawText (ValueX + 156, RowY + 16, &mClrMuted, RowBg, L"+");

  RowY += RowH;

  //
  // Auto boot toggle
  //
  RowBg    = (mFocus == FocusAutoBoot) ? &mClrFocus : &mClrSurface;
  GfxFillRect (CardX + 12, RowY, CardW - 24, RowH, RowBg);
  if (mFocus == FocusAutoBoot) {
    GfxFillRect (CardX + 12, RowY, 3, RowH, &mClrAccent);
  }

  GfxDrawText (LabelX, RowY + 16, &mClrText, RowBg, STR_AUTO_BOOT);
  GfxDrawToggle (ValueX + 60, RowY + 16, 52, 24, mSetupConfig.AutoBoot != 0, FALSE);

  RowY += RowH;

  //
  // Prefer EDID toggle
  //
  RowBg = (mFocus == FocusPreferEdid) ? &mClrFocus : &mClrSurface;
  GfxFillRect (CardX + 12, RowY, CardW - 24, RowH, RowBg);
  if (mFocus == FocusPreferEdid) {
    GfxFillRect (CardX + 12, RowY, 3, RowH, &mClrAccent);
  }

  GfxDrawText (LabelX, RowY + 16, &mClrText, RowBg, STR_PREFER_EDID);
  GfxDrawToggle (ValueX + 60, RowY + 16, 52, 24, mSetupConfig.PreferEdid != 0, FALSE);

  RowY += RowH;

  //
  // GOP mode row
  //
  RowBg    = (mFocus == FocusGopMode) ? &mClrFocus : &mClrSurface;
  LabelClr = GopDisabled ? &mClrMuted : &mClrText;
  GfxFillRect (CardX + 12, RowY, CardW - 24, RowH, RowBg);
  if (mFocus == FocusGopMode) {
    GfxFillRect (CardX + 12, RowY, 3, RowH, &mClrAccent);
  }

  GfxDrawText (LabelX, RowY + 16, LabelClr, RowBg, STR_GOP_MODE);
  UpdateCurrentGopModeDesc (&mSetupConfig);
  GfxDrawText (
    ValueX,
    RowY + 16,
    GopDisabled ? &mClrMuted : &mClrAccent,
    RowBg,
    mSetupConfig.CurrentGopModeDesc
    );
  if (!GopDisabled) {
    GfxDrawBorder (ValueX + 150, RowY + 12, 36, 28, &mClrBorder);
    GfxDrawText (ValueX + 160, RowY + 16, &mClrMuted, RowBg, L"<");
    GfxDrawBorder (ValueX + 196, RowY + 12, 36, 28, &mClrBorder);
    GfxDrawText (ValueX + 206, RowY + 16, &mClrMuted, RowBg, L">");
  }

  BtnY   = CardY + CardH - 72;
  BtnH   = 40;
  BtnGap = 12;
  BtnW   = (CardW - 56 - 2 * BtnGap) / 3;
  BtnX   = CardX + 28;

  GfxDrawButton (
    BtnX,
    BtnY,
    BtnW,
    BtnH,
    STR_BTN_SAVE_BOOT,
    TRUE,
    mFocus == FocusActionSaveBoot
    );
  GfxDrawButton (
    BtnX + BtnW + BtnGap,
    BtnY,
    BtnW,
    BtnH,
    STR_BTN_BOOT,
    FALSE,
    mFocus == FocusActionBoot
    );
  GfxDrawButton (
    BtnX + 2 * (BtnW + BtnGap),
    BtnY,
    BtnW,
    BtnH,
    STR_BTN_BACK,
    FALSE,
    mFocus == FocusActionBack
    );

  GfxDrawText (
    CardX + 28,
    CardY + CardH - 24,
    &mClrMuted,
    &mClrSurface,
    STR_HELP
    );
}

STATIC
VOID
DrawBootWaitHint (
  IN UINT16  Remaining
  )
{
  CHAR16  Buf[80];
  UINTN   W;
  UINTN   H;
  UINTN   LogoY;
  UINTN   TextY;
  UINTN   TextH;

  if (!mGfxReady || (mGop == NULL)) {
    return;
  }

  W     = mGop->Mode->Info->HorizontalResolution;
  H     = mGop->Mode->Info->VerticalResolution;
  LogoY = (H > (UINTN)LOGO_HEIGHT) ? ((H - (UINTN)LOGO_HEIGHT) / 2) : 0;
  TextY = LogoY + (UINTN)LOGO_HEIGHT + LOGO_SPLASH_TEXT_GAP;
  TextH = (UINTN)GFX_FONT_SIZE + 8;

  UnicodeSPrint (
    Buf,
    sizeof (Buf),
    L"Enter F2 for Setup, auto boot in %u sec",
    Remaining
    );

  GfxFillRect (0, TextY, W, TextH, &mClrBg);
  GfxDrawTextCentered (TextY, &mClrMuted, &mClrBg, Buf);
}

STATIC
VOID
DrawBootWaitScreen (
  IN UINT16  Remaining
  )
{
  UINTN  W;
  UINTN  H;
  UINTN  LogoX;
  UINTN  LogoY;

  if (!mGfxReady) {
    return;
  }

  W = mGop->Mode->Info->HorizontalResolution;
  H = mGop->Mode->Info->VerticalResolution;
  GfxFillRect (0, 0, W, H, &mClrBg);

  /* Center the logo itself; hint text is drawn below and does not shift it. */
  LogoX = (W > (UINTN)LOGO_WIDTH) ? ((W - (UINTN)LOGO_WIDTH) / 2) : 0;
  LogoY = (H > (UINTN)LOGO_HEIGHT) ? ((H - (UINTN)LOGO_HEIGHT) / 2) : 0;

  GfxDrawLogo (LogoX, LogoY);
  DrawBootWaitHint (Remaining);
}

STATIC
VOID
AdjustFocusedValue (
  IN INTN  Delta
  )
{
  if (Delta == 0) {
    return;
  }

  switch (mFocus) {
    case FocusBootTimeout:
      if (Delta > 0) {
        if (mSetupConfig.BootTimeout < 60) {
          mSetupConfig.BootTimeout++;
        }
      } else if (mSetupConfig.BootTimeout > 0) {
        mSetupConfig.BootTimeout--;
      }

      break;

    case FocusAutoBoot:
      mSetupConfig.AutoBoot = (mSetupConfig.AutoBoot == 0) ? 1 : 0;
      break;

    case FocusPreferEdid:
      mSetupConfig.PreferEdid = (mSetupConfig.PreferEdid == 0) ? 1 : 0;
      break;

    case FocusGopMode:
      if (mSetupConfig.PreferEdid != 0) {
        break;
      }

      if (mNumGopModes == 0) {
        break;
      }

      if (Delta > 0) {
        mSetupConfig.GopMode = (mSetupConfig.GopMode + 1) % (UINT32)mNumGopModes;
      } else {
        mSetupConfig.GopMode = (mSetupConfig.GopMode + (UINT32)mNumGopModes - 1) %
                               (UINT32)mNumGopModes;
      }

      UpdateCurrentGopModeDesc (&mSetupConfig);
      break;

    default:
      break;
  }
}

STATIC
VOID
MoveFocus (
  IN INTN  Delta
  )
{
  INTN  NewFocus;

  NewFocus = (INTN)mFocus + Delta;
  if (NewFocus < 0) {
    NewFocus = FocusMax - 1;
  } else if (NewFocus >= FocusMax) {
    NewFocus = 0;
  }

  mFocus = (SETUP_FOCUS)NewFocus;
}

STATIC
EFI_STATUS
ReadKey (
  OUT EFI_INPUT_KEY  *Key
  )
{
  EFI_STATUS                     Status;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;

  Status = gBS->HandleProtocol (
                  gST->ConsoleInHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  (VOID **)&ConIn
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return ConIn->ReadKeyStroke (ConIn, Key);
}

STATIC
EFI_STATUS
WaitForKey (
  OUT EFI_INPUT_KEY  *Key
  )
{
  EFI_STATUS  Status;

  while (TRUE) {
    Status = ReadKey (Key);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    gBS->Stall (10000);
  }
}

EFI_STATUS
CsosSetupUtilityInit (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS  Status;

  (VOID)ImageHandle;

  mGop         = NULL;
  mHiiFont     = NULL;
  mGfxReady    = FALSE;
  mNumGopModes = 0;
  mGopModes    = NULL;
  mFocus       = FocusBootTimeout;

  SetupLoadFromVariable (&mSetupConfig);

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&mGop
                  );
  if (EFI_ERROR (Status) || (mGop == NULL)) {
    return EFI_UNSUPPORTED;
  }

  Status = QueryGopModes (&mNumGopModes, &mGopModes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (mSetupConfig.GopMode >= mNumGopModes) {
    mSetupConfig.GopMode = 0;
  }

  UpdateCurrentGopModeDesc (&mSetupConfig);
  return EFI_SUCCESS;
}

EFI_STATUS
CsosSetupUtilityBeginUi (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = GfxInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CsosSetupUtilityRun (
  OUT BOOLEAN  *BootRequested
  )
{
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;

  if (BootRequested == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *BootRequested = FALSE;

  if (!mGfxReady) {
    return EFI_UNSUPPORTED;
  }

  mFocus = FocusBootTimeout;
  DrawSetupScreen ();

  while (TRUE) {
    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    switch (Key.ScanCode) {
      case SCAN_UP:
        MoveFocus (-1);
        break;

      case SCAN_DOWN:
        MoveFocus (1);
        break;

      case SCAN_LEFT:
        if (mFocus >= FocusActionSaveBoot) {
          MoveFocus (-1);
        } else {
          AdjustFocusedValue (-1);
        }

        break;

      case SCAN_RIGHT:
        if (mFocus >= FocusActionSaveBoot) {
          MoveFocus (1);
        } else {
          AdjustFocusedValue (1);
        }

        break;

      case SCAN_ESC:
        *BootRequested = FALSE;
        return EFI_SUCCESS;

      case SCAN_F10:
        *BootRequested = TRUE;
        return SetupSaveToVariable (&mSetupConfig);

      default:
        break;
    }

    if (Key.ScanCode != SCAN_NULL) {
      DrawSetupScreen ();
      continue;
    }

    switch (Key.UnicodeChar) {
      case CHAR_CARRIAGE_RETURN:
        if (mFocus == FocusActionSaveBoot) {
          *BootRequested = TRUE;
          return SetupSaveToVariable (&mSetupConfig);
        }

        if (mFocus == FocusActionBoot) {
          *BootRequested = TRUE;
          return EFI_SUCCESS;
        }

        if (mFocus == FocusActionBack) {
          *BootRequested = FALSE;
          return EFI_SUCCESS;
        }

        AdjustFocusedValue (1);
        break;

      case L'+':
      case L'=':
        AdjustFocusedValue (1);
        break;

      case L'-':
      case L'_':
        AdjustFocusedValue (-1);
        break;

      case 0x1B:
        *BootRequested = FALSE;
        return EFI_SUCCESS;

      default:
        break;
    }

    DrawSetupScreen ();
  }
}

EFI_STATUS
CsosSetupUtilityWaitForKey (
  IN  UINT16   TimeoutSeconds,
  OUT BOOLEAN  *EnterSetup
  )
{
  EFI_STATUS  Status;
  EFI_INPUT_KEY  Key;
  UINT16      Remaining;
  UINTN       Tick;

  if (EnterSetup == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *EnterSetup = FALSE;

  if (TimeoutSeconds == 0) {
    return EFI_SUCCESS;
  }

  if (!mGfxReady) {
    gBS->Stall (TimeoutSeconds * 1000000);
    return EFI_SUCCESS;
  }

  DrawBootWaitScreen (TimeoutSeconds);

  for (Remaining = TimeoutSeconds; Remaining > 0; Remaining--) {
    DrawBootWaitHint (Remaining);
    for (Tick = 0; Tick < 100; Tick++) {
      Status = ReadKey (&Key);
      if (!EFI_ERROR (Status) && (Key.ScanCode == SCAN_F2)) {
        *EnterSetup = TRUE;
        return EFI_SUCCESS;
      }

      gBS->Stall (10000);
    }
  }

  return EFI_SUCCESS;
}

VOID
CsosSetupUtilityGetConfig (
  OUT CSOS_BOOT_SETUP  *Config
  )
{
  if (Config != NULL) {
    CopyMem (Config, &mSetupConfig, sizeof (*Config));
  }
}

VOID
CsosSetupUtilityFree (
  VOID
  )
{
  if (mGopModes != NULL) {
    FreePool (mGopModes);
    mGopModes = NULL;
  }

  mNumGopModes = 0;
  mGfxReady    = FALSE;
  mGop         = NULL;
  mHiiFont     = NULL;
}
