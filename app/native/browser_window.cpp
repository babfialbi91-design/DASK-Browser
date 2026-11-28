// DASK Browser - native Win32 window hosting the UI browser and all tabs.
// Layout and host-window bookkeeping live on this thread; all browser logic
// lives in TabManager on the CEF UI thread (see tab_manager.h).
#include "browser_window.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// GET_X_LPARAM / GET_Y_LPARAM (normally from windowsx.h — including that
// header here would poison the CEF DOM headers with its GetFirstChild and
// GetNextSibling function-like macros).
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "dask_app.h"
#include "profiles.h"
#include "storage/database.h"
#include "storage/stores.h"
#include "tab_manager.h"

namespace dask {
namespace {

constexpr wchar_t kWindowClassName[] = L"DaskBrowserMainWindow";
constexpr wchar_t kHostClassName[] = L"DaskTabHost";
constexpr wchar_t kMenuPopupClassName[] = L"DaskMenuPopup";
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 800;

// Last applied flyout state (WM_DASK_MENU_WIDE): skip redundant window
// resizes/region repaints so hovering between flyout rows never flickers.
int g_menu_wide = -1, g_menu_top = -1, g_menu_fly = -1;

void ResetMenuWideCache() {
  g_menu_wide = -1;
  g_menu_top = -1;
  g_menu_fly = -1;
}

// UTF-8 helper for the init-failure diagnostic file.
std::string NarrowUtf8(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  if (n) {
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n,
                          nullptr, nullptr);
  }
  return out;
}

// Win32-thread-only bookkeeping.
std::map<int64_t, HWND> g_hosts;  // tab id -> host window (0 = UI toolbar)
HWND g_main_hwnd = nullptr;
HHOOK g_kbd_hook = nullptr;

bool CefRuntimeFilesPresent(const std::wstring& exe_dir) {
  const std::wstring checks[] = {exe_dir + L"\\libcef.dll",
                                 exe_dir + L"\\icudtl.dat",
                                 exe_dir + L"\\resources.pak"};
  for (const auto& path : checks) {
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
      return false;
  }
  return true;
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

void ShowFatalError(const std::wstring& title, const std::wstring& message) {
  ::MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

// Positions the UI browser and every tab host inside the main window.
void DoLayout() {
  if (!g_main_hwnd) return;
  RECT rc;
  if (!::GetClientRect(g_main_hwnd, &rc)) return;
  const int width = rc.right - rc.left;
  const int height = rc.bottom - rc.top;
  // Phase 8: app windows (--app) are pure content — no toolbar strip.
  const bool app_mode = TabManager::launch_app();
  const int content_y = app_mode ? 0 : kUiToolbarHeight;
  const int content_h = height - content_y;

  std::vector<int64_t> order;
  int64_t active_id = 0;
  TabManager::Get()->GetLayout(order, active_id);

  HDWP dwp = ::BeginDeferWindowPos(static_cast<int>(g_hosts.size()) + 1);
  for (const auto& [id, host] : g_hosts) {
    if (!host) continue;
    if (id == 0) {
      // UI toolbar across the top - always visible.
      if (dwp)
        dwp = ::DeferWindowPos(dwp, host, nullptr, 0, 0, width,
                              kUiToolbarHeight,
                              SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      const bool visible = (id == active_id) && content_h > 0;
      if (dwp) {
        dwp = ::DeferWindowPos(
            dwp, host, nullptr, 0, content_y, width,
            content_h > 0 ? content_h : 1,
            visible ? (SWP_NOZORDER | SWP_NOACTIVATE)
                    : (SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW));
      }
    }
    // Keep the embedded Chromium child in sync with the host. CEF 158 uses
    // the aura widget class (Chrome_WidgetWin_1); keep _0 as a fallback.
    HWND chrome = ::FindWindowExW(host, nullptr, L"Chrome_WidgetWin_1", nullptr);
    if (!chrome)
      chrome = ::FindWindowExW(host, nullptr, L"Chrome_WidgetWin_0", nullptr);
    if (chrome) {
      const int h = (id == 0) ? kUiToolbarHeight : content_h;
      if (h > 0) {
        ::SetWindowPos(chrome, nullptr, 0, 0, width, h,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
  }
  if (dwp) ::EndDeferWindowPos(dwp);
}

// Focus a content tab natively after activation (also hides the others).
void ShowActiveTab() {
  std::vector<int64_t> order;
  int64_t active_id = 0;
  TabManager::Get()->GetLayout(order, active_id);
  for (const auto& [id, host] : g_hosts) {
    if (id == 0) continue;
    ::ShowWindow(host, id == active_id ? SW_SHOWNA : SW_HIDE);
  }
}

// ---------------------------------------------------------------------------
// Global keyboard shortcut hook (WH_KEYBOARD_LL). Active only while DASK is
// the foreground window, so system-wide shortcuts are never swallowed.
// ---------------------------------------------------------------------------

bool KeyDebounced(int vk, int gap_ms) {
  static DWORD last[256] = {};
  const DWORD now = ::GetTickCount();
  if (now - last[vk] < static_cast<DWORD>(gap_ms)) return true;
  last[vk] = now;
  return false;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (info && ::GetForegroundWindow() == g_main_hwnd) {
      const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
      const int vk = static_cast<int>(info->vkCode);
      TabManager* mgr = TabManager::Get();
      bool handled = true;

      if (ctrl && !alt) {
        switch (vk) {
          case 'L':
            if (!KeyDebounced(vk, 200)) mgr->FocusAddress();
            break;
          case 'T':
            if (!KeyDebounced(vk, 250)) mgr->CreateTab(std::wstring(kNewTabUrl));
            break;
          case 'W': {
            if (!KeyDebounced(vk, 250)) {
              std::vector<int64_t> order;
              int64_t active = 0;
              mgr->GetLayout(order, active);
              if (active != 0) mgr->CloseTab(active);
            }
            break;
          }
          case 'R':
            mgr->Reload();
            break;
          case 'D':
            if (!KeyDebounced(vk, 250)) mgr->ToggleBookmarkActive();
            break;
          case 'N':
            if (shift && !KeyDebounced(vk, 300)) {
              // Phase 7: a real separate private window (new process).
              mgr->NewBrowserWindow(true);  // Ctrl+Shift+N
            } else if (!shift && !KeyDebounced(vk, 300)) {
              mgr->NewBrowserWindow(false);  // Ctrl+N
            }
            break;
          case 'J':  // Ctrl+J: open the downloads page
            if (!KeyDebounced(vk, 300)) {
              mgr->CreateTab(L"http://dask.ui/settings.html#downloads");
            }
            break;
          case 'F':  // Ctrl+F: find in page (state flag opens the find bar)
            if (!KeyDebounced(vk, 300)) mgr->OpenFindBar();
            break;
          case 'U':  // Ctrl+U: view page source (Chromium's own viewer)
            if (!KeyDebounced(vk, 300)) mgr->ViewSourceActive();
            break;
          case VK_TAB:
            mgr->CycleTab(shift ? -1 : 1);
            break;
          default:
            handled = false;
        }
      } else if (vk == VK_F11) {
        mgr->ToggleFullscreen();
      } else if (alt && !ctrl) {
        if (vk == VK_LEFT) {
          mgr->Back();
        } else if (vk == VK_RIGHT) {
          mgr->Forward();
        } else {
          handled = false;
        }
      } else {
        handled = false;
      }

      if (handled) return 1;
    }
  }
  return ::CallNextHookEx(g_kbd_hook, code, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Main window.
// ---------------------------------------------------------------------------

// Phase 7: menu popup window proc — closes the menu when it loses focus
// (clicks outside / the user switched away).
LRESULT CALLBACK MenuPopupWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  switch (msg) {
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) {
        const HWND main = ::GetWindow(hwnd, GW_OWNER);
        if (main) ::PostMessageW(main, WM_DASK_MENU_CLOSED, 0, 0);
      }
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_NCCALCSIZE: {
      // Phase 9: custom title bar — the React tab strip replaces the native
      // caption (no "DASK Browser" text row, no duplicated buttons). Keep
      // WS_OVERLAPPEDWINDOW so resize borders, Aero snap and Alt+Space work;
      // WM_NCHITTEST below restores the invisible resize borders.
      // --app windows keep the standard caption (their caption shows the
      // page title, there is no toolbar to host controls).
      if (wparam && !TabManager::launch_app()) {
        if (::IsZoomed(hwnd)) {
          // Maximized: the window is inflated by the invisible resize
          // border — keep the content inside the screen.
          const int pad = ::GetSystemMetrics(SM_CXPADDEDBORDER);
          const int cx = ::GetSystemMetrics(SM_CXFRAME) + pad;
          const int cy = ::GetSystemMetrics(SM_CYFRAME) + pad;
          auto* rects = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
          rects->rgrc[0].left += cx;
          rects->rgrc[0].top += cy;
          rects->rgrc[0].right -= cx;
          rects->rgrc[0].bottom -= cy;
        }
        return 0;  // client area == window rect
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_NCHITTEST: {
      // Phase 9: the removed frame ate the resize borders — reimplement
      // them (invisible, ~8px) on all four edges. The tab strip itself is
      // covered by the CEF toolbar child, so drag handling goes through
      // the bridge (dragWindow -> WM_NCLBUTTONDOWN/HTCAPTION).
      if (!TabManager::launch_app() && !::IsZoomed(hwnd)) {
        const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT rc;
        if (::GetWindowRect(hwnd, &rc)) {
          const int pad = ::GetSystemMetrics(SM_CXPADDEDBORDER);
          const int cx = ::GetSystemMetrics(SM_CXSIZEFRAME) + pad;
          const int cy = ::GetSystemMetrics(SM_CYSIZEFRAME) + pad;
          const bool top = pt.y < rc.top + cy;
          const bool bottom = pt.y >= rc.bottom - cy;
          const bool left = pt.x < rc.left + cx;
          const bool right = pt.x >= rc.right - cx;
          if (top && left) return HTTOPLEFT;
          if (top && right) return HTTOPRIGHT;
          if (bottom && left) return HTBOTTOMLEFT;
          if (bottom && right) return HTBOTTOMRIGHT;
          if (top) return HTTOP;
          if (bottom) return HTBOTTOM;
          if (left) return HTLEFT;
          if (right) return HTRIGHT;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_DASK_DRAG: {
      // Phase 9: the toolbar's drag region — start a system caption drag
      // (supports Aero snap + the Alt+Space menu) from the cursor position.
      ::ReleaseCapture();
      POINT pt;
      ::GetCursorPos(&pt);
      ::PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                     MAKELPARAM(pt.x, pt.y));
      return 0;
    }
    case WM_DASK_WINDOWCTL: {
      // Phase 9: wparam 0=min 1=max/restore 2=close (from the toolbar).
      const WPARAM cmd = wparam == 0 ? SC_MINIMIZE
                       : wparam == 2 ? SC_CLOSE
                       : (::IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE);
      ::PostMessageW(hwnd, WM_SYSCOMMAND, cmd, 0);
      return 0;
    }
    case WM_DASK_KICK: {
      // Phase 9.6: Chrome_WidgetWin (the CEF child) sometimes keeps a
      // stale surface after minimize/restore. Force a full paint by
      // invalidating both the host and its Chrome child.
      for (auto& [id, host] : g_hosts) {
        (void)id;
        if (!host) continue;
        ::InvalidateRect(host, nullptr, TRUE);
        HWND chrome = ::FindWindowExW(host, nullptr, L"Chrome_WidgetWin_1", nullptr);
        if (!chrome)
          chrome = ::FindWindowExW(host, nullptr, L"Chrome_WidgetWin_0", nullptr);
        if (chrome) {
          RECT rc;
          if (::GetClientRect(chrome, &rc)) {
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            // Nudge by 1px then back: Chromium re-lays out and re-paints the
            // whole surface, repairing the blank after minimize/restore.
            ::SetWindowPos(chrome, nullptr, 0, 0, w, h - 1,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ::SetWindowPos(chrome, nullptr, 0, 0, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
          }
          ::InvalidateRect(chrome, nullptr, TRUE);
        }
      }
      return 0;
    }
    case WM_SIZE: {
      if (wparam != SIZE_MINIMIZED) DoLayout();
      // Phase 9.6: after restore/maximize Chromium's embedded child windows
      // can come back with a broken (partially blank) surface. Nudge every
      // host child by 1px on the next loop pass to force a full re-render.
      if (wparam == SIZE_RESTORED || wparam == SIZE_MAXIMIZED) {
        ::PostMessageW(hwnd, WM_DASK_KICK, 0, 0);
      }
      return 0;
    }
    case WM_CLOSE: {
      // Graceful close: close every browser, wait for all OnBeforeClose, then
      // WM_DASK_ALL_CLOSED destroys the window.
      TabManager::Get()->RequestCloseAll();
      return 0;
    }
    case WM_DASK_RUN_TASK: {
      auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
      if (fn) {
        auto run = std::move(*fn);
        delete fn;
        run();
      }
      return 0;
    }
    case WM_DASK_CREATE_HOST: {
      const int64_t id = static_cast<int64_t>(wparam);
      HWND host = ::CreateWindowExW(
          0, kHostClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0,
          kUiToolbarHeight, 100, 100, hwnd, nullptr,
          reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
          nullptr);
      if (host) g_hosts[id] = host;
      return reinterpret_cast<LRESULT>(host);
    }
    case WM_DASK_DESTROY_HOST: {
      const int64_t id = static_cast<int64_t>(wparam);
      auto it = g_hosts.find(id);
      if (it != g_hosts.end()) {
        ::DestroyWindow(it->second);
        g_hosts.erase(it);
      }
      return 0;
    }
    case WM_DASK_MENU_HOST: {
      // Phase 7: borderless popup owned by the main window hosting menu.html.
      const int menu_w = 300, menu_h = 600;
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      // RTL toolbar: the hamburger sits on the left edge. Convert client
      // coords to screen coords (WS_POPUP uses screen coordinates).
      POINT origin = {rc.left + 8, kUiToolbarHeight};
      ::ClientToScreen(hwnd, &origin);
      HWND popup = ::CreateWindowExW(
          WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuPopupClassName, L"",
          WS_POPUP, origin.x, origin.y, menu_w, menu_h, hwnd, nullptr,
          reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
          nullptr);
      if (popup) {
        // Visible + focused so clicks outside deactivate (and close) it.
        ::ShowWindow(popup, SW_SHOW);
        ::SetForegroundWindow(popup);
      }
      return reinterpret_cast<LRESULT>(popup);
    }
    case WM_DASK_MENU_CLOSED:
      // The popup was deactivated or destroyed: tear down the menu browser.
      ResetMenuWideCache();
      TabManager::Get()->OnMenuDeactivated();
      return 0;
    case WM_DASK_DESTROY_MENU_HOST:
      ResetMenuWideCache();
      // Destroy the popup on the thread that owns it.
      ::EnumWindows([](HWND w, LPARAM) -> BOOL {
        wchar_t cls[64] = {};
        ::GetClassNameW(w, cls, 64);
        if (::lstrcmpW(cls, L"DaskMenuPopup") == 0) {
          ::DestroyWindow(w);
        }
        return TRUE;
      }, 0);
      return 0;
    case WM_DASK_MENU_WIDE: {
      // Phase 7.5: show the hover flyout beside the menu column. The window
      // expands to the right, and is clipped (SetWindowRgn) so ONLY the menu
      // column and the flyout panel are visible — the rest is fully
      // transparent, nothing shows over the page except the panels.
      // lparam packs the flyout rect: LOWORD=top, HIWORD=height.
      const int top = LOWORD(lparam);
      const int fly_h = HIWORD(lparam);
      // Skip redundant work: moving between flyout rows re-sends the same
      // resize/clip; repeating SetWindowPos + SetWindowRgn(bRepaint=TRUE)
      // repaints the whole area every time = visible flicker over the page.
      if (wparam == g_menu_wide && top == g_menu_top && fly_h == g_menu_fly)
        return 0;
      g_menu_wide = static_cast<int>(wparam);
      g_menu_top = top;
      g_menu_fly = fly_h;
      struct WideCtx { HWND main; int wide; int top; int fly_h; };
      WideCtx ctx{hwnd, static_cast<int>(wparam), top, fly_h};
      ::EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<WideCtx*>(lp);
        wchar_t cls[64] = {};
        ::GetClassNameW(w, cls, 64);
        if (::lstrcmpW(cls, L"DaskMenuPopup") == 0 &&
            ::GetWindow(w, GW_OWNER) == c->main) {
          RECT rc;
          ::GetWindowRect(w, &rc);
          if (!c->wide) {
            // Back to the plain 300px menu column, no clipping.
            ::SetWindowPos(w, nullptr, rc.left, rc.top, 300, 600,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            if (HWND chrome = ::FindWindowExW(w, nullptr, L"Chrome_WidgetWin_1",
                                              nullptr)) {
              ::SetWindowPos(chrome, nullptr, 0, 0, 300, 600,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ::SetWindowRgn(w, nullptr, TRUE);
            return TRUE;
          }
          // Window tall enough for the flyout even at the lowest row.
          const int needed = c->fly_h > 0 ? c->top + c->fly_h + 10 : 0;
          const int target_h = needed > 600 ? needed : 600;
          ::SetWindowPos(w, nullptr, rc.left, rc.top, 620, target_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
          if (HWND chrome = ::FindWindowExW(w, nullptr, L"Chrome_WidgetWin_1",
                                            nullptr)) {
            ::SetWindowPos(chrome, nullptr, 0, 0, 620, target_h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
          }
          // Clip to: menu column (0..300, full height) + flyout panel
          // (298..598 at the hovered row — 2px overlap so the mouse never
          // crosses a dead zone).
          HRGN menu_rgn = ::CreateRectRgn(0, 0, 300, target_h);
          if (c->fly_h > 0) {
            const int fly_bottom = c->top + c->fly_h;
            const int bottom = fly_bottom < target_h ? fly_bottom : target_h;
            HRGN fly_rgn = ::CreateRectRgn(298, c->top, 598, bottom);
            ::CombineRgn(menu_rgn, menu_rgn, fly_rgn, RGN_OR);
            ::DeleteObject(fly_rgn);
          }
          ::SetWindowRgn(w, menu_rgn, TRUE);
          // SetWindowRgn takes ownership of the region on success.
        }
        return TRUE;
      }, reinterpret_cast<LPARAM>(&ctx));
      return 0;
    }
    case WM_DASK_LAYOUT: {
      ShowActiveTab();
      DoLayout();
      return 0;
    }
    case WM_DASK_ALL_CLOSED: {
      // Stop the DB thread before CefShutdown so SQLite closes cleanly.
      TabManager::Get()->StopDatabase();
      ::DestroyWindow(hwnd);
      return 0;
    }
    case WM_DESTROY: {
      if (g_kbd_hook) {
        ::UnhookWindowsHookEx(g_kbd_hook);
        g_kbd_hook = nullptr;
      }
      g_main_hwnd = nullptr;
      ::PostQuitMessage(0);
      return 0;
    }
    default:
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

}  // namespace

int RunBrowserWindow(HINSTANCE h_instance, int n_cmd_show) {
  const std::wstring exe_dir = ExeDirectory();

  // Phase 7: "new private window" opens a second process with --private:
  // every tab is private (no session restore, no history, no permissions
  // persisted) inside a genuinely separate top-level window.
  const wchar_t* cmdline = ::GetCommandLineW();
  if (cmdline && std::wstring(cmdline).find(L"--private") != std::wstring::npos) {
    TabManager::SetForcePrivate(true);
  }

  // Phase 8: --app <url> opens a chromeless app window (the installed
  // "page as app" shortcuts use it); a bare http(s) URL argument opens the
  // normal window directly on that URL (the "create shortcut" launchers).
  bool launch_app = false;
  std::wstring launch_url;
  {
    int argc = 0;
    LPWSTR* argv =
        ::CommandLineToArgvW(cmdline ? cmdline : L"", &argc);
    if (argv) {
      for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--app" && i + 1 < argc) {
          launch_app = true;
          launch_url = argv[++i];
        } else if (arg.rfind(L"http://", 0) == 0 ||
                   arg.rfind(L"https://", 0) == 0) {
          launch_url = arg;
        }
      }
      ::LocalFree(argv);
    }
  }
  TabManager::SetLaunch(launch_app, launch_url);

  if (!CefRuntimeFilesPresent(exe_dir)) {
    ShowFatalError(
        L"DASK Browser",
        L"CEF runtime files are missing next to dask_browser.exe "
        L"(libcef.dll / icudtl.dat / resources.pak).\n\n"
        L"Run:  powershell -ExecutionPolicy Bypass -File "
        L"scripts/download_cef.ps1\nthen rebuild so CMake copies the CEF files.");
    return 1;
  }

  // Phase 7.5: DNS-over-HTTPS provider (read before CefInitialize so the
  // command-line switch is applied on this launch).
  SetDoHResolver(TabManager::ReadPrefString("dnsResolver"));
  // CEF configuration. Sandbox stays enabled.
  CefSettings settings;
  CefString(&settings.browser_subprocess_path)
      .FromWString(exe_dir + L"\\dask_browser_subprocess.exe");
  CefString(&settings.resources_dir_path).FromWString(exe_dir);
  CefString(&settings.locales_dir_path).FromWString(exe_dir + L"\\locales");
  // Phase 9.6: persistent HTTP cache — pages/JS/images served from disk on
  // repeat visits (huge perceived speed-up).
  // Phase 8 constraint: CEF 120+ enforces a process singleton lock on
  // root_cache_path (an empty value falls back to the SHARED default dir,
  // so a second DASK window process would fail CefInitialize — the
  // "CefInitialize() failed." dialog). cache_path must also be a CHILD of
  // root_cache_path or CEF silently falls back to in-memory storage.
  // Resolution: the FIRST DASK window process claims the stable
  // "cef_root\main" root (named mutex, held for the process lifetime) and
  // keeps the persistent profile cache. Every additional process (new
  // window, --app, --private) gets a per-pid root with an ephemeral cache
  // under it; --private is additionally wiped on launch.
  {
    wchar_t appdata[MAX_PATH] = {};
    std::wstring base;
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                     appdata))) {
      base = std::wstring(appdata) + L"\\DaskBrowser";
    }
    if (!base.empty()) {
      const std::wstring pid_root = base + L"\\cef_root\\p" +
                                    std::to_wstring(::GetCurrentProcessId());
      static HANDLE g_main_root_mutex = nullptr;  // held for process lifetime.
      g_main_root_mutex = ::CreateMutexW(nullptr, TRUE,
                                         L"Local\\DaskBrowser.MainRoot");
      const bool owns_main_root =
          g_main_root_mutex && ::GetLastError() != ERROR_ALREADY_EXISTS;
      if (TabManager::force_private()) {
        const std::wstring pc = pid_root + L"\\cache";
        // Private launch: drop any leftover cache before CEF opens it.
        SHFILEOPSTRUCTW op = {};
        std::wstring from = pc;
        from.push_back(L'\0');
        op.hwnd = nullptr;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        ::SHFileOperationW(&op);
        CefString(&settings.root_cache_path).FromWString(pid_root);
        CefString(&settings.cache_path).FromWString(pc);
      } else if (owns_main_root) {
        // First window process: stable root + persistent profile cache
        // (cache_path must live UNDER root_cache_path or CEF rejects it).
        CefString(&settings.root_cache_path)
            .FromWString(base + L"\\cef_root\\main");
        CefString(&settings.cache_path)
            .FromWString(base + L"\\cef_root\\main\\profile_cache");
        // Per-profile request contexts cache under the same stable root.
        SetCefCacheBase(base + L"\\cef_root\\main");
        ::SHCreateDirectoryExW(
            nullptr,
            (base + L"\\cef_root\\main\\profile_cache").c_str(), nullptr);
      } else {
        // Extra process: per-pid root, ephemeral cache under it.
        CefString(&settings.root_cache_path).FromWString(pid_root);
        CefString(&settings.cache_path).FromWString(pid_root + L"\\cache");
        SetCefCacheBase(pid_root);
      }
    }
  }
  settings.multi_threaded_message_loop = true;  // CEF runs its own loop thread.
  // Phase 6: diagnostic logging is OFF by default (no personal data is ever
  // written). Enable only for troubleshooting with DASK_LOG=1: CEF's own
  // log goes to <exe dir>\debug.log (rotated by CEF; inspect locally only).
  wchar_t log_env[8] = {};
  if (::GetEnvironmentVariableW(L"DASK_LOG", log_env, 8) && log_env[0] == L'1') {
    CefString(&settings.log_file).FromWString(exe_dir + L"\\debug.log");
    settings.log_severity = LOGSEVERITY_WARNING;
    // Debug only. A second DASK instance must NOT fail CefInitialize when
    // the first one already owns the port: probe before enabling DevTools.
    WSADATA wsa = {};
    bool port_free = false;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
      SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (probe != INVALID_SOCKET) {
        SOCKADDR_IN addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = ::htons(9222);
        port_free =
            ::bind(probe, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == 0;
        ::closesocket(probe);
      }
    }
    if (port_free) settings.remote_debugging_port = 9222;
  } else {
    settings.log_severity = LOGSEVERITY_DISABLE;
  }
  // Phase 8: dark native UI (context menus, dialogs) unless the light
  // theme is chosen. Applied through DaskApp::OnBeforeCommandLineProcessing
  // (the documented place to adjust switches before CefInitialize).
  SetForceDarkUI(!TabManager::ReadPrefBool("lightTheme"));
  // Arabic Chromium UI (context-menu accelerators, print dialogs, …).
  CefString(&settings.locale).FromString("ar");
  CefMainArgs main_args(h_instance);
  // The browser process installs the http://dask.ui UI handler.
  CefRefPtr<CefApp> app = CreateDaskApp(exe_dir + L"\\ui");
  if (!CefInitialize(main_args, settings, app, nullptr)) {
    // One-line local diagnostic (no personal data): which debug flags were
    // active when initialization failed. Overwritten on every failure.
    const int exit_code = CefGetExitCode();
    std::ofstream why(exe_dir + L"\\init_fail.txt", std::ios::binary);
    if (why) {
      why << "CefInitialize failed; DASK_LOG='" << NarrowUtf8(log_env)
          << "'; app_mode=" << (launch_app ? 1 : 0)
          << "; locale=ar; force-dark="
          << (TabManager::ReadPrefBool("lightTheme") ? 0 : 1)
          << "; launch_url_len=" << launch_url.size()
          << "; exit_code=" << exit_code << std::endl;
    }
    ShowFatalError(L"DASK Browser", L"CefInitialize() failed.");
    return 1;
  }

  // Host window.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = h_instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // App icon (title bar + taskbar + desktop shortcut) from the embedded
  // resource (DASK_Browser_Icon_Pack/01_Normal_Light).
  wc.hIcon = ::LoadIconW(h_instance, MAKEINTRESOURCEW(1));
  if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  wc.hbrBackground = ::CreateSolidBrush(RGB(20, 21, 25));
  wc.lpszClassName = kWindowClassName;
  if (!::RegisterClassExW(&wc)) {
    ShowFatalError(L"DASK Browser", L"RegisterClassExW() failed.");
    CefShutdown();
    return 1;
  }
  // Host windows live inside the main window.
  WNDCLASSEXW hc = wc;
  hc.lpfnWndProc = ::DefWindowProcW;
  hc.hbrBackground = ::CreateSolidBrush(RGB(20, 21, 25));
  hc.lpszClassName = kHostClassName;
  ::RegisterClassExW(&hc);

  // Phase 7: dedicated class for the menu popup (closes on deactivation).
  WNDCLASSEXW mc = wc;
  mc.lpfnWndProc = MenuPopupWndProc;
  mc.hbrBackground = ::CreateSolidBrush(RGB(20, 21, 25));
  mc.lpszClassName = kMenuPopupClassName;
  ::RegisterClassExW(&mc);

  RECT frame = {0, 0, kInitialWidth, kInitialHeight};
  ::AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = ::CreateWindowExW(
      0, kWindowClassName, L"DASK Browser", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
      frame.bottom - frame.top, nullptr, nullptr, h_instance, nullptr);
  if (!hwnd) {
    ShowFatalError(L"DASK Browser", L"CreateWindowExW() failed.");
    CefShutdown();
    return 1;
  }
  g_main_hwnd = hwnd;

  TabManager::Get()->Initialize(hwnd);

  // Global shortcuts (active only while DASK is focused).
  g_kbd_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, h_instance, 0);

  ::ShowWindow(hwnd, n_cmd_show);
  ::UpdateWindow(hwnd);
  // Phase 9: re-apply the custom frame (WM_NCCALCSIZE returns 0 -> the tab
  // strip IS the title bar). Creation-time NCCALCSIZE can be cached by DWM,
  // so force one frame recomputation + layout after showing the window.
  ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
  DoLayout();

  // Create the UI browser + restore the last session.
  TabManager::Get()->Start();

  // Standard Win32 loop; CEF runs on its own thread (MTML).
  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  // All browsers closed (OnBeforeClose for every one ran) -> safe per CEF docs.
  CefShutdown();
  return static_cast<int>(msg.wParam);
}

}  // namespace dask
