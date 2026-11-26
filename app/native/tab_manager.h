// DASK Browser - tab manager: owns the UI browser, all content tabs, the UI
// bridge (CefMessageRouter), state snapshots and session persistence.
//
// Threading model:
//   * The CEF UI thread (TID_UI, spawned by multi_threaded_message_loop)
//     owns all logical tab metadata and every CEF browser operation.
//   * The Win32 thread owns the native window tree. Structural work (creating
//     and destroying tab host windows, layout) is marshaled to it through
//     PostMessage; it never calls CEF APIs directly.
//   * No thread ever blocks waiting on the other.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/wrapper/cef_message_router.h"

namespace dask {

// Custom messages used by TabManager <-> browser_window marshaling.
constexpr UINT WM_DASK_RUN_TASK = WM_APP + 0x110;     // lParam: std::function<void()>*
constexpr UINT WM_DASK_CREATE_HOST = WM_APP + 0x111;  // wParam: tab id, returns HWND
constexpr UINT WM_DASK_ALL_CLOSED = WM_APP + 0x112;   // every browser closed
constexpr UINT WM_DASK_LAYOUT = WM_APP + 0x113;       // recompute tab layout
constexpr UINT WM_DASK_DESTROY_HOST = WM_APP + 0x114; // wParam: tab id

// Height of the React toolbar region (tab strip + nav bar), CSS px.
constexpr int kUiToolbarHeight = 84;

constexpr wchar_t kNewTabUrl[] = L"http://dask.ui/newtab.html";

struct QuickLink {
  std::wstring title;
  std::wstring url;
};

struct SearchEngineConfig {
  std::wstring id = L"google";   // google | bing | duckduckgo | custom
  std::wstring custom;           // %s format string for the custom engine
};

// Runs |fn| on the Win32 thread via the main window's message queue.
void PostToWin(HWND main_hwnd, std::function<void()> fn);

// Runs |fn| on the CEF UI thread.
void PostToCef(std::function<void()> fn);

class TabManager {
 public:
  static TabManager* Get();

  // Called on the Win32 thread before any browser is created.
  void Initialize(HWND main_hwnd);

  // Creates the UI browser and restores the persisted session.
  // Called on the Win32 thread after CefInitialize().
  void Start();

  // Close-all request used on app shutdown. Thread-safe.
  void RequestCloseAll();

  // ---- Operations (thread-safe; marshaled to the CEF UI thread) ----
  void CreateTab(std::wstring url);
  void CloseTab(int64_t tab_id);
  void ActivateTab(int64_t tab_id);
  void NavigateActive(const std::wstring& input);  // address bar / search text
  void NavigateSelf(CefRefPtr<CefBrowser> browser, const std::wstring& input);
  void Back(); void Forward(); void Reload(); void Stop(); void Home();
  void FocusAddress();  // Ctrl+L: focus UI browser + notify the React toolbar.
  void CycleTab(int direction);  // Ctrl+Tab / Ctrl+Shift+Tab.

  // ---- Called by TabClient on the CEF UI thread ----
  void OnBrowserCreated(int64_t tab_id, CefRefPtr<CefBrowser> browser);
  void OnBrowserClosed(int64_t tab_id);
  void OnTitleChanged(int64_t tab_id, const std::wstring& title);
  void OnAddressChanged(int64_t tab_id, const std::wstring& url);
  void OnLoadingChanged(int64_t tab_id, bool loading);

  // ---- Wiring for TabClient (CEF UI thread) ----
  CefRefPtr<CefMessageRouterBrowserSide> router() const { return router_; }
  int64_t NewTabId() { return next_tab_id_++; }

  // ---- Layout helpers (Win32 thread) ----
  // Reads the current logical order/active id under a lock for positioning.
  void GetLayout(std::vector<int64_t>& order, int64_t& active_id);

 private:
  TabManager() = default;

  struct Tab {
    int64_t id = 0;
    CefRefPtr<CefBrowser> browser;
    bool internal = false;  // trusted UI tab
    bool closing = false;
    std::wstring title;
    std::wstring url;
    bool loading = false;
  };

  Tab* FindTab(int64_t id);
  Tab* FindTabByBrowser(CefRefPtr<CefBrowser> browser);
  void SetActive(Tab* tab);                     // CEF UI thread
  std::wstring ResolveInput(const std::wstring& input);
  void PushUiEvent(bool focus_address = false); // CEF UI thread
  std::string BuildStateJson(bool focus_address);
  void SaveSession();                           // CEF UI thread
  void RequestLayout();                         // post WM_DASK_LAYOUT

  HWND main_hwnd_ = nullptr;

  // Logical state - CEF UI thread only.
  std::vector<Tab> tabs_;
  CefRefPtr<CefBrowser> ui_browser_;
  int64_t active_tab_id_ = 0;
  int64_t next_tab_id_ = 1;
  SearchEngineConfig engine_;
  std::vector<QuickLink> quick_links_;
  bool closing_all_ = false;
  bool state_dirty_ = true;  // snapshot not yet delivered to the UI
  int outstanding_closes_ = 0;

  // Layout mirror for the Win32 thread (guarded by layout_mutex_).
  CRITICAL_SECTION layout_lock_;
  std::vector<int64_t> layout_order_;
  int64_t layout_active_ = 0;

  CefRefPtr<CefMessageRouterBrowserSide> router_;
  // All pages with a pending "wait" query get every state event.
  std::map<int64_t, CefRefPtr<CefMessageRouterBrowserSide::Callback>> waiters_;
  bool queued_focus_address_ = false;

  friend class DaskQueryHandler;
  friend class TabClient;
};

}  // namespace dask
