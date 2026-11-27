// DASK Browser - native Win32 window hosting the UI browser and all tabs.
// Layout and host-window bookkeeping live on this thread; all browser logic
// lives in TabManager on the CEF UI thread (see tab_manager.h).
#include "browser_window.h"

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "dask_app.h"
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
  const int content_y = kUiToolbarHeight;
  const int content_h = height - kUiToolbarHeight;

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
    case WM_SIZE: {
      if (wparam != SIZE_MINIMIZED) DoLayout();
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
      TabManager::Get()->OnMenuDeactivated();
      return 0;
    case WM_DASK_DESTROY_MENU_HOST:
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
  // cache_path left empty: no profile data is stored (no passwords, history).
  settings.multi_threaded_message_loop = true;  // CEF runs its own loop thread.
  // Phase 6: diagnostic logging is OFF by default (no personal data is ever
  // written). Enable only for troubleshooting with DASK_LOG=1: CEF's own
  // log goes to <exe dir>\debug.log (rotated by CEF; inspect locally only).
  wchar_t log_env[8] = {};
  if (::GetEnvironmentVariableW(L"DASK_LOG", log_env, 8) && log_env[0] == L'1') {
    CefString(&settings.log_file).FromWString(exe_dir + L"\\debug.log");
    settings.log_severity = LOGSEVERITY_WARNING;
    settings.remote_debugging_port = 9222;  // for perf probes (debug only)
  } else {
    settings.log_severity = LOGSEVERITY_DISABLE;
  }
  CefMainArgs main_args(h_instance);
  // The browser process installs the http://dask.ui UI handler.
  CefRefPtr<CefApp> app = CreateDaskApp(exe_dir + L"\\ui");
  if (!CefInitialize(main_args, settings, app, nullptr)) {
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
