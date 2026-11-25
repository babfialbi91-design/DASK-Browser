// DASK Browser - native Win32 window hosting a CEF browser.
#include "browser_window.h"

#include <windows.h>

#include <string>

#include "dask_app.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"

namespace dask {
namespace {

constexpr wchar_t kWindowClassName[] = L"DaskBrowserMainWindow";
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 800;

// CEF runtime files that must sit next to the executable.
bool CefRuntimeFilesPresent(const std::wstring& exe_dir) {
  std::wstring checks[] = {exe_dir + L"\\libcef.dll", exe_dir + L"\\icudtl.dat",
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

void ShowFatalError(HWND owner, const std::wstring& title,
                    const std::wstring& message) {
  ::MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

// Client implementation: wires CEF lifecycle / display / load events to the
// hosting Win32 window. Phase 1 keeps this minimal but real (no stubs).
class DaskClient : public CefClient, public CefLifeSpanHandler,
                   public CefDisplayHandler, public CefLoadHandler {
 public:
  explicit DaskClient(HWND host_window) : host_window_(host_window) {}

  // CefClient
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

  // CefLifeSpanHandler
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    // The browser is gone; tear down the host window. This is the documented
    // safe place to leave the app loop before CefShutdown() is called.
    browser_.reset();
    ::PostMessageW(host_window_, WM_DASK_BROWSER_CLOSED, 0, 0);
  }

  // CefDisplayHandler
  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override {
    ::SetWindowTextW(host_window_,
                     (L"DASK Browser - " + std::wstring(title)).c_str());
  }

  // CefLoadHandler
  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode, const CefString& errorText,
                   const CefString& failedUrl) override {
    if (errorCode == ERR_ABORTED) return;  // User navigation, not a failure.
    std::wstring msg = L"Load error (" + std::to_wstring(errorCode) +
                       L"): " + errorText.ToWString() + L"\nURL: " +
                       failedUrl.ToWString();
    ::OutputDebugStringW(msg.c_str());
    ::SetWindowTextW(host_window_,
                     (L"DASK Browser - " + msg).c_str());
  }

  CefRefPtr<CefBrowser> browser() const { return browser_; }
  void set_browser(CefRefPtr<CefBrowser> browser) { browser_ = browser; }

  static constexpr UINT WM_DASK_BROWSER_CLOSED =
      WM_APP + 0x4B41;  // "dask" marker for the custom close message.

 private:
  IMPLEMENT_REFCOUNTING(DaskClient);
  DISALLOW_COPY_AND_ASSIGN(DaskClient);

  HWND host_window_;
  CefRefPtr<CefBrowser> browser_;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* client =
      reinterpret_cast<DaskClient*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_SIZE: {
      if (client && client->browser()) {
        // Windowed CEF renders into a child Chromium window; keep it in sync.
        RECT rc;
        if (::GetClientRect(hwnd, &rc)) {
          HWND chrome = ::FindWindowExW(hwnd, nullptr, L"Chrome_WidgetWin_0",
                                        nullptr);
          if (chrome)
            ::SetWindowPos(chrome, nullptr, 0, 0, rc.right - rc.left,
                           rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      return 0;
    }
    case WM_CLOSE: {
      if (client && client->browser()) {
        // Start the documented close sequence; WM_DASK_BROWSER_CLOSED in
        // OnBeforeClose destroys the window afterwards.
        client->browser()->GetHost()->CloseBrowser(true);
        return 0;
      }
      ::DestroyWindow(hwnd);
      return 0;
    }
    case DaskClient::WM_DASK_BROWSER_CLOSED:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

}  // namespace

int RunBrowserWindow(HINSTANCE h_instance, int n_cmd_show) {
  const std::wstring exe_dir = ExeDirectory();

  if (!CefRuntimeFilesPresent(exe_dir)) {
    ShowFatalError(nullptr, L"DASK Browser",
                   L"CEF runtime files are missing next to dask_browser.exe "
                   L"(libcef.dll / icudtl.dat / resources.pak).\n\n"
                   L"Run:  powershell -ExecutionPolicy Bypass -File "
                   L"scripts/download_cef.ps1\n"
                   L"then rebuild so CMake copies the CEF files.");
    return 1;
  }

  // CEF configuration. Sandbox stays enabled.
  CefSettings settings;
  CefString(&settings.browser_subprocess_path)
      .FromWString(exe_dir + L"\\dask_browser_subprocess.exe");
  CefString(&settings.resources_dir_path).FromWString(exe_dir);
  CefString(&settings.locales_dir_path).FromWString(exe_dir + L"\\locales");
  // cache_path left empty for Phase 1: fully in-memory profile, no disk state.
  settings.multi_threaded_message_loop = true;  // CEF runs its own loop thread.

  CefMainArgs main_args(h_instance);
  CefRefPtr<CefApp> app = CreateDaskApp();
  if (!CefInitialize(main_args, settings, app, nullptr)) {
    ShowFatalError(nullptr, L"DASK Browser", L"CefInitialize() failed.");
    return 1;
  }

  // Host window.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = h_instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  wc.hbrBackground = ::CreateSolidBrush(RGB(30, 30, 30));
  wc.lpszClassName = kWindowClassName;
  if (!::RegisterClassExW(&wc)) {
    ShowFatalError(nullptr, L"DASK Browser", L"RegisterClassExW() failed.");
    CefShutdown();
    return 1;
  }

  RECT frame = {0, 0, kInitialWidth, kInitialHeight};
  ::AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = ::CreateWindowExW(
      0, kWindowClassName, L"DASK Browser", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
      frame.bottom - frame.top, nullptr, nullptr, h_instance, nullptr);
  if (!hwnd) {
    ShowFatalError(nullptr, L"DASK Browser", L"CreateWindowExW() failed.");
    CefShutdown();
    return 1;
  }
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // Client attached below.

  // Browser windowed into the host window.
  auto client = CefRefPtr<DaskClient>(new DaskClient(hwnd));

  CefWindowInfo window_info;
  RECT rc;
  ::GetClientRect(hwnd, &rc);
  window_info.SetAsChild(hwnd, CefRect(0, 0, rc.right - rc.left,
                                       rc.bottom - rc.top));

  CefBrowserSettings browser_settings;

  // Safe local test page shipped with the app (file:// on our own file).
  CefString url;
  url.FromWString(L"file:///" + exe_dir + L"\\test_page.html");

  // Signature: (window_info, client, url, settings, extra_info, request_context).
  CefBrowserHost::CreateBrowser(window_info, client, url, browser_settings,
                                nullptr, nullptr);

  // Remember the client on the window once the browser exists so WndProc can
  // forward events. CreateBrowser is async; hook the client immediately for
  // close handling (CreateBrowser already registered the client internally).
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(client.get()));

  ::ShowWindow(hwnd, n_cmd_show);
  ::UpdateWindow(hwnd);

  // Standard Win32 loop; CEF runs on its own thread (MTML).
  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  // Browser already closed (OnBeforeClose ran) -> safe per CEF docs.
  CefShutdown();
  return static_cast<int>(msg.wParam);
}

}  // namespace dask
