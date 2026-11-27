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

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adblock/filter_engine.h"
#include "filter_lists.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_download_handler.h"
#include "include/cef_find_handler.h"
#include "include/cef_permission_handler.h"
#include "include/wrapper/cef_message_router.h"
#include "privacy_settings.h"
#include "profiles.h"
#include "storage/database.h"
#include "storage/stores.h"

namespace dask {

// Custom messages used by TabManager <-> browser_window marshaling.
constexpr UINT WM_DASK_RUN_TASK = WM_APP + 0x110;     // lParam: std::function<void()>*
constexpr UINT WM_DASK_CREATE_HOST = WM_APP + 0x111;  // wParam: tab id, returns HWND
constexpr UINT WM_DASK_ALL_CLOSED = WM_APP + 0x112;   // every browser closed
constexpr UINT WM_DASK_LAYOUT = WM_APP + 0x113;       // recompute tab layout
constexpr UINT WM_DASK_DESTROY_HOST = WM_APP + 0x114; // wParam: tab id
constexpr UINT WM_DASK_MENU_HOST = WM_APP + 0x115;   // Phase 7: create menu popup host, returns HWND
constexpr UINT WM_DASK_MENU_CLOSED = WM_APP + 0x116; // Phase 7: menu popup deactivated/closed
constexpr UINT WM_DASK_DESTROY_MENU_HOST = WM_APP + 0x117; // Phase 7: destroy popup on its owning thread
constexpr UINT WM_DASK_MENU_WIDE = WM_APP + 0x118; // Phase 7.5: flyout clip (wparam=wide, lparam= top | height<<16)

// Height of the React toolbar region (tab strip + nav bar), CSS px.
constexpr int kUiToolbarHeight = 84;

constexpr wchar_t kNewTabUrl[] = L"http://dask.ui/newtab.html";

struct QuickLink {
  std::wstring title;
  std::wstring url;
};

// UI-facing mirrors of DB rows (CEF UI thread copies, updated by the DB
// thread through PostToCef).
struct BookmarkUi {
  int64_t id = 0;
  std::wstring title;
  std::wstring url;
};
struct DownloadUi {
  int64_t id = 0;
  std::wstring url;
  std::wstring path;
  std::wstring filename;
  int state = 0;
  int64_t received = 0;
  int64_t total = -1;
  bool risky = false;  // executable/script type (Phase 5)
};
struct PermissionUi {  // stored per-site decision (profile DB)
  std::wstring host;
  std::wstring type;
  int decision = 0;  // 0 ask / 1 allow / 2 deny
};
// Live CEF permission request waiting for the user (Phase 5).
struct PendingPermission {
  uint64_t cef_id = 0;   // CEF prompt id (0 for media requests)
  uint64_t ui_id = 0;    // our queue id used by the bridge response
  std::wstring origin;
  std::vector<std::wstring> labels;    // Arabic labels of requested types
  std::vector<std::string> db_keys;    // permission keys to store on "always"
  uint32_t media_bits = 0;             // media request bits (media flow only)
  bool media = false;                  // CefMediaAccessCallback vs prompt cb
  bool for_private = false;            // private tab: never persist decisions
  CefRefPtr<CefMediaAccessCallback> media_cb;
  CefRefPtr<CefPermissionPromptCallback> prompt_cb;
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
  void CreateTab(std::wstring url, bool private_tab = false);
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

  // ---- Downloads (called by TabClient on the browser UI thread) ----
  bool OnBeforeDownload(int64_t tab_id, CefRefPtr<CefDownloadItem> item,
                        const std::wstring& suggested_name,
                        CefRefPtr<CefBeforeDownloadCallback> callback);
  void OnDownloadUpdated(int64_t tab_id, CefRefPtr<CefDownloadItem> item,
                         CefRefPtr<CefDownloadItemCallback> callback);
  void CancelDownload(int64_t db_id);   // CEF UI thread
  void OpenDownload(int64_t db_id);     // CEF UI thread
  void OpenDownloadsFolder();           // CEF UI thread

  // ---- Storage (Phase 4): posts work to the dedicated DB thread ----
  void StartDatabase();                 // called on the CEF UI thread
  void StopDatabase();                  // called before CefShutdown
  void RecordVisit(int64_t tab_id, const std::wstring& url,
                   const std::wstring& title);
  void ToggleBookmark(const std::wstring& title, const std::wstring& url);
  void RemoveBookmark(int64_t id);
  void UpdateBookmark(int64_t id, const std::wstring& title,
                      const std::wstring& url);
  void ClearHistory(int days);          // 0 = everything
  void ToggleBookmarkActive();          // Ctrl+D: star the active tab
  void SetHistoryEnabled(bool enabled);
  void SetRetentionDays(int days);
  void SetRestoreSession(bool enabled);
  void SetLightTheme(bool enabled);
  void SetDnsResolver(const std::string& resolver);  // Phase 7.5 (applies next launch)
  void SetListEnabled(const std::string& name, bool enabled);  // Phase 7.5
  void ViewSourceActive();  // Phase 7.5: Ctrl+U / menu
  const std::string& dns_resolver() const { return dns_resolver_; }

  // ---- Main menu commands (Phase 7) ----
  static void SetForcePrivate(bool on) { force_private_ = on; }
  // Reads one boolean/string preference synchronously (pre-CEF or pre-DB
  // thread). Returns "" when unset.
  static std::string ReadPrefString(const std::string& key);
  static bool force_private() { return force_private_; }
  void NewBrowserWindow(bool private_window);  // second process (real window)
  void AdjustZoom(double step);  // +/- zoom on the active page; 0 = reset
  void ToggleFullscreen();       // native borderless fullscreen of the window
  void FindInPage(const std::string& text, bool forward, bool find_next);
  void StopFind();
  void OnFindResult(int count, int active);  // CefFindHandler callback
  void OpenFindBar();  // Ctrl+F: reveal the find bar in the UI
  void ShowDevTools();
  void SavePageAsPdf();          // PrintToPDF into the Downloads folder
  void PrintPage();              // window.print() in the active frame
  void CopyPageLink();           // active tab URL to the clipboard
  void ExitApp();                // safe close: WM_CLOSE on the main window
  void ShowAppMenu();            // Phase 7: open the popup menu window
  void CloseAppMenu();           // Phase 7: close it (deactivate/toggle)
  void ToggleAppMenu();          // Phase 7: hamburger button
  void OnMenuDeactivated();      // Phase 7: popup lost activation (Win32 thread)
  void ResizeMenuPopup(bool wide, int top = 0, int height = 0);  // Phase 7.5
  void ShowAppMenuInternal();    // CEF UI thread
  void CloseAppMenuInternal();   // CEF UI thread
  void OnMenuBrowserCreated(CefRefPtr<CefBrowser> browser);
  void OnMenuBrowserClosed();    // MenuClient::OnBeforeClose
  bool fullscreen() const { return fullscreen_; }

  // ---- Permissions & profiles (Phase 5) ----
  void OnMediaAccessPermission(int64_t tab_id, const std::wstring& origin,
                               uint32_t requested,
                               CefRefPtr<CefMediaAccessCallback> callback);
  bool OnPermissionPrompt(int64_t tab_id, uint64_t prompt_id,
                          const std::wstring& origin, uint32_t requested,
                          CefRefPtr<CefPermissionPromptCallback> callback);
  void DismissPermissionPrompt(uint64_t prompt_id);
  // action: allowOnce / allow / denyOnce / deny
  void RespondPermission(uint64_t id, const std::string& action);
  void SetPermission(const std::string& host, const std::string& type,
                     int decision);  // 0 ask / 1 allow / 2 deny
  void ResetPermission(const std::string& host, const std::string& type);
  ProfileStore* profiles() { return profile_store_.get(); }
  void AddProfile(const std::string& name);
  void SwitchProfile(const std::string& id);
  void DeleteProfile(const std::string& id);
  void PerformProfileSwitch();  // CEF UI thread, after the last tab closes
  void QueuePermissionPrompt(
      uint64_t cef_id, const std::wstring& origin,
      const std::vector<std::wstring>& labels,
      const std::vector<std::string>& db_keys, uint32_t media_bits,
      bool media, bool for_private,
      CefRefPtr<CefMediaAccessCallback> media_cb,
      CefRefPtr<CefPermissionPromptCallback> prompt_cb);

  // Internal helpers (used by TabClient/DaskQueryHandler via friendship).
  void PostToDb(std::function<void()> fn);
  void RefreshStorageCachesOnDb();  // DB thread only

  // ---- Called by TabClient on the CEF IO thread (Phase 3) ----
  // Returns true when the request must be blocked. |first_party| is the
  // top-level site host of the tab.
  bool ShouldBlockRequest(const std::wstring& url,
                          const std::wstring& first_party,
                          adblock::ResourceKind kind, std::string* rule);
  void OnRequestBlocked(int64_t tab_id);
  // Cookies: third-party cookie send/save policy.
  bool CookieAllowed(const std::wstring& request_host,
                     const std::wstring& first_party) const;
  // Clear persisted site data (cookies) for one host or everything.
  void ClearCookiesFor(const std::wstring& site);   // CEF UI thread
  void ClearAllCookies();                           // CEF UI thread
  void UpdateFilterLists();                         // CEF UI thread
  // Site the user is browsing (top-level host of the tab, CEF UI thread).
  std::wstring TabSiteHost(int64_t tab_id);
  // Current tab title (CEF UI thread; used when recording history).
  std::wstring TabTitle(int64_t tab_id);

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
    bool private_flag = false;  // private tab: no history, isolated storage
    bool closing = false;
    bool hidden = false;  // window visibility mirror (Phase 6 bookkeeping)
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

  // ---- Privacy / ad blocking (Phase 3) ----
  PrivacySettings privacy_;
  FilterLists lists_;

  // ---- Profiles (Phase 5): isolated directories per user profile ----
  std::unique_ptr<ProfileStore> profile_store_;
  CefRefPtr<CefRequestContext> profile_context_;  // active profile's context
  bool switching_profile_ = false;
  std::string pending_profile_switch_;
  // Blocked-request counters (session-local), shared between the CEF IO
  // thread (writes) and the CEF UI thread (reads for snapshots).
  std::mutex blocked_mutex_;
  std::map<int64_t, int> tab_blocked_;   // tab id -> count (per page load)
  std::map<std::wstring, int> site_blocked_;  // site host -> session count
  int blocked_total_ = 0;
  // tab id -> top-level site host ("" for internal pages); written on the
  // CEF UI thread, read on the IO thread for request filtering.
  std::map<int64_t, std::wstring> tab_site_;

  // ---- Storage (Phase 4) ----
  // Dedicated DB thread: all SQLite work happens here.
  std::thread db_thread_;
  std::mutex db_mutex_;
  std::condition_variable db_cv_;
  std::deque<std::function<void()>> db_queue_;
  bool db_quit_ = false;

  // DB-thread only objects (valid between StartDatabase and StopDatabase).
  std::unique_ptr<storage::Database> db_;
  std::unique_ptr<storage::Bookmarks> bookmarks_store_;
  std::unique_ptr<storage::History> history_store_;
  std::unique_ptr<storage::Downloads> downloads_store_;
  std::unique_ptr<storage::Settings> settings_store_;
  std::unique_ptr<storage::Permissions> permissions_store_;

  // Read models mirrored to the CEF UI thread (guarded by state_mutex_).
  std::mutex state_mutex_;
  std::vector<BookmarkUi> bookmarks_cache_;
  std::vector<DownloadUi> downloads_cache_;
  std::vector<PermissionUi> permissions_cache_;
  std::vector<PendingPermission> permission_prompts_;
  uint64_t prompt_seq_ = 1;
  std::wstring db_note_;  // human-readable storage problem, if any

  // Preferences (CEF UI thread copies; persisted in settings table).
  bool restore_session_ = false;
  bool history_enabled_ = true;
  int retention_days_ = 90;
  bool fullscreen_ = false;   // Phase 7: borderless fullscreen state
  CefRefPtr<CefBrowser> menu_browser_;  // Phase 7: popup menu browser
  HWND menu_popup_hwnd_ = nullptr;      // Phase 7: popup host window (Win32)
  bool menu_open_ = false;              // Phase 7: menu popup visible
  DWORD menu_deactivated_at_ = 0;       // Phase 7: hamburger-click debounce
  double zoom_level_ = 0.0;   // Phase 7: active page zoom (Chromium units)
  bool find_active_ = false;  // Phase 7: find-in-page bar open
  int find_count_ = 0;        // last find result count
  int find_active_match_ = 0; // 1-based active match index
  static bool force_private_; // Phase 7: --private second-window process
  bool light_theme_ = false;  // Phase 7: UI theme (false = dark, default)
  std::string dns_resolver_;  // Phase 7.5: DoH provider (empty = system default)

  // Active downloads: CEF id -> callback used for cancel (CEF UI thread).
  std::map<uint32_t, CefRefPtr<CefDownloadItemCallback>> active_downloads_;
  // CEF id -> our DB row id (CEF UI thread; private downloads use id 0).
  std::map<uint32_t, int64_t> download_ids_;
  // DB row id -> CEF id (for cancel lookup).
  std::map<int64_t, uint32_t> download_db_to_cef_;
  // Shared in-memory request context for private tabs (cookie/cache
  // isolation). Created lazily on the CEF UI thread.
  CefRefPtr<CefRequestContext> private_context_;

  friend class DaskQueryHandler;
  friend class TabClient;
};

}  // namespace dask
