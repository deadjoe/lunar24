// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// host/main.mm — the self-authored, nib-free bootstrap (Path C). This is OUR OWN
// entry point; iPlug2 (third_party/iPlug2, pinned d54f6905) is used ONLY as an
// unmodified library. The stock stock IPlugAPP_main.cpp is NOT compiled (it would
// bring a second main()+SWELLAppMain and, worse, edit iPlug2); instead this file
// provides both:
//
//   * main()          — a manual replication of the nib lifecycle that
//                       NSApplicationMain() would drive from MainMenu.xib. The
//                       machine only has Command Line Tools (no ibtool/Xcode), so
//                       the stock nib path cannot build. We bypass it: instantiate
//                       SWELLAppController directly (+new), set it as delegate,
//                       dispatch awakeFromNib (-> SWELLAPP_ONLOAD) and let
//                       [NSApplication run] fire applicationDidFinishLaunching
//                       (-> SWELLAPP_LOADED -> the editor window).
//   * SWELLAppMain()  — the C dispatcher SWELLAppController calls for
//                       ONLOAD/LOADED/ONCOMMAND/DESTROY/PROCESSMESSAGE.
//
// The window it opens is sized by lunar24::host::compute_window_layout (via the
// plugin's mMakeGraphicsFunc -> SetEditorSize), i.e. the host does NOT hardcode a
// window size — the geometry choke point is the only place the size comes from.

#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <cstring>

#include "IPlugAPP_host.h"   // IPlugAPPHost, sInstance, MainDlgProc, WDL_String
#include "IPlugSWELL.h"      // SWELL API: CreateDialog, LoadMenu, SetMenu, menu...
#include "config.h"
#include "resource.h"

using namespace iplug;

// ---------------------------------------------------------------------------
// Geometry probes (called from host/plugin.cpp, which does the iPlug-free layout
// math). The plugin asks the host for the screen's LOGICAL visible area and the
// retina backing scale; it never needs a framework type itself, so the geometry
// stays at the core/host boundary.
// ---------------------------------------------------------------------------
extern "C" double lunar_host_avail_logical_w()
{
  return [[NSScreen mainScreen] visibleFrame].size.width;
}

extern "C" double lunar_host_avail_logical_h()
{
  return [[NSScreen mainScreen] visibleFrame].size.height;
}

extern "C" double lunar_host_screen_scale()
{
  return [[NSScreen mainScreen] backingScaleFactor];
}

extern "C" bool lunar_host_force_clamp()
{
  const char* v = std::getenv("LUNAR_HOST_FORCE_CLAMP");
  return (v != nullptr) && (std::strcmp(v, "1") == 0);
}

// ---------------------------------------------------------------------------
// The global editor window HWND. IPlugAPP_dialog.cpp (compiled unmodified) writes
// gHWND in MainDlgProc WM_INITDIALOG and uses it throughout, so it must be a real
// symbol. gHINSTANCE is intentionally NOT defined: on macOS the SWELL DialogBox /
// CreateDialog / LoadMenu macros discard their hinst argument (swell-functions.h),
// so gHINSTANCE never appears in compiled code.
// ---------------------------------------------------------------------------
HWND gHWND;

// ---------------------------------------------------------------------------
// SaveWindowScreenshot — referenced unconditionally by IPlugAPP_dialog.cpp
// (extern, called from the screenshot timer + the ID_SCREENSHOT menu case), so the
// linker needs the symbol. P5-① mandate is window size, not capture, so this is an
// honest stub: it compiles (no deprecated CGWindowListCreateImage) and never runs
// in our probe (we never enable screenshot mode). A real capture is a later slice.
// ---------------------------------------------------------------------------
extern "C" bool SaveWindowScreenshot(void* hwnd, const char* path)
{
  static_cast<void>(hwnd);
  static_cast<void>(path);
  return false;
}

// ---------------------------------------------------------------------------
// Command-line state (mirrors IPlugAPP_main.cpp): --no-io lets the probe open the
// window without touching the audio device, and --screenshot is threaded through
// for parity (even though capture is stubbed above).
// ---------------------------------------------------------------------------
static WDL_String gScreenshotPath;
static bool gNoIO = false;

// ---------------------------------------------------------------------------
// SWELLAppMain — the dispatcher. Re-produced verbatim from the upstream APP entry
// (we own this now, we do not link the upstream TU). Faithful copy minimizes the
// risk of inventing SWELL API misuse.
// ---------------------------------------------------------------------------
INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  IPlugAPPHost* pAppHost = nullptr;

  switch (msg)
  {
    case SWELLAPP_ONLOAD:
    {
      pAppHost = IPlugAPPHost::Create();

      if (gScreenshotPath.GetLength() > 0) {
        pAppHost->SetScreenshotPath(gScreenshotPath.Get());
        pAppHost->SetNoIO(true);  // screenshot mode implies --no-io
      }
      else if (gNoIO) {
        pAppHost->SetNoIO(true);
      }

      pAppHost->Init();
      pAppHost->TryToChangeAudio();
      break;
    }
    case SWELLAPP_LOADED:
    {
      pAppHost = IPlugAPPHost::sInstance.get();

      HMENU menu = SWELL_GetCurrentMenu();

      if (menu)
      {
        // work on a new menu
        menu = SWELL_DuplicateMenu(menu);
        HMENU src = LoadMenu(NULL, MAKEINTRESOURCE(IDR_MENU1));

        for (int x = 0; x < GetMenuItemCount(src) - 1; x++)
        {
          HMENU sm = GetSubMenu(src, x);
          if (sm)
          {
            char str[1024];
            MENUITEMINFO mii = {sizeof(mii), MIIM_TYPE};
            mii.dwTypeData = str;
            mii.cch = sizeof(str);
            str[0] = 0;
            GetMenuItemInfo(src, x, TRUE, &mii);
            MENUITEMINFO mi = {sizeof(mi), MIIM_STATE|MIIM_SUBMENU|MIIM_TYPE, MFT_STRING,
                               0, 0, SWELL_DuplicateMenu(sm), NULL, NULL, 0, str};
            InsertMenuItem(menu, x + 1, TRUE, &mi);
          }
        }
      }

      if (menu)
      {
        HMENU sm = GetSubMenu(menu, 1);
        DeleteMenu(sm, ID_QUIT, MF_BYCOMMAND);        // in the system menu on OSX
        DeleteMenu(sm, ID_PREFERENCES, MF_BYCOMMAND); // in the system menu on OSX

        // remove any trailing separators
        int a = GetMenuItemCount(sm);
        while (a > 0 && GetMenuItemID(sm, a - 1) == 0)
          DeleteMenu(sm, --a, MF_BYPOSITION);

        DeleteMenu(menu, 1, MF_BYPOSITION); // delete file menu
      }
#ifdef ID_SCREENSHOT
      SetMenuItemModifier(menu, ID_SCREENSHOT, MF_BYCOMMAND, 'S', FCONTROL | FSHIFT);
#endif

#if !defined _DEBUG || defined NO_IGRAPHICS
      if (menu)
      {
        HMENU sm = GetSubMenu(menu, 1);
        DeleteMenu(sm, ID_LIVE_EDIT, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_BOUNDS, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_DRAWN, MF_BYCOMMAND);
        DeleteMenu(sm, ID_SHOW_FPS, MF_BYCOMMAND);

        int a = GetMenuItemCount(sm);
        while (a > 0 && GetMenuItemID(sm, a - 1) == 0)
          DeleteMenu(sm, --a, MF_BYPOSITION);

        if (GetMenuItemCount(sm) == 0)
          DeleteMenu(menu, 1, MF_BYPOSITION);
      }
#else
      SetMenuItemModifier(menu, ID_LIVE_EDIT, MF_BYCOMMAND, 'E', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_DRAWN, MF_BYCOMMAND, 'D', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_BOUNDS, MF_BYCOMMAND, 'B', FCONTROL);
      SetMenuItemModifier(menu, ID_SHOW_FPS, MF_BYCOMMAND, 'F', FCONTROL);
#endif

      HWND hwnd = CreateDialog(gHINST, MAKEINTRESOURCE(IDD_DIALOG_MAIN), NULL,
                               IPlugAPPHost::MainDlgProc);

      if (menu)
      {
        SetMenu(hwnd, menu);
        SWELL_SetDefaultModalWindowMenu(menu); // other windows get the stock menus
      }
      break;
    }
    case SWELLAPP_ONCOMMAND:
      // catch commands coming from the system menu etc.
      if (gHWND && (parm1 & 0xffff))
        SendMessage(gHWND, WM_COMMAND, parm1 & 0xffff, 0);
      break;
    case SWELLAPP_DESTROY:
      if (gHWND)
        DestroyWindow(gHWND);
      break;
    case SWELLAPP_PROCESSMESSAGE:
    {
      MSG* pMSG = (MSG*)parm1;
      NSView* pContentView = (NSView*)pMSG->hwnd;
      NSEvent* pEvent = (NSEvent*)parm2;
      int etype = (int)[pEvent type];

      bool textField = [pContentView isKindOfClass:[NSText class]];

      if (!textField && etype == NSKeyDown)
      {
        int flag, code = SWELL_MacKeyToWindowsKey(pEvent, &flag);

        if (!(flag & ~FVIRTKEY) && (code == VK_RETURN || code == VK_ESCAPE))
        {
          [pContentView keyDown:pEvent];
          return 1;
        }
      }
      break;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// main() — the nib-free bootstrap. Replaces the stock NSApplicationMain() (which
// needs MainMenu.xib + ibtool, unavailable on a CLT-only machine) with the manual
// lifecycle that the nib path would otherwise drive. This is the proof the
// iPlug2 bootstrap gap is a TOOLCHAIN artifact, not a framework failure.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
  for (int i = 1; i < argc; i++)
  {
    if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
    {
      gScreenshotPath.Set(argv[i + 1]);
      i++;
    }
    else if (std::strcmp(argv[i], "--no-io") == 0)
    {
      gNoIO = true;
    }
  }

  @autoreleasepool
  {
    NSApplication* app = [NSApplication sharedApplication];

    Class ctrlClass = NSClassFromString(@"SWELLAppController");
    id ctrl = ctrlClass ? [ctrlClass new] : nil;
    if (!ctrl)
    {
      NSLog(@"Lunar24Host: SWELLAppController class not found; cannot bootstrap.\n");
      return 1;
    }

    [app setDelegate:ctrl];
    [ctrl performSelector:@selector(awakeFromNib)];   // -> SWELLAPP_ONLOAD

    [app activateIgnoringOtherApps:YES];
    [app run];   // -> applicationDidFinishLaunching -> SWELLAPP_LOADED -> window
  }

  return 0;
}

// ---------------------------------------------------------------------------
// SWELL resource generation. MUST be in this TU (the one that would otherwise be
// IPlugAPP_main.cpp): swell-dlggen.h + main.rc_mac_dlg build the dialog resource
// index (SWELL_curmodule_dialogresource_head) and swell-menugen.h + main.rc_mac_menu
// build the menu resource index, both consumed by CreateDialog/LoadMenu above.
// ---------------------------------------------------------------------------
#define CBS_HASSTRINGS 0
#define SWELL_DLG_SCALE_AUTOGEN 1
#define SET_IDD_DIALOG_PREF_SCALE 1.5
#if PLUG_HOST_RESIZE
#define SWELL_DLG_FLAGS_AUTOGEN SWELL_DLG_WS_FLIPPED|SWELL_DLG_WS_RESIZABLE
#endif
#include "swell-dlggen.h"
#include "resources/main.rc_mac_dlg"
#include "swell-menugen.h"
#include "resources/main.rc_mac_menu"
