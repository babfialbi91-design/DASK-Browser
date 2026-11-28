// DASK Browser - tab manager implementation. See tab_manager.h for the
// threading contract.
#include "tab_manager.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <fstream>
#include <map>
#include <sstream>

#include "include/cef_parser.h"
#include "include/cef_cookie.h"
#include "include/cef_request_handler.h"
#include "include/cef_task.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_message_router.h"
#include "adblock/site_util.h"
#include "dask_scheme.h"
#include "storage/download_filename.h"
#include "updater.h"

#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace dask {

// Phase 10 forward decls (defined later in namespace dask; used by
// DaskQueryHandler + TabClient before their definitions).
std::string HostOf(const std::wstring& wurl);
bool DpapiUnprotect(const std::string& b64, std::string* plain);
std::string DpapiProtect(const std::string& plain);
std::string FillerScript(const std::string& host);
bool ScriptMatches(const std::string& patterns, const std::string& host);

namespace {

constexpr wchar_t kDaskUiOrigin[] = L"http://dask.ui/";
constexpr wchar_t kSessionFile[] = L"session.json";
constexpr int kMaxTabs = 32;  // sanity limit for popups / session restore.

const SearchEngineConfig kDefaultEngine;
const std::vector<QuickLink> kDefaultLinks = {
    {L"ويكيبيديا", L"https://ar.wikipedia.org"},
    {L"GitHub", L"https://github.com"},
    {L"MDN", L"https://developer.mozilla.org"},
    {L"YouTube", L"https://youtube.com"},
};

std::wstring Trim(const std::wstring& s) {
  const size_t a = s.find_first_not_of(L" \t\r\n");
  if (a == std::wstring::npos) return L"";
  const size_t b = s.find_last_not_of(L" \t\r\n");
  return s.substr(a, b - a + 1);
}

// UTF-8 <-> UTF-16 conversions for storage strings.
std::string Utf8FromWide(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string out(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring WideFromUtf8(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
  return out;
}

// Risky download types: an executable/script that would run arbitrary code.
// Used to require an explicit confirmation before opening the file.
bool IsRiskyDownloadFilename(const std::wstring& filename) {
  const wchar_t* risky[] = {L".exe", L".msi", L".bat", L".cmd", L".ps1",
                            L".scr", L".vbs", L".js",  L".jar", L".com",
                            L".hta", L".msix", L".appx"};
  if (filename.empty()) return false;
  std::wstring lower = filename;
  for (wchar_t& c : lower) {
    if (c >= L'A' && c <= L'Z') c += 32;
  }
  const size_t dot = lower.find_last_of(L'.');
  if (dot == std::wstring::npos) return false;
  const std::wstring ext = lower.substr(dot);
  for (const wchar_t* r : risky) {
    if (ext == r) return true;
  }
  // Double extension: setup.pdf.exe style - last ext matched above but check
  // whether an earlier risky extension is embedded as well.
  size_t prev = lower.find_last_of(L'.', dot - 1);
  if (prev != std::wstring::npos) {
    const std::wstring prev_ext = lower.substr(prev);
    for (const wchar_t* r : risky) {
      if (prev_ext == r) return true;
    }
  }
  return false;
}

// Known Downloads folder (created when missing).
std::wstring DownloadsDir() {
  PWSTR raw = nullptr;
  if (::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw) != S_OK) {
    return L"";
  }
  std::wstring dir = raw;
  ::CoTaskMemFree(raw);
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring AppDataDir() {
  PWSTR raw = nullptr;
  if (::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw) != S_OK)
    return L"";
  std::wstring dir = std::wstring(raw) + L"\\DaskBrowser";
  ::CoTaskMemFree(raw);
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool IsHttpUrl(const std::wstring& url) {
  return url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
}

// CefTask adapter that runs a plain callable on the target thread.
class FnTask : public CefTask {
 public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    auto run = std::move(fn_);
    run();
  }

 private:
  IMPLEMENT_REFCOUNTING(FnTask);
  DISALLOW_COPY_AND_ASSIGN(FnTask);

  std::function<void()> fn_;
};

void PostTaskTo(CefThreadId thread, std::function<void()> fn) {
  CefRefPtr<FnTask> task(new FnTask(std::move(fn)));
  const bool ok = CefPostTask(thread, task);
  if (!ok) {
  }
}

// Appends an already-filled dictionary to a CefListValue (SetDictionary
// copies the value at call time, so callers must fill |item| first).
void AppendDict(CefRefPtr<CefListValue> list,
                const CefRefPtr<CefDictionaryValue>& item) {
  const size_t idx = list->GetSize();
  list->SetSize(idx + 1);
  list->SetDictionary(idx, item);
}

}  // namespace

void PostToWin(HWND main_hwnd, std::function<void()> fn) {
  auto* boxed = new std::function<void()>(std::move(fn));
  if (!::PostMessageW(main_hwnd, WM_DASK_RUN_TASK, 0,
                      reinterpret_cast<LPARAM>(boxed))) {
    // Window already destroyed (app shutting down): nothing to run.
    delete boxed;
  }
}

void PostToCef(std::function<void()> fn) {
  PostTaskTo(TID_UI, std::move(fn));
}

// ---------------------------------------------------------------------------
// Query handler: executes UI requests. Authorization: only the UI browser and
// trusted dask UI pages may call the bridge; ordinary web content is
// rejected with 403. Note: Handler is not ref-counted; the instance is owned
// by the process-lifetime TabManager singleton via router_->AddHandler().
// ---------------------------------------------------------------------------

class DaskQueryHandler : public CefMessageRouterBrowserSide::Handler {
 public:
  explicit DaskQueryHandler(TabManager* mgr) : mgr_(mgr) {}

  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<CefMessageRouterBrowserSide::Callback> callback) override {
    // Authorization: browser == UI browser, or a trusted internal page.
    // Phase 10: WEB content gets exactly three narrow filler-channel calls
    // (pwQuery/pwSubmit — password manager — and usCode for user scripts),
    // each verified against the frame's own host.
    if (!mgr_->ui_browser_ || !browser->IsSame(mgr_->ui_browser_)) {
      if (mgr_->menu_browser_ && browser->IsSame(mgr_->menu_browser_)) {
        // trusted: the menu popup browser (Phase 7)
      } else {
      TabManager::Tab* tab = mgr_->FindTabByBrowser(browser);
      const std::wstring frame_url = frame->GetURL().ToWString();
      const bool trusted = tab && tab->internal &&
                           frame_url.rfind(kDaskUiOrigin, 0) == 0;
      if (!trusted) {
        // Peek at the request type before rejecting: allow the narrow
        // filler channel for real web pages.
        CefRefPtr<CefValue> peek =
            CefParseJSON(request, cef_json_parser_options_t(0));
        const std::string t =
            peek && peek->GetType() == VTYPE_DICTIONARY
                ? peek->GetDictionary()->GetString("type").ToString()
                : std::string();
        if (t != "pwQuery" && t != "pwSubmit" && t != "usCode") {
          callback->Failure(403, "bridge access denied for this page");
          return true;
        }
        if (!peek || peek->GetType() != VTYPE_DICTIONARY) {
          callback->Failure(400, "bad request");
          return true;
        }
        CefRefPtr<CefDictionaryValue> d = peek->GetDictionary();
        const std::string frame_host = HostOf(frame_url);
        if (frame_host.empty()) {
          callback->Failure(403, "no host");
          return true;
        }
        if (t == "pwQuery") {
          // Only answer when a saved credential exists for THIS host.
          const std::string host = d->GetString("host").ToString();
          if (host != frame_host) {
            callback->Failure(403, "host mismatch");
            return true;
          }
          TabManager* mgr = mgr_;
          mgr_->PostToDb([mgr, callback, host]() {
            if (!mgr->passwords_store_) {
              callback->Success("{}");
              return;
            }
            std::vector<storage::PasswordRow> rows;
            std::string err;
            if (!mgr->passwords_store_->FindByOrigin(host, &rows, &err) ||
                rows.empty()) {
              callback->Success("{}");
              return;
            }
            std::string pass;
            if (!DpapiUnprotect(rows[0].secret, &pass)) {
              callback->Success("{}");
              return;
            }
            CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
            root->SetString("user", rows[0].username);
            root->SetString("pass", pass);
            CefRefPtr<CefValue> value = CefValue::Create();
            value->SetDictionary(root);
            callback->Success(
                CefWriteJSON(value, cef_json_writer_options_t(0)).ToString());
          });
          return true;
        }
        if (t == "pwSubmit") {
          const std::string host = d->GetString("host").ToString();
          const std::string user =
              d->GetString("username").ToString().substr(0, 128);
          const std::string pass =
              d->GetString("password").ToString().substr(0, 512);
          if (host != frame_host || pass.empty()) {
            callback->Failure(403, "host mismatch");
            return true;
          }
          mgr_->PromptSavePassword(host, user, pass);
          mgr_->pw_hosts_.insert(host);
          callback->Success("{}");
          return true;
        }
        // t == "usCode": return enabled userscripts for this host.
        CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
        CefRefPtr<CefListValue> list = CefListValue::Create();
        const auto codes = mgr_->ScriptsForHost(frame_host);
        for (size_t i = 0; i < codes.size(); ++i) list->SetString(i, codes[i]);
        root->SetList("scripts", list);
        CefRefPtr<CefValue> value = CefValue::Create();
        value->SetDictionary(root);
        callback->Success(
            CefWriteJSON(value, cef_json_writer_options_t(0)).ToString());
        return true;
      }
      }
    }

    CefRefPtr<CefValue> parsed =
        CefParseJSON(request, cef_json_parser_options_t(0));
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) {
      callback->Failure(400, "bad request");
      return true;
    }
    CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
    const std::string type = dict->GetString("type").ToString();

    // Phase 5: explicit IPC allowlist. Anything not listed here is rejected
    // outright (defense in depth on top of the origin check above).
    static const std::set<std::string> kAllowedRequests = {
        "subscribe", "wait", "getState", "navigate", "back", "forward",
        "reload", "stop", "home", "createTab", "closeTab", "activateTab",
        "setQuickLinks", "setSearchEngine", "setBlockingEnabled",
        "setThirdPartyCookies", "setSiteAllowed", "removeSiteException",
        "clearSiteData", "clearBrowsingData", "updateFilterLists",
        "toggleBookmark", "removeBookmark", "updateBookmark",
        "searchHistory", "clearHistory", "setHistoryEnabled",
        "setRetentionDays", "setRestoreSession", "cancelDownload",
        "removeDownload", "openDownload", "openDownloadsFolder",
        "respondPermission", "setPermission", "resetPermission",
        "addProfile", "switchProfile", "deleteProfile", "newWindow",
        "newPrivateWindow", "adjustZoom", "toggleFullscreen", "findInPage",
        "stopFind", "showDevTools", "savePagePdf", "printPage",
        "copyPageLink", "exitApp", "setLightTheme", "toggleAppMenu",
        "closeAppMenu", "setDnsResolver", "setListEnabled", "viewSource",
        "resizeMenuPopup", "editCommand", "createShortcut", "installPageApp",
        "dragWindow", "windowControl", "getSetting", "setSetting",
        "pwList", "pwDelete", "pwClearAll", "pwResolve", "checkUpdate",
        "usList", "usAdd", "usRemove", "usToggle"};
    if (!kAllowedRequests.count(type)) {
      callback->Failure(403, "unknown bridge request type");
      return true;
    }

    if (type == "subscribe" || type == "wait") {
      // Persistent wait channel: keep the callback and deliver on the next
      // event. If state changed while nobody was waiting, deliver a fresh
      // snapshot to this subscriber right away.
      mgr_->waiters_[query_id] = callback;
      if (mgr_->state_dirty_) {
        mgr_->state_dirty_ = false;
        const bool focus = mgr_->queued_focus_address_;
        mgr_->queued_focus_address_ = false;
        callback->Success(mgr_->BuildStateJson(focus));
        mgr_->waiters_.erase(query_id);
      }
      return true;
    }

    if (type == "getState") {
      const bool focus = mgr_->queued_focus_address_;
      mgr_->queued_focus_address_ = false;
      callback->Success(mgr_->BuildStateJson(focus));
      return true;
    }

    if (type == "navigate") {
      const std::wstring url = dict->GetString("url").ToWString();
      if (mgr_->ui_browser_ && browser->IsSame(mgr_->ui_browser_)) {
        mgr_->NavigateActive(url);
      } else {
        mgr_->NavigateSelf(browser, url);
      }
    } else if (type == "back") {
      mgr_->Back();
    } else if (type == "forward") {
      mgr_->Forward();
    } else if (type == "reload") {
      mgr_->Reload();
    } else if (type == "stop") {
      mgr_->Stop();
    } else if (type == "home") {
      mgr_->Home();
    } else if (type == "createTab") {
      std::wstring url = dict->GetString("url").ToWString();
      if (url.empty() || url == kNewTabUrl) {
        url = kNewTabUrl;
      } else if (!IsHttpUrl(url) && url.rfind(kDaskUiOrigin, 0) != 0) {
        url = kNewTabUrl;  // only http(s) / internal UI targets are accepted
      }
      mgr_->CreateTab(url, dict->GetBool("private"));
    } else if (type == "closeTab") {
      mgr_->CloseTab(dict->GetInt("tabId"));
    } else if (type == "activateTab") {
      mgr_->ActivateTab(dict->GetInt("tabId"));
    } else if (type == "setBlockingEnabled") {
      mgr_->privacy_.set_blocking_enabled(dict->GetBool("enabled"));
      mgr_->privacy_.Save();
      mgr_->PushUiEvent();
    } else if (type == "setThirdPartyCookies") {
      mgr_->privacy_.set_block_third_party_cookies(dict->GetBool("enabled"));
      mgr_->privacy_.Save();
      mgr_->PushUiEvent();
    } else if (type == "setSiteAllowed") {
      std::wstring host = dict->GetString("host").ToWString();
      host = adblock::UrlHost(host);
      if (!host.empty()) {
        mgr_->privacy_.SetSiteAllowed(host, dict->GetBool("allowed"));
        mgr_->privacy_.Save();
        mgr_->PushUiEvent();
      }
    } else if (type == "removeSiteException") {
      std::wstring host = dict->GetString("host").ToWString();
      host = adblock::UrlHost(host);
      if (!host.empty()) {
        mgr_->privacy_.RemoveSiteException(host);
        mgr_->privacy_.Save();
        mgr_->PushUiEvent();
      }
    } else if (type == "clearSiteData") {
      mgr_->ClearCookiesFor(dict->GetString("host").ToWString());
    } else if (type == "clearBrowsingData") {
      mgr_->ClearAllCookies();
    } else if (type == "updateFilterLists") {
      mgr_->UpdateFilterLists();
    } else if (type == "toggleBookmark") {
      mgr_->ToggleBookmark(dict->GetString("title").ToWString(),
                           dict->GetString("url").ToWString());
    } else if (type == "removeBookmark") {
      mgr_->RemoveBookmark(dict->GetInt("id"));
    } else if (type == "updateBookmark") {
      mgr_->UpdateBookmark(dict->GetInt("id"),
                           dict->GetString("title").ToWString(),
                           dict->GetString("url").ToWString());
    } else if (type == "searchHistory") {
      // Async DB query; the callback is answered from the DB thread.
      const std::string query =
          Utf8FromWide(dict->GetString("query").ToWString());
      int limit = dict->GetInt("limit");
      if (limit <= 0) limit = 50;
      if (limit > 500) limit = 500;
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, callback, query, limit]() {
        if (!mgr->history_store_) {
          callback->Failure(503, "database unavailable");
          return;
        }
        std::vector<storage::HistoryItem> rows;
        std::string err;
        if (!mgr->history_store_->Search(query, limit, &rows, &err)) {
          callback->Failure(500, err);
          return;
        }
        // Build the response JSON via CEF values.
        CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
        CefRefPtr<CefListValue> items = CefListValue::Create();
        for (size_t i = 0; i < rows.size(); ++i) {
          CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
          item->SetInt("id", static_cast<int>(rows[i].id));
          item->SetString("url", rows[i].url);
          item->SetString("title", rows[i].title);
          item->SetInt("visitedAt", static_cast<int>(rows[i].visited_at));
          items->SetDictionary(i, item);
        }
        root->SetList("items", items);
        CefRefPtr<CefValue> value = CefValue::Create();
        value->SetDictionary(root);
        callback->Success(
            CefWriteJSON(value, cef_json_writer_options_t(0)).ToString());
      });
      return true;
    } else if (type == "clearHistory") {
      mgr_->ClearHistory(dict->GetInt("days"));
    } else if (type == "setHistoryEnabled") {
      mgr_->SetHistoryEnabled(dict->GetBool("enabled"));
      mgr_->PushUiEvent();
    } else if (type == "setRetentionDays") {
      mgr_->SetRetentionDays(dict->GetInt("days"));
    } else if (type == "setRestoreSession") {
      mgr_->SetRestoreSession(dict->GetBool("enabled"));
    } else if (type == "cancelDownload") {
      mgr_->CancelDownload(dict->GetInt("id"));
    } else if (type == "removeDownload") {
      const int64_t id = dict->GetInt("id");
      TabManager* mgr = mgr_;
      mgr_->CancelDownload(id);
      mgr_->PostToDb([mgr, id]() {
        if (!mgr->downloads_store_) return;
        std::string err;
        mgr->downloads_store_->Delete(id, &err);
        mgr->RefreshStorageCachesOnDb();
      });
    } else if (type == "openDownload") {
      mgr_->OpenDownload(dict->GetInt("id"));
    } else if (type == "openDownloadsFolder") {
      mgr_->OpenDownloadsFolder();
    } else if (type == "respondPermission") {
      mgr_->RespondPermission(
          static_cast<uint64_t>(dict->GetInt("id")),
          dict->GetString("action").ToString());
    } else if (type == "setPermission") {
      mgr_->SetPermission(dict->GetString("host").ToString(),
                          dict->GetString("perm").ToString(),
                          dict->GetInt("decision"));
    } else if (type == "resetPermission") {
      mgr_->ResetPermission(dict->GetString("host").ToString(),
                            dict->GetString("perm").ToString());
    } else if (type == "addProfile") {
      mgr_->AddProfile(dict->GetString("name").ToString());
    } else if (type == "switchProfile") {
      mgr_->SwitchProfile(dict->GetString("id").ToString());
    } else if (type == "deleteProfile") {
      mgr_->DeleteProfile(dict->GetString("id").ToString());
    } else if (type == "newWindow") {
      mgr_->NewBrowserWindow(false);
    } else if (type == "newPrivateWindow") {
      mgr_->NewBrowserWindow(true);
    } else if (type == "adjustZoom") {
      mgr_->AdjustZoom(dict->GetDouble("step"));
    } else if (type == "toggleFullscreen") {
      mgr_->ToggleFullscreen();
    } else if (type == "findInPage") {
      const std::string text = dict->GetString("text").ToString();
      if (text.empty()) {
        mgr_->StopFind();
      } else {
        mgr_->FindInPage(text, dict->GetBool("forward"),
                         dict->GetBool("findNext"));
        {
          std::lock_guard<std::mutex> lock(mgr_->state_mutex_);
          mgr_->find_active_ = true;
        }
        mgr_->PushUiEvent();
      }
    } else if (type == "stopFind") {
      mgr_->StopFind();
    } else if (type == "showDevTools") {
      mgr_->ShowDevTools();
    } else if (type == "savePagePdf") {
      mgr_->SavePageAsPdf();
    } else if (type == "printPage") {
      mgr_->PrintPage();
    } else if (type == "copyPageLink") {
      mgr_->CopyPageLink();
    } else if (type == "toggleAppMenu") {
      mgr_->ToggleAppMenu();
    } else if (type == "closeAppMenu") {
      mgr_->CloseAppMenu();
    } else if (type == "exitApp") {
      mgr_->ExitApp();
    } else if (type == "setLightTheme") {
      mgr_->SetLightTheme(dict->GetBool("enabled"));
    } else if (type == "setDnsResolver") {
      mgr_->SetDnsResolver(dict->GetString("resolver").ToString());
    } else if (type == "setListEnabled") {
      mgr_->SetListEnabled(dict->GetString("name").ToString(),
                           dict->GetBool("enabled"));
    } else if (type == "viewSource") {
      mgr_->ViewSourceActive();
    } else if (type == "editCommand") {
      mgr_->EditCommandActive(dict->GetString("cmd").ToString());
    } else if (type == "createShortcut") {
      mgr_->CreatePageShortcut();
    } else if (type == "installPageApp") {
      mgr_->InstallPageApp();
    } else if (type == "dragWindow" || type == "windowControl") {
      // Phase 9: custom title bar. Only the toolbar browser (tab id 0) may
      // drag the window or use the min/max/close controls — web content and
      // even internal pages cannot.
      if (!mgr_->ui_browser_ || !browser->IsSame(mgr_->ui_browser_)) {
        callback->Failure(403, "window controls are toolbar-only");
        return true;
      }
      if (type == "dragWindow") {
        ::PostMessageW(mgr_->main_hwnd_, WM_DASK_DRAG, 0, 0);
      } else {
        const std::string action = dict->GetString("action").ToString();
        const WPARAM wparam = action == "min" ? 0
                              : action == "close" ? 2 : 1;
        ::PostMessageW(mgr_->main_hwnd_, WM_DASK_WINDOWCTL, wparam, 0);
      }
    } else if (type == "getSetting" || type == "setSetting") {
      // Phase 9.5: small named preferences for the UI (accent color, first-run
      // wizard flag). Strict key allowlist — the bridge never becomes a
      // generic key/value store.
      static const std::set<std::string> kSettingKeys = {"accent", "onboarded"};
      const std::string key = dict->GetString("key").ToString();
      if (!kSettingKeys.count(key)) {
        callback->Failure(403, "unknown setting key");
        return true;
      }
      // Read from the in-memory copy: a synchronous SQLite read here would
      // race the DB thread's connection and silently fail (empty value).
      if (type == "getSetting") {
        callback->Success(key == "accent" ? mgr_->accent_ : mgr_->onboarded_);
      } else {
        const std::string value = dict->GetString("value").ToString();
        if (value.size() > 64) {
          callback->Failure(400, "value too long");
          return true;
        }
        mgr_->SetPrefString(key, value);
        if (key == "accent") {
          if (mgr_->accent_ != value) {
            mgr_->accent_ = value;
            mgr_->PushUiEvent();
          }
        } else if (key == "onboarded") {
          mgr_->onboarded_ = value;
        }
      }
    } else if (type == "checkUpdate") {
      mgr_->CheckAppUpdate(true);
      callback->Success("{}");
    } else if (type == "pwResolve") {
      const int64_t id = dict->GetInt("id");
      const bool save = dict->GetBool("save");
      mgr_->ResolvePasswordPrompt(id, save);
      callback->Success("{}");
    } else if (type == "pwList") {
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, callback]() {
        if (!mgr->passwords_store_) {
          callback->Failure(503, "database unavailable");
          return;
        }
        std::vector<storage::PasswordRow> rows;
        std::string err;
        if (!mgr->passwords_store_->List(&rows, &err)) {
          callback->Failure(500, err);
          return;
        }
        CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
        CefRefPtr<CefListValue> items = CefListValue::Create();
        for (size_t i = 0; i < rows.size(); ++i) {
          CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
          item->SetInt("id", static_cast<int>(rows[i].id));
          item->SetString("origin", rows[i].origin);
          item->SetString("username", rows[i].username);
          item->SetInt("updatedAt", static_cast<int>(rows[i].updated_at));
          items->SetDictionary(i, item);
        }
        root->SetList("items", items);
        CefRefPtr<CefValue> value = CefValue::Create();
        value->SetDictionary(root);
        callback->Success(
            CefWriteJSON(value, cef_json_writer_options_t(0)).ToString());
      });
      return true;
    } else if (type == "pwDelete") {
      const int64_t id = dict->GetInt("id");
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, id]() {
        if (mgr->passwords_store_) {
          std::string err;
          mgr->passwords_store_->Remove(id, &err);
        }
      });
      callback->Success("{}");
    } else if (type == "pwClearAll") {
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr]() {
        if (mgr->passwords_store_) {
          std::string err;
          mgr->passwords_store_->ClearAll(&err);
        }
      });
      callback->Success("{}");
    } else if (type == "usList") {
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, callback]() {
        if (!mgr->user_scripts_store_) {
          callback->Failure(503, "database unavailable");
          return;
        }
        std::vector<storage::UserScriptRow> rows;
        std::string err;
        if (!mgr->user_scripts_store_->List(&rows, &err)) {
          callback->Failure(500, err);
          return;
        }
        CefRefPtr<CefDictionaryValue> root = CefDictionaryValue::Create();
        CefRefPtr<CefListValue> items = CefListValue::Create();
        for (size_t i = 0; i < rows.size(); ++i) {
          CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
          item->SetInt("id", static_cast<int>(rows[i].id));
          item->SetString("name", rows[i].name);
          item->SetString("matches", rows[i].matches);
          item->SetString("code", rows[i].code);
          item->SetBool("enabled", rows[i].enabled);
          items->SetDictionary(i, item);
        }
        root->SetList("items", items);
        CefRefPtr<CefValue> value = CefValue::Create();
        value->SetDictionary(root);
        callback->Success(
            CefWriteJSON(value, cef_json_writer_options_t(0)).ToString());
      });
      return true;
    } else if (type == "usAdd") {
      std::string name = dict->GetString("name").ToString();
      std::string matches = dict->GetString("matches").ToString();
      std::string code = dict->GetString("code").ToString();
      if (name.empty() || name.size() > 64 || matches.empty() ||
          matches.size() > 256 || code.empty() || code.size() > (256 << 10)) {
        callback->Failure(400, "bad script fields");
        return true;
      }
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, name, matches, code]() {
        if (mgr->user_scripts_store_) {
          std::string err;
          int64_t id = 0;
          mgr->user_scripts_store_->Add(name, matches, code, &id, &err);
          // refresh the CEF UI copy of the scripts list
          std::vector<storage::UserScriptRow> rows;
          if (mgr->user_scripts_store_->List(&rows, &err)) {
            PostToCef([mgr, rows]() {
              std::lock_guard<std::mutex> lock(mgr->state_mutex_);
              mgr->scripts_cache_.clear();
              for (const auto& r : rows) {
                mgr->scripts_cache_.push_back({r.id, r.name, r.matches,
                                               r.code, r.enabled});
              }
            });
          }
        }
      });
      callback->Success("{}");
    } else if (type == "usRemove") {
      const int64_t id = dict->GetInt("id");
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, id]() {
        if (mgr->user_scripts_store_) {
          std::string err;
          mgr->user_scripts_store_->Remove(id, &err);
          std::vector<storage::UserScriptRow> rows;
          if (mgr->user_scripts_store_->List(&rows, &err)) {
            PostToCef([mgr, rows]() {
              std::lock_guard<std::mutex> lock(mgr->state_mutex_);
              mgr->scripts_cache_.clear();
              for (const auto& r : rows) {
                mgr->scripts_cache_.push_back({r.id, r.name, r.matches,
                                               r.code, r.enabled});
              }
            });
          }
        }
      });
      callback->Success("{}");
    } else if (type == "usToggle") {
      const int64_t id = dict->GetInt("id");
      const bool enabled = dict->GetBool("enabled");
      TabManager* mgr = mgr_;
      mgr_->PostToDb([mgr, id, enabled]() {
        if (mgr->user_scripts_store_) {
          std::string err;
          mgr->user_scripts_store_->SetEnabled(id, enabled, &err);
          std::vector<storage::UserScriptRow> rows;
          if (mgr->user_scripts_store_->List(&rows, &err)) {
            PostToCef([mgr, rows]() {
              std::lock_guard<std::mutex> lock(mgr->state_mutex_);
              mgr->scripts_cache_.clear();
              for (const auto& r : rows) {
                mgr->scripts_cache_.push_back({r.id, r.name, r.matches,
                                               r.code, r.enabled});
              }
            });
          }
        }
      });
      callback->Success("{}");
    } else if (type == "resizeMenuPopup") {
      mgr_->ResizeMenuPopup(dict->GetBool("wide"), dict->GetInt("top"),
                            dict->GetInt("height"));
    } else if (type == "setQuickLinks") {
      CefRefPtr<CefListValue> list = dict->GetList("links");
      if (list && list->GetSize() <= 24) {
        std::vector<QuickLink> links;
        for (size_t i = 0; i < list->GetSize(); ++i) {
          CefRefPtr<CefDictionaryValue> item = list->GetDictionary(i);
          if (!item) continue;
          QuickLink link;
          link.title = Trim(item->GetString("title").ToWString()).substr(0, 64);
          std::wstring url =
              Trim(item->GetString("url").ToWString()).substr(0, 2048);
          if (!IsHttpUrl(url)) continue;  // refuse non-http links
          link.url = url;
          links.push_back(link);
        }
        mgr_->quick_links_ = std::move(links);
        mgr_->SaveSession();
        mgr_->PushUiEvent();
      }
    } else if (type == "setSearchEngine") {
      const std::wstring id = dict->GetString("id").ToWString();
      if (id == L"google" || id == L"bing" || id == L"duckduckgo" ||
          id == L"custom") {
        mgr_->engine_.id = id;
        mgr_->engine_.custom =
            Trim(dict->GetString("custom").ToWString()).substr(0, 2048);
        mgr_->SaveSession();
        mgr_->PushUiEvent();
      }
    } else {
      callback->Failure(400, "unknown request type");
      return true;
    }

    callback->Success("{\"ok\":true}");
    return true;
  }

  void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int64_t query_id) override {
    (void)browser;
    (void)frame;
    // A wait query ended (page navigated away or closed).
    mgr_->waiters_.erase(query_id);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(DaskQueryHandler);

  TabManager* mgr_;
};

// ---------------------------------------------------------------------------
// TabClient: per-browser CEF client for content tabs and the UI browser.
// ---------------------------------------------------------------------------

// Per-request resource handler: ad filtering + third-party cookie policy.
// Created per request on the IO thread via GetResourceRequestHandler.
class DaskResourceHandler : public CefResourceRequestHandler,
                            public CefCookieAccessFilter {
 public:
  DaskResourceHandler(TabManager* mgr, int64_t tab_id,
                      std::wstring first_party)
      : mgr_(mgr), tab_id_(tab_id), first_party_(std::move(first_party)) {}

  // CefResourceRequestHandler (IO thread).
  ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefRequest> request,
                                   CefRefPtr<CefCallback> callback) override {
    (void)browser;
    (void)frame;
    (void)callback;
    const std::wstring url = request->GetURL().ToWString();
    if (ShouldCheck(url)) {
      std::string matched;
      if (mgr_->ShouldBlockRequest(url, first_party_,
                                   KindOf(request->GetResourceType()),
                                   &matched)) {
        mgr_->OnRequestBlocked(tab_id_);
        return RV_CANCEL;
      }
    }
    return RV_CONTINUE;
  }

  CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)request;
    return this;
  }

  // CefCookieAccessFilter (IO thread).
  bool CanSendCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     const CefCookie& cookie) override {
    (void)browser;
    (void)frame;
    (void)cookie;
    return mgr_->CookieAllowed(adblock::UrlHost(request->GetURL().ToWString()),
                               first_party_);
  }

  bool CanSaveCookie(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     CefRefPtr<CefRequest> request,
                     CefRefPtr<CefResponse> response,
                     const CefCookie& cookie) override {
    (void)browser;
    (void)frame;
    (void)response;
    (void)cookie;
    return mgr_->CookieAllowed(adblock::UrlHost(request->GetURL().ToWString()),
                               first_party_);
  }

 private:
  IMPLEMENT_REFCOUNTING(DaskResourceHandler);
  DISALLOW_COPY_AND_ASSIGN(DaskResourceHandler);

  // Only http(s) web requests with a known first-party site are candidates;
  // internal UI pages and special schemes are always allowed.
  bool ShouldCheck(const std::wstring& url) const {
    if (first_party_.empty()) return false;
    return url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
  }

  static adblock::ResourceKind KindOf(CefRequest::ResourceType t) {
    switch (t) {
      case RT_SCRIPT: return adblock::ResourceKind::Script;
      case RT_IMAGE: return adblock::ResourceKind::Image;
      case RT_STYLESHEET: return adblock::ResourceKind::Stylesheet;
      case RT_SUB_FRAME: return adblock::ResourceKind::Subdocument;
      case RT_XHR: return adblock::ResourceKind::XmlHttpRequest;
      case RT_MEDIA: return adblock::ResourceKind::Media;
      case RT_FONT_RESOURCE: return adblock::ResourceKind::Font;
      case RT_OBJECT: return adblock::ResourceKind::Object;
      default: return adblock::ResourceKind::Other;
    }
  }

  TabManager* mgr_;
  int64_t tab_id_;
  std::wstring first_party_;  // top-level site host for this tab
};

class TabClient : public CefClient,
                  public CefDisplayHandler,
                  public CefFindHandler,
                  public CefDownloadHandler,
                  public CefLoadHandler,
                  public CefLifeSpanHandler,
                  public CefPermissionHandler,
                  public CefRequestHandler,
                  public CefContextMenuHandler {
 public:
  TabClient(TabManager* mgr, int64_t tab_id) : mgr_(mgr), tab_id_(tab_id) {}

  // CefClient
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return this;
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    return mgr_->router()->OnProcessMessageReceived(browser, frame,
                                                    source_process, message);
  }

  // CefRequestHandler
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override {
    (void)user_gesture;
    (void)is_redirect;
    mgr_->router()->OnBeforeBrowse(browser, frame);
    return false;  // allow the navigation
  }

  // CefRequestHandler (Phase 3): attach the ad-block + cookie filter to every
  // request of this tab. Main-frame document loads are never blocked.
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override {
    (void)browser;
    (void)frame;
    (void)is_navigation;
    (void)is_download;
    (void)request_initiator;
    (void)disable_default_handling;
    if (request->GetResourceType() == RT_MAIN_FRAME) {
      return nullptr;  // documents always load
    }
    return new DaskResourceHandler(mgr_, tab_id_,
                                   mgr_->TabSiteHost(tab_id_));
  }

  // CefDownloadHandler (Phase 4). Methods run on the browser UI thread.
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }

  // CefPermissionHandler (Phase 5): camera/microphone and the generic
  // permission prompt flow. Default (unhandled) = deny with Alloy style.
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return this;
  }

  // CefFindHandler (Phase 7): report find-in-page match counts to the UI.
  CefRefPtr<CefFindHandler> GetFindHandler() override { return this; }

  void OnFindResult(CefRefPtr<CefBrowser> browser,
                    int identifier,
                    int count,
                    const CefRect& selectionRect,
                    int activeMatchOrdinal,
                    bool finalUpdate) override {
    (void)browser;
    (void)identifier;
    (void)selectionRect;
    if (!finalUpdate) return;
    mgr_->OnFindResult(count, activeMatchOrdinal);
  }

  bool OnRequestMediaAccessPermission(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback) override {
    (void)browser;
    (void)frame;
    mgr_->OnMediaAccessPermission(tab_id_, requesting_origin.ToWString(),
                                  requested_permissions, callback);
    return true;  // decided asynchronously via TabManager
  }

  bool OnShowPermissionPrompt(
      CefRefPtr<CefBrowser> browser,
      uint64_t prompt_id,
      const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefPermissionPromptCallback> callback) override {
    (void)browser;
    return mgr_->OnPermissionPrompt(tab_id_, prompt_id,
                                    requesting_origin.ToWString(),
                                    requested_permissions, callback);
  }

  void OnDismissPermissionPrompt(
      CefRefPtr<CefBrowser> browser,
      uint64_t prompt_id,
      cef_permission_request_result_t result) override {
    (void)browser;
    (void)result;
    mgr_->DismissPermissionPrompt(prompt_id);
  }

  bool OnBeforeDownload(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefDownloadItem> download_item,
      const CefString& suggested_name,
      CefRefPtr<CefBeforeDownloadCallback> callback) override {
    (void)browser;
    return mgr_->OnBeforeDownload(tab_id_, download_item,
                                  suggested_name.ToWString(), callback);
  }

  void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefDownloadItem> download_item,
                         CefRefPtr<CefDownloadItemCallback> callback) override {
    (void)browser;
    mgr_->OnDownloadUpdated(tab_id_, download_item, callback);
  }

  // CefDisplayHandler
  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override {
    (void)browser;
    std::wstring t = title.ToWString();
    // Hide our own page suffix for a cleaner tab strip.
    const std::wstring suffix = L" - DASK Browser";
    if (t.size() > suffix.size() &&
        t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0) {
      t.resize(t.size() - suffix.size());
    }
    mgr_->OnTitleChanged(tab_id_, t);
  }

  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override {
    (void)browser;
    if (frame->IsMain()) {
      mgr_->OnAddressChanged(tab_id_, url.ToWString());
      mgr_->RecordVisit(tab_id_, url.ToWString(),
                        mgr_->TabTitle(tab_id_));
    }
  }

  // CefLoadHandler
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override {
    (void)browser;
    (void)canGoBack;
    (void)canGoForward;
    mgr_->OnLoadingChanged(tab_id_, isLoading);
  }

  // CefLoadHandler — Phase 10: inject user-script extensions and the
  // password auto-fill bootstrap into real web pages at load end.
  void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override {
    (void)httpStatusCode;
    if (!frame || frame->IsMain() == false) return;  // top frames only
    const std::wstring wurl = frame->GetURL().ToWString();
    if (wurl.rfind(L"http://dask.ui", 0) == 0 ||
        wurl.rfind(L"http://dask.ui/", 0) == 0) {
      return;  // never inject into internal pages
    }
    if (!(wurl.rfind(L"http://", 0) == 0 || wurl.rfind(L"https://", 0) == 0))
      return;
    // Host for the password channel.
    std::string host = HostOf(wurl);
    if (host.empty()) return;
    // 1) userscripts
    for (const std::string& code : mgr_->ScriptsForHost(host)) {
      frame->ExecuteJavaScript(code, frame->GetURL(), 0);
    }
    // 2) password filler bootstrap (only when the host already has a saved
    //    password — a tiny script that fills + listens for submit).
    if (mgr_->HasSavedPassword(host)) {
      frame->ExecuteJavaScript(
          CefString(FillerScript(host)), frame->GetURL(), 0);
    }
  }


  // CefContextMenuHandler (Phase 8): Brave-style Arabic context menu.
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override {
    (void)frame;
    mgr_->BuildContextMenu(browser, params, model);
  }

  bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            int command_id,
                            CefContextMenuHandler::EventFlags event_flags)
      override {
    (void)frame;
    (void)event_flags;
    return mgr_->HandleContextMenuCommand(browser, params, command_id);
  }

  // CefLifeSpanHandler: all popups become ordinary tabs inside the tab strip.
  // The disposition parameter is declared via CefLifeSpanHandler's
  // WindowOpenDisposition typedef (same underlying enum as CefRequestHandler's).
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     CefLifeSpanHandler::WindowOpenDisposition target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extraInfo,
                     bool* no_javascript_access) override {
    (void)browser;
    (void)frame;
    (void)popup_id;
    (void)target_frame_name;
    (void)target_disposition;
    (void)user_gesture;
    (void)popupFeatures;
    (void)windowInfo;
    (void)client;
    (void)settings;
    (void)extraInfo;
    (void)no_javascript_access;
    std::wstring url = target_url.ToWString();
    if (url.empty() || url == L"about:blank") url = kNewTabUrl;
    mgr_->CreateTab(url);
    return true;  // suppress the popup browser; a tab replaces it.
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    (void)browser;
    mgr_->router()->OnBeforeClose(browser);
    mgr_->OnBrowserClosed(tab_id_);
  }

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    (void)browser;
    mgr_->OnBrowserCreated(tab_id_, browser);
  }

 private:
  IMPLEMENT_REFCOUNTING(TabClient);
  DISALLOW_COPY_AND_ASSIGN(TabClient);

  TabManager* mgr_;
  int64_t tab_id_;
};

// ---------------------------------------------------------------------------
// TabManager
// ---------------------------------------------------------------------------

TabManager* TabManager::Get() {
  static TabManager instance;
  return &instance;
}

void TabManager::Initialize(HWND main_hwnd) {
  main_hwnd_ = main_hwnd;
  ::InitializeCriticalSection(&layout_lock_);
  engine_ = kDefaultEngine;
  quick_links_ = kDefaultLinks;

  // Phase 5: user profiles. The active profile owns dask.db, session.json,
  // privacy.json and a CEF cache under its own directory.
  profile_store_ = std::make_unique<ProfileStore>(AppDataDir());
  profile_store_->Load();

  // Phase 3: privacy settings and filter lists (per profile / shared cache).
  privacy_.Load(profile_store_->privacy_path());
  lists_.SetDisabledLists(privacy_.DisabledLists());
  lists_.Initialize(AppDataDir());

  CefMessageRouterConfig config;
  router_ = CefMessageRouterBrowserSide::Create(config);
  router_->AddHandler(new DaskQueryHandler(this), false);
}

void TabManager::Start() {
  // Phase 4: load preferences synchronously first (temporary connection) so
  // the restore decision below is correct.
  {
    storage::Database prefs_db;
    std::string db_err;
    if (prefs_db.Open(Utf8FromWide(profile_store_->db_path()), &db_err) &&
        prefs_db.Migrate(&db_err)) {
      storage::Settings prefs(&prefs_db);
      prefs.GetBool("restoreSession", false, &restore_session_, &db_err);
      prefs.GetBool("historyEnabled", true, &history_enabled_, &db_err);
      prefs.GetInt("historyRetentionDays", 90, &retention_days_, &db_err);
      prefs.GetBool("lightTheme", false, &light_theme_, &db_err);
      prefs.GetString("accent", "", &accent_, &db_err);
      if (accent_.empty() || accent_[0] != '#') accent_ = "#8b5cf6";
      prefs.GetString("onboarded", "", &onboarded_, &db_err);
    }
  }

  // Phase 8: an --app launch opens a chromeless app window: a single
  // content browser, no toolbar UI and no session restore. A bare URL
  // argument opens the normal window directly on that URL.
  const bool direct_url = !launch_url_.empty();
  if (launch_app_) {
    CreateTab(launch_url_.empty() ? kNewTabUrl : launch_url_);
    StartDatabase();
    return;
  }

  // Restore the persisted session (read on this thread, applied on CEF's).
  std::ifstream file(profile_store_->session_path(), std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  bool has_session = false;
  size_t restored_tabs = 0;
  if (!json.empty()) {
    CefRefPtr<CefValue> parsed = CefParseJSON(json, cef_json_parser_options_t(0));
    if (parsed && parsed->GetType() == VTYPE_DICTIONARY) {
      has_session = true;
      CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
      const std::wstring engine_id = dict->GetString("engineId").ToWString();
      if (engine_id == L"google" || engine_id == L"bing" ||
          engine_id == L"duckduckgo" || engine_id == L"custom") {
        engine_.id = engine_id;
        engine_.custom = dict->GetString("engineCustom").ToWString();
      }
      CefRefPtr<CefListValue> links = dict->GetList("quickLinks");
      if (links) {
        std::vector<QuickLink> restored;
        for (size_t i = 0; i < links->GetSize() && restored.size() < 24; ++i) {
          CefRefPtr<CefDictionaryValue> item = links->GetDictionary(i);
          if (!item) continue;
          QuickLink link;
          link.title = item->GetString("title").ToWString();
          link.url = item->GetString("url").ToWString();
          if (IsHttpUrl(link.url) && !link.title.empty()) {
            restored.push_back(link);
          }
        }
        if (!restored.empty()) quick_links_ = std::move(restored);
      }
      CefRefPtr<CefListValue> tabs = dict->GetList("tabs");
      // Phase 9: a private window starts fresh — it never restores tabs.
      if (tabs && restore_session_ && !direct_url && !force_private_) {
        for (size_t i = 0;
             i < tabs->GetSize() && i < static_cast<size_t>(kMaxTabs); ++i) {
          CefRefPtr<CefDictionaryValue> item = tabs->GetDictionary(i);
          if (!item) continue;
          const std::wstring url = item->GetString("url").ToWString();
          const bool internal = url == kNewTabUrl ||
                                url.rfind(kDaskUiOrigin, 0) == 0;
          if (!internal && !IsHttpUrl(url)) continue;
          CreateTab(url);  // thread-safe; queues work for the CEF UI thread.
          ++restored_tabs;
        }
      }
    }
  }

  // Always end up with at least one tab (empty session, or a session whose
  // stored tabs were all filtered out).
  if ((!has_session || restored_tabs == 0) && !direct_url) {
    CreateTab(kNewTabUrl);
  }

  // Phase 4: open the local database (thread + stores).
  StartDatabase();

  // Phase 9.6: automatic update check — one anonymous HTTPS GET per day,
  // delayed so it never affects startup.
  PostToCef([this]() {
    CefPostDelayedTask(TID_UI, new FnTask(
        []() { dask::CheckForUpdate(false); }), 15000);
  });

  // Phase 10: load the userscript cache (CEF UI copy) right after startup.
  PostToDb([this]() {
    if (!user_scripts_store_) return;
    std::vector<storage::UserScriptRow> rows;
    std::string err;
    if (user_scripts_store_->List(&rows, &err)) {
      PostToCef([this, rows]() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        scripts_cache_.clear();
        for (const auto& r : rows) {
          scripts_cache_.push_back(
              {r.id, r.name, r.matches, r.code, r.enabled});
        }
      });
    }
  });

  // The UI browser (React toolbar) on top of the window. Tab id 0.
  // Skipped in --app mode: the app window is pure content.
  if (!launch_app_) PostToCef([this]() {
    RECT rc;
    ::GetClientRect(main_hwnd_, &rc);
    const int width = rc.right - rc.left;
    HWND host = reinterpret_cast<HWND>(::SendMessageW(
        main_hwnd_, WM_DASK_CREATE_HOST, 0, 0));
    if (!host) return;

    CefRefPtr<TabClient> client = new TabClient(this, 0);
    CefWindowInfo info;
    info.SetAsChild(host, CefRect(0, 0, width, kUiToolbarHeight));
    CefBrowserSettings settings;
    CefString url;
    url.FromWString(L"http://dask.ui/index.html");
    CefBrowserHost::CreateBrowser(info, client, url, settings, nullptr,
                                  nullptr);
  });
}

void TabManager::RequestCloseAll() {
  PostToCef([this]() {
    closing_all_ = true;
    outstanding_closes_ = 0;
    if (ui_browser_) {
      ++outstanding_closes_;
      ui_browser_->GetHost()->CloseBrowser(true);
    }
    for (Tab& tab : tabs_) {
      if (!tab.closing && tab.browser) {
        tab.closing = true;
        ++outstanding_closes_;
        tab.browser->GetHost()->CloseBrowser(true);
      }
    }
    if (outstanding_closes_ == 0 && !ui_browser_) {
      PostToWin(main_hwnd_, [hwnd = main_hwnd_]() {
        ::PostMessageW(hwnd, WM_DASK_ALL_CLOSED, 0, 0);
      });
    }
  });
}

// ---- Operations -----------------------------------------------------------

void TabManager::CreateTab(std::wstring url, bool private_tab) {
  PostToCef([this, url, private_tab]() {
    if (closing_all_ || tabs_.size() >= static_cast<size_t>(kMaxTabs)) return;
    // Phase 7: in a --private process window every tab is private.
    const bool priv = private_tab || force_private_;
    const bool internal = url == kNewTabUrl ||
                          url.rfind(kDaskUiOrigin, 0) == 0;

    // Ask the Win32 thread for a host window. The Win32 thread never blocks
    // on us, so this SendMessage cannot deadlock.
    const int64_t id = NewTabId();
    HWND host = reinterpret_cast<HWND>(::SendMessageW(
        main_hwnd_, WM_DASK_CREATE_HOST, static_cast<WPARAM>(id), 0));
    if (!host) {
      return;
    }

    Tab tab;
    tab.id = id;
    tab.internal = internal;
    tab.private_flag = priv;
    tab.url = url;

    RECT rc;
    ::GetClientRect(main_hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top - kUiToolbarHeight;
    if (height <= 0) {
      return;
    }

    CefRefPtr<TabClient> client = new TabClient(this, id);
    CefWindowInfo info;
    info.SetAsChild(host, CefRect(0, 0, width, height));
    CefBrowserSettings settings;
    // Private tabs use a shared in-memory request context: cookies and cache
    // never touch the default profile storage.
    CefRefPtr<CefRequestContext> context;
    if (priv) {
      if (!private_context_) {
        private_context_ = CefRequestContext::CreateContext(
            CefRequestContextSettings(), nullptr);
        // Custom contexts need their own dask.ui registration; the global
        // factory only covers the default context.
        RegisterUiSchemeHandlerOnContext(private_context_);
      }
      context = private_context_;
    } else {
      // Phase 5: the active profile gets an isolated CEF request context
      // (cookies/cache under the active root, per profile — see profiles.h).
      // Pre-create the cache dir: a failed CEF-side profile creation at
      // first-tab time logs "Cannot create profile" and can fail-fast.
      if (!profile_context_) {
        const std::wstring cache_dir = profile_store_->cef_cache_dir();
        ::SHCreateDirectoryExW(nullptr, cache_dir.c_str(), nullptr);
        CefRequestContextSettings ctx_settings;
        CefString(&ctx_settings.cache_path).FromWString(cache_dir);
        profile_context_ = CefRequestContext::CreateContext(ctx_settings, nullptr);
        RegisterUiSchemeHandlerOnContext(profile_context_);
      }
      context = profile_context_;
    }
    CefString url_str;
    url_str.FromWString(url);
    CefBrowserHost::CreateBrowser(info, client, url_str, settings, nullptr,
                                  context);

    tabs_.push_back(std::move(tab));
    {
      std::lock_guard<std::mutex> lock(blocked_mutex_);
      tab_site_[id] = internal ? std::wstring() : adblock::UrlHost(url);
      tab_blocked_[id] = 0;
    }
    SetActive(&tabs_.back());
    SaveSession();
    PushUiEvent();
  });
}

void TabManager::CloseTab(int64_t tab_id) {
  PostToCef([this, tab_id]() {
    Tab* tab = FindTab(tab_id);
    if (!tab || tab->closing || !tab->browser) return;
    tab->closing = true;
    ++outstanding_closes_;
    tab->browser->GetHost()->CloseBrowser(true);
  });
}

void TabManager::ActivateTab(int64_t tab_id) {
  PostToCef([this, tab_id]() {
    Tab* tab = FindTab(tab_id);
    if (!tab || tab->closing) return;
    SetActive(tab);
    PushUiEvent();
  });
}

void TabManager::NavigateActive(const std::wstring& input) {
  PostToCef([this, input]() {
    Tab* tab = FindTab(active_tab_id_);
    if (!tab || tab->closing || !tab->browser) return;
    CefString url;
    url.FromWString(ResolveInput(input));
    tab->browser->GetMainFrame()->LoadURL(url);
  });
}

void TabManager::NavigateSelf(CefRefPtr<CefBrowser> browser,
                              const std::wstring& input) {
  // Called from OnQuery on the CEF UI thread - resolve and load directly.
  Tab* tab = FindTabByBrowser(browser);
  if (!tab || tab->closing || !tab->browser) return;
  CefString url;
  url.FromWString(ResolveInput(input));
  tab->browser->GetMainFrame()->LoadURL(url);
}

void TabManager::Back() {
  PostToCef([this]() {
    if (Tab* tab = FindTab(active_tab_id_)) {
      if (tab->browser) tab->browser->GoBack();
    }
  });
}

void TabManager::Forward() {
  PostToCef([this]() {
    if (Tab* tab = FindTab(active_tab_id_)) {
      if (tab->browser) tab->browser->GoForward();
    }
  });
}

void TabManager::Reload() {
  PostToCef([this]() {
    if (Tab* tab = FindTab(active_tab_id_)) {
      if (tab->browser) tab->browser->Reload();
    }
  });
}

void TabManager::Stop() {
  PostToCef([this]() {
    if (Tab* tab = FindTab(active_tab_id_)) {
      if (tab->browser) tab->browser->StopLoad();
    }
  });
}

void TabManager::Home() {
  PostToCef([this]() {
    if (Tab* tab = FindTab(active_tab_id_)) {
      if (tab->browser && !tab->closing) {
        CefString url;
        url.FromWString(kNewTabUrl);
        tab->browser->GetMainFrame()->LoadURL(url);
      }
    }
  });
}

void TabManager::FocusAddress() {
  PostToCef([this]() {
    if (ui_browser_) ui_browser_->GetHost()->SetFocus(true);
    PushUiEvent(/*focus_address=*/true);
  });
}

void TabManager::CycleTab(int direction) {
  PostToCef([this, direction]() {
    if (tabs_.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < tabs_.size(); ++i) {
      if (tabs_[i].id == active_tab_id_) {
        idx = i;
        break;
      }
    }
    const size_t next = (idx + tabs_.size() + direction) % tabs_.size();
    SetActive(&tabs_[next]);
    PushUiEvent();
  });
}

// ---- TabClient callbacks (CEF UI thread) -----------------------------------

void TabManager::OnBrowserCreated(int64_t tab_id, CefRefPtr<CefBrowser> browser) {
  if (tab_id == 0) {
    ui_browser_ = browser;
    RequestLayout();
    PushUiEvent();
    return;
  }
  for (Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      tab.browser = browser;
      if (tab.id == active_tab_id_) {
        browser->GetHost()->SetFocus(true);
        // Phase 7: newly created active page follows the chosen zoom level.
        browser->GetHost()->SetZoomLevel(zoom_level_);
      }
      break;
    }
  }
  RequestLayout();
  PushUiEvent();
}

void TabManager::OnBrowserClosed(int64_t tab_id) {
  PostToWin(main_hwnd_, [hwnd = main_hwnd_, tab_id]() {
    ::PostMessageW(hwnd, WM_DASK_DESTROY_HOST, static_cast<WPARAM>(tab_id), 0);
  });
  {
    std::lock_guard<std::mutex> lock(blocked_mutex_);
    tab_site_.erase(tab_id);
    tab_blocked_.erase(tab_id);
  }

  if (tab_id == 0) {
    ui_browser_ = nullptr;
  } else {
    for (auto it = tabs_.begin(); it != tabs_.end(); ++it) {
      if (it->id == tab_id) {
        tabs_.erase(it);
        break;
      }
    }
  }

  if (closing_all_) {
    if (outstanding_closes_ > 0) --outstanding_closes_;
    if (outstanding_closes_ == 0 && !ui_browser_) {
      PostToWin(main_hwnd_, [hwnd = main_hwnd_]() {
        ::PostMessageW(hwnd, WM_DASK_ALL_CLOSED, 0, 0);
      });
    }
  } else if (tabs_.empty()) {
    if (switching_profile_) {
      // Profile switch: every content tab is gone — swap storage on the CEF
      // UI thread and open the new profile's tabs (no window close).
      PerformProfileSwitch();
    } else {
      // Closing the last tab closes the window (Chrome-like behavior).
      ::PostMessageW(main_hwnd_, WM_CLOSE, 0, 0);
    }
  } else if (switching_profile_ && tabs_.size() == 1 && tabs_[0].id == 0) {
    // Only the UI browser (tab 0) remains: the switch can proceed.
    PerformProfileSwitch();
  } else {
    bool active_exists = false;
    for (const Tab& tab : tabs_) {
      if (tab.id == active_tab_id_ && !tab.closing) {
        active_exists = true;
        break;
      }
    }
    if (!active_exists) SetActive(&tabs_.back());
    SaveSession();
    PushUiEvent();
    RequestLayout();
  }
}

void TabManager::OnTitleChanged(int64_t tab_id, const std::wstring& title) {
  if (tab_id == 0) return;  // UI toolbar is not a session tab.
  for (Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      tab.title = title;
      break;
    }
  }
  // Phase 8: app windows carry the page title in their caption.
  if (launch_app_ && main_hwnd_ && !title.empty()) {
    PostToWin(main_hwnd_, [h = main_hwnd_, title]() {
      ::SetWindowTextW(h, (title + L" — DASK").c_str());
    });
  }
  PushUiEvent();
}

void TabManager::OnAddressChanged(int64_t tab_id, const std::wstring& url) {
  if (tab_id == 0) return;  // UI toolbar is not a session tab.
  for (Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      tab.url = url;
      tab.internal = tab.internal && url.rfind(kDaskUiOrigin, 0) == 0;
      // New page load: per-tab blocked counter restarts.
      {
        std::lock_guard<std::mutex> lock(blocked_mutex_);
        tab_blocked_[tab_id] = 0;
        tab_site_[tab_id] = tab.internal ? std::wstring() : adblock::UrlHost(url);
      }
      break;
    }
  }
  SaveSession();
  PushUiEvent();
}

void TabManager::OnLoadingChanged(int64_t tab_id, bool loading) {
  for (Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      tab.loading = loading;
      break;
    }
  }
  PushUiEvent();
}

// ---- Internal helpers (CEF UI thread) --------------------------------------

TabManager::Tab* TabManager::FindTab(int64_t id) {
  for (Tab& tab : tabs_) {
    if (tab.id == id) return &tab;
  }
  return nullptr;
}

TabManager::Tab* TabManager::FindTabByBrowser(CefRefPtr<CefBrowser> browser) {
  for (Tab& tab : tabs_) {
    if (tab.browser && tab.browser->IsSame(browser)) return &tab;
  }
  return nullptr;
}

void TabManager::SetActive(Tab* tab) {
  active_tab_id_ = tab->id;
  // Phase 6: background tabs are throttled by Chromium itself because the
  // layout hides their host windows (SW_HIDE). That pauses painting and
  // throttles timers/parsers for hidden tabs, while audio, downloads and
  // necessary page work continue per Chromium policy. (CefBrowserHost::
  // WasHidden applies only to windowless rendering, which we do not use.)
  {
    ::EnterCriticalSection(&layout_lock_);
    layout_order_.clear();
    layout_order_.reserve(tabs_.size());
    for (const Tab& t : tabs_) layout_order_.push_back(t.id);
    layout_active_ = active_tab_id_;
    ::LeaveCriticalSection(&layout_lock_);
  }
  if (tab->browser) {
    tab->browser->GetHost()->SetFocus(true);
    // Phase 7: carry the chosen zoom level over to the newly active page.
    tab->browser->GetHost()->SetZoomLevel(zoom_level_);
  }
  RequestLayout();
}


void TabManager::RequestLayout() {
  ::PostMessageW(main_hwnd_, WM_DASK_LAYOUT, 0, 0);
}

std::wstring TabManager::ResolveInput(const std::wstring& input) {
  const std::wstring text = Trim(input);
  if (text.empty()) return std::wstring(kNewTabUrl);
  if (text.rfind(L"http://", 0) == 0 || text.rfind(L"https://", 0) == 0 ||
      text.rfind(L"dask://", 0) == 0 || text.rfind(L"about:", 0) == 0) {
    return text;
  }
  // Looks like a domain: no spaces and contains a dot (or a host:port).
  if (text.find(L' ') == std::wstring::npos &&
      (text.find(L'.') != std::wstring::npos ||
       text.find(L"localhost") == 0)) {
    return L"https://" + text;
  }
  // Otherwise treat it as a search query for the configured engine.
  std::wstring fmt;
  if (engine_.id == L"bing") {
    fmt = L"https://www.bing.com/search?q=%s";
  } else if (engine_.id == L"duckduckgo") {
    fmt = L"https://duckduckgo.com/?q=%s";
  } else if (engine_.id == L"custom") {
    // Custom engines must be http(s) and contain %s; otherwise fall back.
    if (IsHttpUrl(engine_.custom) &&
        engine_.custom.find(L"%s") != std::wstring::npos) {
      fmt = engine_.custom;
    } else {
      fmt = L"https://www.google.com/search?q=%s";
    }
  } else {
    fmt = L"https://www.google.com/search?q=%s";
  }
  const size_t pos = fmt.find(L"%s");
  return fmt.substr(0, pos) + CefURIEncode(text, true).ToWString() +
         fmt.substr(pos + 2);
}

std::string TabManager::BuildStateJson(bool focus_address) {
  CefRefPtr<CefDictionaryValue> root_dict = CefDictionaryValue::Create();
  CefRefPtr<CefListValue> tabs_list = CefListValue::Create();
  for (const Tab& tab : tabs_) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetInt("id", static_cast<int>(tab.id));
    item->SetString("title", tab.title);
    item->SetString("url", tab.url);
    item->SetBool("loading", tab.loading);
    item->SetBool("internal", tab.internal);
    item->SetBool("private", tab.private_flag);
    AppendDict(tabs_list, item);
  }
  root_dict->SetList("tabs", tabs_list);
  root_dict->SetInt("activeTabId", static_cast<int>(active_tab_id_));

  // ---- Storage state (Phase 4) ----
  CefRefPtr<CefListValue> bm_list = CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t i = 0; i < bookmarks_cache_.size(); ++i) {
      CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
      item->SetInt("id", static_cast<int>(bookmarks_cache_[i].id));
      item->SetString("title", bookmarks_cache_[i].title);
      item->SetString("url", bookmarks_cache_[i].url);
      bm_list->SetDictionary(i, item);
    }
  }
  root_dict->SetList("bookmarks", bm_list);

  CefRefPtr<CefListValue> dl_list = CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t i = 0; i < downloads_cache_.size(); ++i) {
      CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
      item->SetInt("id", static_cast<int>(downloads_cache_[i].id));
      item->SetString("filename", downloads_cache_[i].filename);
      item->SetString("url", downloads_cache_[i].url);
      item->SetInt("state", downloads_cache_[i].state);
      item->SetInt("received", static_cast<int>(downloads_cache_[i].received));
      item->SetInt("total", static_cast<int>(downloads_cache_[i].total));
      item->SetBool("risky", downloads_cache_[i].risky);
      dl_list->SetDictionary(i, item);
    }
  }
  root_dict->SetList("downloads", dl_list);

  // ---- Profiles & permissions (Phase 5) ----
  CefRefPtr<CefListValue> prof_list = CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const ProfileStore* store = profile_store_.get();
    int idx = 0;
    for (const ProfileInfo& p : store->profiles()) {
      CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
      item->SetString("id", p.id);
      item->SetString("name", p.name);
      item->SetBool("active", p.id == store->active_id());
      prof_list->SetDictionary(idx++, item);
    }
  }
  root_dict->SetList("profiles", prof_list);

  CefRefPtr<CefListValue> perm_list = CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int idx = 0;
    for (const PermissionUi& p : permissions_cache_) {
      CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
      item->SetString("host", p.host);
      item->SetString("type", p.type);
      item->SetInt("decision", p.decision);
      perm_list->SetDictionary(idx++, item);
    }
  }
  root_dict->SetList("permissions", perm_list);

  CefRefPtr<CefDictionaryValue> pending_perm;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!permission_prompts_.empty()) {
      const PendingPermission& pp = permission_prompts_.front();
      pending_perm = CefDictionaryValue::Create();
      pending_perm->SetInt("id", static_cast<int>(pp.ui_id));
      pending_perm->SetString("origin", pp.origin);
      CefRefPtr<CefListValue> types = CefListValue::Create();
      for (size_t t = 0; t < pp.labels.size(); ++t) {
        types->SetString(t, pp.labels[t]);
      }
      pending_perm->SetList("types", types);
      pending_perm->SetBool("media", pp.media);
    }
  }
  if (pending_perm) root_dict->SetDictionary("permissionPrompt", pending_perm);

  root_dict->SetBool("restoreSession", restore_session_);
  root_dict->SetBool("historyEnabled", history_enabled_);
  root_dict->SetInt("retentionDays", retention_days_);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!db_note_.empty()) root_dict->SetString("dbNote", db_note_);
  }

  // Phase 9.6: app update availability.
  auto upd = CefDictionaryValue::Create();
  upd->SetBool("available", update_available_);
  upd->SetString("version", update_version_);
  upd->SetString("url", update_url_);
  root_dict->SetDictionary("update", upd);

  // Phase 10: pending password save prompt (lives only in memory).
  if (!pw_prompt_.host.empty() && pw_prompt_.id) {
    auto pwp = CefDictionaryValue::Create();
    pwp->SetInt("id", static_cast<int>(pw_prompt_.id));
    pwp->SetString("host", pw_prompt_.host);
    pwp->SetString("username", pw_prompt_.user);
    root_dict->SetDictionary("passwordPrompt", pwp);
  }

  root_dict->SetBool("fullscreen", fullscreen_);
  root_dict->SetInt("zoomPercent", static_cast<int>(std::llround(std::exp(zoom_level_ * 0.1823215568) * 100.0)));
  root_dict->SetBool("lightTheme", light_theme_);
  root_dict->SetString("accent", accent_);  // Phase 9.5: user accent color
  root_dict->SetString("dnsResolver", dns_resolver_);
  root_dict->SetBool("findActive", find_active_);
  root_dict->SetInt("findCount", find_count_);
  root_dict->SetInt("findActiveMatch", find_active_match_);

  CefRefPtr<CefDictionaryValue> engine = CefDictionaryValue::Create();
  engine->SetString("id", engine_.id);
  engine->SetString("custom", engine_.custom);
  root_dict->SetDictionary("searchEngine", engine);

  CefRefPtr<CefListValue> links = CefListValue::Create();
  for (const QuickLink& link : quick_links_) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("title", link.title);
    item->SetString("url", link.url);
    AppendDict(links, item);
  }
  root_dict->SetList("quickLinks", links);

  // ---- Privacy / ad blocking state (Phase 3) ----
  CefRefPtr<CefDictionaryValue> privacy = CefDictionaryValue::Create();
  privacy->SetBool("enabled", privacy_.blocking_enabled());
  privacy->SetBool("thirdPartyCookies", privacy_.block_third_party_cookies());
  CefRefPtr<CefListValue> exceptions = CefListValue::Create();
  for (const auto& [host, allowed] : privacy_.SiteExceptions()) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("host", host);
    item->SetBool("allowed", allowed);
    AppendDict(exceptions, item);
  }
  privacy->SetList("siteExceptions", exceptions);
  CefRefPtr<CefListValue> list_items = CefListValue::Create();
  const std::vector<FilterListStatus> statuses = lists_.Statuses();
  for (size_t i = 0; i < statuses.size(); ++i) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("name", statuses[i].name);
    item->SetString("url", statuses[i].url);
    item->SetInt("rules", static_cast<int>(statuses[i].rules));
    item->SetString("updated", statuses[i].updated);
    item->SetString("status", statuses[i].status);
    list_items->SetDictionary(i, item);
  }
  privacy->SetList("lists", list_items);
  privacy->SetBool("updating", lists_.updating());
  {
    CefRefPtr<CefListValue> dis = CefListValue::Create();
    const auto disabled = privacy_.DisabledLists();
    for (size_t i = 0; i < disabled.size(); ++i) dis->SetString(i, disabled[i]);
    privacy->SetList("disabledLists", dis);
  }

  CefRefPtr<CefDictionaryValue> blocked = CefDictionaryValue::Create();
  CefRefPtr<CefDictionaryValue> tab_counts = CefDictionaryValue::Create();
  CefRefPtr<CefDictionaryValue> site_counts = CefDictionaryValue::Create();
  {
    std::lock_guard<std::mutex> lock(blocked_mutex_);
    blocked->SetInt("total", blocked_total_);
    for (const auto& [id, count] : tab_blocked_) {
      tab_counts->SetInt(std::to_string(id), count);
    }
    for (const auto& [host, count] : site_blocked_) {
      site_counts->SetInt(host, count);
    }
  }
  privacy->SetDictionary("blocked", blocked);
  privacy->SetDictionary("tabBlockedCounts", tab_counts);
  privacy->SetDictionary("siteBlockedCounts", site_counts);

  // Per-site context for the shield button (active tab).
  const std::wstring active_site = TabSiteHost(active_tab_id_);
  privacy->SetString("activeSite", active_site);
  privacy->SetBool("activeSiteAllowed",
                   !active_site.empty() && privacy_.IsSiteAllowed(active_site));
  root_dict->SetDictionary("privacy", privacy);

  if (focus_address) root_dict->SetBool("focusAddress", true);

  CefRefPtr<CefValue> root = CefValue::Create();
  root->SetDictionary(root_dict);
  return CefWriteJSON(root, cef_json_writer_options_t(0)).ToString();
}

void TabManager::PushUiEvent(bool focus_address) {
  if (focus_address) queued_focus_address_ = true;
  if (!ui_browser_) return;
  if (!waiters_.empty()) {
    const bool focus = queued_focus_address_;
    queued_focus_address_ = false;
    state_dirty_ = false;
    const std::string snapshot = BuildStateJson(focus);
    for (auto& [id, cb] : waiters_) {
      cb->Success(snapshot);
    }
    waiters_.clear();  // each page re-registers after processing
  } else {
    // No one is waiting; the next wait/getState delivers the snapshot.
    state_dirty_ = true;
  }
}

void TabManager::SaveSession() {
  // Phase 9: a --private window never writes session state at all — the
  // shared session.json stays untouched by private browsing.
  if (force_private_) return;
  CefRefPtr<CefDictionaryValue> root_dict = CefDictionaryValue::Create();
  CefRefPtr<CefListValue> tabs_list = CefListValue::Create();
  for (const Tab& tab : tabs_) {
    if (tab.closing || tab.private_flag) continue;
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("url", tab.url);
    AppendDict(tabs_list, item);
  }
  root_dict->SetList("tabs", tabs_list);
  root_dict->SetInt("active", static_cast<int>(active_tab_id_));
  root_dict->SetString("engineId", engine_.id);
  root_dict->SetString("engineCustom", engine_.custom);

  CefRefPtr<CefListValue> links = CefListValue::Create();
  for (const QuickLink& link : quick_links_) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("title", link.title);
    item->SetString("url", link.url);
    AppendDict(links, item);
  }
  root_dict->SetList("quickLinks", links);

  CefRefPtr<CefValue> root = CefValue::Create();
  root->SetDictionary(root_dict);
  const std::string json =
      CefWriteJSON(root, cef_json_writer_options_t(0)).ToString();
  std::ofstream file(profile_store_->session_path(),
                     std::ios::binary | std::ios::trunc);
  file.write(json.data(), static_cast<std::streamsize>(json.size()));
}

void TabManager::GetLayout(std::vector<int64_t>& order, int64_t& active_id) {
  ::EnterCriticalSection(&layout_lock_);
  order = layout_order_;
  active_id = layout_active_;
  ::LeaveCriticalSection(&layout_lock_);
}

// ---- Privacy / ad blocking (Phase 3) ---------------------------------------

std::wstring TabManager::TabSiteHost(int64_t tab_id) {
  // Thread-safe: reads the io_state_ mirror maintained on the CEF UI thread.
  std::lock_guard<std::mutex> lock(blocked_mutex_);
  const auto it = tab_site_.find(tab_id);
  return it == tab_site_.end() ? std::wstring() : it->second;
}

std::wstring TabManager::TabTitle(int64_t tab_id) {
  // CEF UI thread.
  for (const Tab& tab : tabs_) {
    if (tab.id == tab_id) return tab.title;
  }
  return std::wstring();
}

bool TabManager::ShouldBlockRequest(const std::wstring& url,
                                    const std::wstring& first_party,
                                    adblock::ResourceKind kind,
                                    std::string* rule) {
  // CEF IO thread.
  if (!privacy_.blocking_enabled() || first_party.empty()) return false;
  if (privacy_.IsSiteAllowed(first_party)) return false;
  const auto engine = lists_.engine();
  if (!engine) return false;

  adblock::RequestInfo req;
  req.url.resize(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    req.url[i] = (url[i] >= L'A' && url[i] <= L'Z') ? url[i] - L'A' + L'a'
                                                    : url[i];
  }
  req.host = adblock::UrlHost(url);
  req.first_party = first_party;
  req.kind = kind;
  req.third_party = !adblock::SameSiteApprox(req.host, first_party);
  if (!engine->ShouldBlock(req, rule)) return false;

  std::lock_guard<std::mutex> lock(blocked_mutex_);
  ++blocked_total_;
  ++site_blocked_[first_party];
  return true;
}

void TabManager::OnRequestBlocked(int64_t tab_id) {
  // CEF IO thread. Per-tab counter; the snapshot is pushed on the next
  // load-state change to avoid spamming the UI with an event per request.
  std::lock_guard<std::mutex> lock(blocked_mutex_);
  ++tab_blocked_[tab_id];
}

bool TabManager::CookieAllowed(const std::wstring& request_host,
                               const std::wstring& first_party) const {
  // CEF IO thread via the cookie access filter.
  if (first_party.empty()) return true;
  if (!privacy_.block_third_party_cookies()) return true;
  return adblock::SameSiteApprox(request_host, first_party);
}

void TabManager::ClearCookiesFor(const std::wstring& site) {
  // CEF UI thread.
  CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
  if (!manager) return;
  const std::wstring host = adblock::UrlHost(site);
  const std::wstring scheme = host.empty() ? std::wstring() : L"http://" + host;
  const std::wstring scheme_s =
      host.empty() ? std::wstring() : L"https://" + host;
  manager->DeleteCookies(scheme, L"", nullptr);
  manager->DeleteCookies(scheme_s, L"", nullptr);
  manager->FlushStore(nullptr);
}

void TabManager::ClearAllCookies() {
  // CEF UI thread. In-memory profile (cache_path is empty), so cookies are
  // the only persisted site data to clear.
  CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
  if (!manager) return;
  manager->DeleteCookies(L"", L"", nullptr);
  manager->FlushStore(nullptr);
}

void TabManager::UpdateFilterLists() {
  // CEF UI thread. PushUiEvent when the background update finishes so the
  // settings page shows the new statuses.
  lists_.UpdateAll([this]() {
    PushUiEvent();
  });
  PushUiEvent();  // reflect the "loading" status right away
}

// ---- Storage (Phase 4) ------------------------------------------------------


// ---- Phase 10 implementation ----------------------------------------------

bool TabManager::FillCredential(const std::string& host, std::string* user,
                                std::string* pass) {
  PostToDb([this, host, user, pass]() {
    if (!passwords_store_) return;
    std::vector<storage::PasswordRow> rows;
    std::string err;
    if (!passwords_store_->FindByOrigin(host, &rows, &err)) return;
    for (const auto& row : rows) {
      std::string secret;
      if (!DpapiUnprotect(row.secret, &secret)) continue;
      *user = row.username;
      *pass = secret;
      return;  // newest first
    }
  });
  return true;
}

void TabManager::PromptSavePassword(const std::string& host,
                                    const std::string& user,
                                    const std::string& pass) {
  PostToCef([this, host, user, pass]() {
    pw_prompt_.id = pw_prompt_next_++;
    pw_prompt_.host = host;
    pw_prompt_.user = user;
    pw_prompt_.pass = pass;
    PushUiEvent();
  });
}

void TabManager::ResolvePasswordPrompt(int64_t prompt_id, bool save) {
  if (pw_prompt_.id != prompt_id) return;
  if (save && !pw_prompt_.host.empty() && !pw_prompt_.pass.empty()) {
    const std::string secret = DpapiProtect(pw_prompt_.pass);
    if (!secret.empty()) {
      PostToDb([this, host = pw_prompt_.host, user = pw_prompt_.user,
                secret]() {
        if (!passwords_store_) return;
        std::string err;
        passwords_store_->Add(host, user, secret, nullptr, &err);
      });
    }
  }
  pw_prompt_ = {};
  PushUiEvent();
}


void TabManager::CheckAppUpdate(bool manual) {
  dask::CheckForUpdate(manual);
}

void TabManager::UpdateAvailable(std::string tag, std::string url,
                                 bool available, bool failed) {
  // CEF UI thread (posted from the updater thread).
  update_available_ = available;
  update_version_ = tag;
  update_url_ = url;
  (void)failed;
  if (available) PushUiEvent();
}


bool TabManager::HasSavedPassword(const std::string& host) {
  // CEF UI thread. pw_hosts_ is a fast "definitely has credential" cache
  // (updated whenever we see a pwSubmit or load the DB list). OnLoadEnd
  // uses it to skip the injection on hosts that have never had a password.
  // We also kick off an async DB probe that reinjects the filler when it
  // confirms a match (covers the cold-start case).
  if (pw_hosts_.count(host)) return true;
  PostToDb([this, host]() {
    bool found = false;
    if (passwords_store_) {
      std::vector<storage::PasswordRow> rows;
      std::string err;
      if (passwords_store_->FindByOrigin(host, &rows, &err) && !rows.empty())
        found = true;
    }
    if (!found) return;
    PostToCef([this, host]() {
      pw_hosts_.insert(host);
      for (Tab& tab : tabs_) {
        if (tab.browser && !tab.internal) {
          CefRefPtr<CefFrame> frame = tab.browser->GetMainFrame();
          const std::wstring wurl = frame->GetURL().ToWString();
          if (HostOf(wurl) == host)
            frame->ExecuteJavaScript(CefString(FillerScript(host)),
                                     frame->GetURL(), 0);
        }
      }
    });
  });
  return false;
}

std::vector<std::string> TabManager::ScriptsForHost(const std::string& host) {
  // CEF UI thread (also called from OnQuery).
  std::vector<std::string> codes;
  std::lock_guard<std::mutex> lock(state_mutex_);
  for (const ScriptUi& s : scripts_cache_) {
    if (s.enabled && ScriptMatches(s.matches, host)) codes.push_back(s.code);
  }
  return codes;
}

void TabManager::StartDatabase() {
  // CEF UI thread. The DB thread owns the SQLite connection.
  const std::string db_path = Utf8FromWide(profile_store_->db_path());
  db_quit_ = false;
  db_thread_ = std::thread([this, db_path]() {
    db_ = std::make_unique<storage::Database>();
    std::string err;
    if (!db_->Open(db_path, &err) || !db_->Migrate(&err)) {
      if (db_->corrupted()) {
        // Understandable message + fresh start: the corrupted file is gone.
        std::string recover;
        if (db_->Recreate(&recover)) {
          err.clear();
        } else {
          err = "storage failure: " + recover;
        }
      }
      if (!err.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        db_note_ = L"تعذّر فتح قاعدة البيانات المحلية (سيبدأ التخزين من جديد): " +
                   WideFromUtf8(err);
      }
    }
    if (db_->opened()) {
      bookmarks_store_ = std::make_unique<storage::Bookmarks>(db_.get());
      history_store_ = std::make_unique<storage::History>(db_.get());
      downloads_store_ = std::make_unique<storage::Downloads>(db_.get());
      settings_store_ = std::make_unique<storage::Settings>(db_.get());
      permissions_store_ = std::make_unique<storage::Permissions>(db_.get());
      passwords_store_ = std::make_unique<storage::Passwords>(db_.get());
      user_scripts_store_ = std::make_unique<storage::UserScripts>(db_.get());

      std::string db_err;
      settings_store_->GetBool("restoreSession", false, &restore_session_,
                               &db_err);
      settings_store_->GetBool("historyEnabled", true, &history_enabled_,
                               &db_err);
      settings_store_->GetInt("historyRetentionDays", 90, &retention_days_,
                              &db_err);
      settings_store_->GetBool("lightTheme", false, &light_theme_, &db_err);
      settings_store_->GetString("dnsResolver", "", &dns_resolver_, &db_err);
      if (history_enabled_ && retention_days_ > 0) {
        const int64_t now = static_cast<int64_t>(time(nullptr));
        history_store_->Prune(now - retention_days_ * 86400, &db_err);
      }
      // First run: seed bookmarks with the default quick links.
      std::vector<storage::Bookmark> seeded;
      bookmarks_store_->Search("", &seeded, &db_err);
      if (seeded.empty()) {
        for (const QuickLink& link : kDefaultLinks) {
          int64_t sid = 0;
          bookmarks_store_->Add(Utf8FromWide(link.title),
                                Utf8FromWide(link.url), &sid, &db_err);
        }
      }
      RefreshStorageCachesOnDb();
    }

    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(db_mutex_);
        db_cv_.wait(lock, [this]() { return db_quit_ || !db_queue_.empty(); });
        if (db_quit_) break;
        if (db_queue_.empty()) continue;
        task = std::move(db_queue_.front());
        db_queue_.pop_front();
      }
      task();
    }

    // Tear down on this thread (SQLite objects are thread-confined).
    bookmarks_store_.reset();
    history_store_.reset();
    downloads_store_.reset();
    settings_store_.reset();
    permissions_store_.reset();
    db_.reset();
  });
}

void TabManager::StopDatabase() {
  if (!db_thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(db_mutex_);
    db_quit_ = true;
  }
  db_cv_.notify_all();
  db_thread_.join();
}

void TabManager::PostToDb(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(db_mutex_);
    db_queue_.push_back(std::move(fn));
  }
  db_cv_.notify_all();
}

// DB thread: rebuild the UI-facing mirrors and hand them to the CEF UI
// thread.
void TabManager::RefreshStorageCachesOnDb() {
  if (!bookmarks_store_ || !downloads_store_ || !permissions_store_) return;
  std::string err;
  std::vector<storage::Bookmark> bm_rows;
  bookmarks_store_->Search("", &bm_rows, &err);
  std::vector<storage::DownloadItemRow> dl_rows;
  downloads_store_->List(30, &dl_rows, &err);
  std::vector<storage::PermissionRow> perm_rows;
  permissions_store_->List(&perm_rows, &err);
  if (!err.empty()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    db_note_ = L"خطأ في قاعدة البيانات: " + WideFromUtf8(err);
  }
  PostToCef([this, bm_rows = std::move(bm_rows),
             dl_rows = std::move(dl_rows),
             perm_rows = std::move(perm_rows)]() {
    std::vector<BookmarkUi> bm;
    for (const storage::Bookmark& row : bm_rows) {
      BookmarkUi item;
      item.id = row.id;
      item.title = WideFromUtf8(row.title);
      item.url = WideFromUtf8(row.url);
      bm.push_back(std::move(item));
    }
    std::vector<DownloadUi> dl;
    for (const storage::DownloadItemRow& row : dl_rows) {
      DownloadUi item;
      item.id = row.id;
      item.url = WideFromUtf8(row.url);
      item.path = WideFromUtf8(row.path);
      item.filename = WideFromUtf8(row.filename);
      item.state = row.state;
      item.received = row.received;
      item.total = row.total;
      item.risky = IsRiskyDownloadFilename(WideFromUtf8(row.filename));
      dl.push_back(std::move(item));
    }
    std::vector<PermissionUi> perms;
    for (const storage::PermissionRow& row : perm_rows) {
      PermissionUi item;
      item.host = WideFromUtf8(row.host);
      item.type = WideFromUtf8(row.type);
      item.decision = row.decision;
      perms.push_back(std::move(item));
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      bookmarks_cache_ = std::move(bm);
      downloads_cache_ = std::move(dl);
      permissions_cache_ = std::move(perms);
    }
    PushUiEvent();
  });
}

void TabManager::RecordVisit(int64_t tab_id, const std::wstring& url,
                             const std::wstring& title) {
  // CEF UI thread.
  if (!history_enabled_) return;
  bool private_tab = false;
  for (const Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      private_tab = tab.private_flag || tab.internal;
      break;
    }
  }
  if (private_tab) return;  // private tabs never reach storage
  if (!(url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0)) {
    return;
  }
  PostToDb([this, url = Utf8FromWide(url), title = Utf8FromWide(title)]() {
    if (!history_store_) return;
    int64_t id = 0;
    std::string err;
    history_store_->Add(url, title, static_cast<int64_t>(time(nullptr)), &id,
                        &err);
  });
}

void TabManager::ToggleBookmarkActive() {
  // CEF UI thread (Ctrl+D).
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || tab->internal || tab->private_flag || tab->closing) return;
  if (!(tab->url.rfind(L"http://", 0) == 0 ||
        tab->url.rfind(L"https://", 0) == 0)) {
    return;
  }
  ToggleBookmark(tab->title.empty() ? tab->url : tab->title, tab->url);
}

void TabManager::ToggleBookmark(const std::wstring& title,
                                const std::wstring& url) {
  // CEF UI thread.
  const std::string url_utf8 = Utf8FromWide(url);
  bool exists = false;
  int64_t existing_id = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const BookmarkUi& bm : bookmarks_cache_) {
      if (bm.url == url) {
        exists = true;
        existing_id = bm.id;
        break;
      }
    }
  }
  PostToDb([this, exists, existing_id, url_utf8,
            title_utf8 = Utf8FromWide(title)]() {
    if (!bookmarks_store_) return;
    std::string err;
    if (exists) {
      bookmarks_store_->Remove(existing_id, &err);
    } else {
      int64_t id = 0;
      bookmarks_store_->Add(title_utf8, url_utf8, &id, &err);
    }
    RefreshStorageCachesOnDb();
  });
}

void TabManager::RemoveBookmark(int64_t id) {
  PostToDb([this, id]() {
    if (!bookmarks_store_) return;
    std::string err;
    bookmarks_store_->Remove(id, &err);
    RefreshStorageCachesOnDb();
  });
}

void TabManager::UpdateBookmark(int64_t id, const std::wstring& title,
                                const std::wstring& url) {
  PostToDb([this, id, title = Utf8FromWide(title),
            url = Utf8FromWide(url)]() {
    if (!bookmarks_store_) return;
    std::string err;
    bookmarks_store_->Update(id, title, url, &err);
    RefreshStorageCachesOnDb();
  });
}

void TabManager::ClearHistory(int days) {
  PostToDb([this, days]() {
    if (!history_store_) return;
    std::string err;
    history_store_->Clear(days, &err);
  });
}

void TabManager::SetHistoryEnabled(bool enabled) {
  history_enabled_ = enabled;
  PostToDb([this, enabled]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetBool("historyEnabled", enabled, &err);
  });
}

void TabManager::SetRetentionDays(int days) {
  retention_days_ = days;
  PostToDb([this, days]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetInt("historyRetentionDays", days, &err);
    if (history_enabled_ && days > 0) {
      history_store_->Prune(static_cast<int64_t>(time(nullptr)) -
                                static_cast<int64_t>(days) * 86400,
                            &err);
    }
  });
}

void TabManager::OnFindResult(int count, int active) {
  // Runs on the CEF UI thread; do not hold state_mutex_ across PushUiEvent
  // (BuildStateJson reads the same fields on this thread).
  find_count_ = count;
  find_active_match_ = active;
  PushUiEvent();
}

namespace {
// Phase 7 helper: UTF-8 path for storage helpers (file-local).
std::string WstrToUtf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr,
                              0, nullptr, nullptr);
  std::string out(n, 0);
  if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(),
                             n, nullptr, nullptr);
  return out;
}

}  // namespace

// Extracts the host from a URL (lowercase, no port).
std::string HostOf(const std::wstring& wurl) {
  CefURLParts parts;
  if (!CefParseURL(CefString(wurl), parts)) return {};
  const std::string host = CefString(&parts.host).ToString();
  const size_t colon = host.find(':');
  std::string clean = colon == std::string::npos ? host : host.substr(0, colon);
  std::transform(clean.begin(), clean.end(), clean.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return clean;
}

// The password filler bootstrap. Runs in the PAGE context and talks to the
// browser through the restricted pwQuery/pwSubmit channel. The native side
// only answers pwQuery for the frame's own host, only when a saved
// credential exists, and only once per frame load.
std::string FillerScript(const std::string& host) {
  std::string js = R"js((function(){
  var HOST = )js";
  js += "\"" + host + "\"";
  js += R"js(;
  function q(type, data, cb){
    if(!window.cefQuery) return;
    window.cefQuery({
      request: JSON.stringify(Object.assign({type:type, host:HOST}, data||{})),
      persistent:false,
      onSuccess:function(r){ try{ cb(JSON.parse(r)); }catch(e){} },
      onFailure:function(){}
    });
  }
  function pwField(){
    var pw = document.querySelectorAll('input[type=password]');
    for (var i=0;i<pw.length;i++){
      var el = pw[i];
      var r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) return el;
    }
    return pw[0] || null;
  }
  function userFieldFor(pw){
    var form = pw.closest('form') || document;
    var inputs = form.querySelectorAll('input:not([type=password]):not([type=hidden])');
    var best = null;
    for (var i=0;i<inputs.length;i++){
      var t = (inputs[i].type||'text').toLowerCase();
      if (t==='text'||t==='email'||t==='tel'){ best = inputs[i]; break; }
    }
    return best;
  }
  function fill(user, pass){
    var el = pwField(); if(!el) return;
    var uf = userFieldFor(el);
    if (uf && user){ uf.value = user; uf.dispatchEvent(new Event('input',{bubbles:true})); }
    el.value = pass; el.dispatchEvent(new Event('input',{bubbles:true}));
  }
  function hookSubmit(){
    var el = pwField(); if(!el) return;
    var form = el.closest('form');
    var target = form || document;
    var handler = function(){
      var e = pwField(); if(!e) return;
      if (!e.value) return;
      var uf = userFieldFor(e);
      q('pwSubmit', {username: uf ? uf.value : '', password: e.value});
    };
    target.addEventListener('submit', handler, {once:true});
  }
  q('pwQuery', null, function(r){
    if (r && r.user){ fill(r.user, r.pass); }
    hookSubmit();
  });
})();)js";
  return js;
}


void TabManager::OpenFindBar() {
  // Called from the Win32 keyboard-hook thread: set the flag on the CEF
  // UI thread via PostToCef, never touch waiter state cross-thread.
  PostToCef([this]() {
    find_active_ = true;
    PushUiEvent();
  });
}

// Phase 7: minimal client for the menu popup browser. The menu page talks
// through the shared message router; closing destroys its popup host window.
class MenuClient : public CefClient, public CefLifeSpanHandler {
 public:
  explicit MenuClient(TabManager* mgr) : mgr_(mgr) {}
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    mgr_->OnMenuBrowserCreated(browser);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    return mgr_->router()->OnProcessMessageReceived(browser, frame,
                                                    source_process, message);
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    mgr_->OnMenuBrowserClosed();
    (void)browser;
  }

 private:
  TabManager* mgr_;
  IMPLEMENT_REFCOUNTING(MenuClient);
  DISALLOW_COPY_AND_ASSIGN(MenuClient);
};

void TabManager::ShowAppMenu() {
  PostToCef([this]() { ShowAppMenuInternal(); });
}

void TabManager::ShowAppMenuInternal() {
  if (menu_open_) return;
  // Size and position the popup under the toolbar (RTL: left edge).
  HWND popup = reinterpret_cast<HWND>(::SendMessageW(
      main_hwnd_, WM_DASK_MENU_HOST, 0, 0));
  if (!popup) return;
  menu_popup_hwnd_ = popup;

  CefWindowInfo info;
  RECT rc;
  ::GetClientRect(popup, &rc);
  info.SetAsChild(popup,
                  CefRect(0, 0, rc.right - rc.left, rc.bottom - rc.top));
  CefRefPtr<MenuClient> client = new MenuClient(this);
  CefBrowserSettings settings;
  CefString url;
  url.FromWString(L"http://dask.ui/menu.html");
  CefBrowserHost::CreateBrowser(info, client, url, settings, nullptr,
                                nullptr);
  menu_open_ = true;
}

void TabManager::CloseAppMenu() {
  PostToCef([this]() { CloseAppMenuInternal(); });
}

void TabManager::CloseAppMenuInternal() {
  if (!menu_open_) return;
  menu_open_ = false;
  menu_deactivated_at_ = 0;  // explicit close: no hamburger-click debounce
  if (menu_browser_) {
    // OnBeforeClose tears down the popup host window (menu browser path).
    menu_browser_->GetHost()->CloseBrowser(true);
  } else {
    // Browser still initializing: destroy the popup host on its own thread.
    ::SendMessageW(main_hwnd_, WM_DASK_DESTROY_MENU_HOST, 0, 0);
    menu_browser_ = nullptr;
  }
}

void TabManager::ToggleAppMenu() {
  PostToCef([this]() {
    if (menu_open_) {
      CloseAppMenuInternal();
      return;
    }
    // Ignore the toggle sent by the same hamburger click that deactivated
    // (and thereby closed) the popup a moment ago.
    if (menu_deactivated_at_ != 0 &&
        ::GetTickCount() - menu_deactivated_at_ < 400) {
      return;
    }
    ShowAppMenuInternal();
  });
}

void TabManager::ResizeMenuPopup(bool wide, int top, int height) {
  // Phase 7.5: expand the popup beside the menu column for the hover flyout.
  // top/height describe the flyout's page rect so the Win32 thread can clip
  // the window (SetWindowRgn) to just the menu column + the flyout panel.
  PostToCef([this, wide, top, height]() {
    if (!menu_open_ || !menu_popup_hwnd_) return;
    const LPARAM lp =
        static_cast<LPARAM>((static_cast<unsigned>(top) & 0xFFFF) |
                            (static_cast<unsigned>(height) << 16));
    ::PostMessageW(main_hwnd_, WM_DASK_MENU_WIDE,
                   wide ? 1 : 0, lp);
  });
}

void TabManager::OnMenuDeactivated() {
  // Called on the Win32 thread when the popup lost activation.
  const DWORD now = ::GetTickCount();
  PostToCef([this, now]() {
    if (!menu_open_) return;
    menu_deactivated_at_ = now;
    CloseAppMenuInternal();
  });
}

void TabManager::OnMenuBrowserCreated(CefRefPtr<CefBrowser> browser) {
  menu_browser_ = browser;
}

void TabManager::OnMenuBrowserClosed() {
  // Runs on the CEF UI thread. Destroy the popup host on the Win32 thread
  // and clear state.
  menu_browser_ = nullptr;
  menu_popup_hwnd_ = nullptr;
  if (menu_open_) menu_open_ = false;
  ::SendMessageW(main_hwnd_, WM_DASK_DESTROY_MENU_HOST, 0, 0);
}

// ---- Main menu commands (Phase 7) ------------------------------------------

bool TabManager::force_private_ = false;

void TabManager::SetPrefString(const std::string& key,
                               const std::string& value) {
  PostToDb([this, key, value]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetString(key, value, &err);
  });
}
bool TabManager::launch_app_ = false;
std::wstring TabManager::launch_url_;

void TabManager::NewBrowserWindow(bool private_window) {
  // A real top-level window: a second browser process sharing the same
  // user data (profiles dir). The private variant runs with --private so
  // ALL its tabs are private (no session, no history, no permission store).
  wchar_t exe[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring args =
      private_window ? L"--private" : L"";

  // Launch WITHOUT inheriting this process's diagnostic environment:
  // DASK_LOG=1 enables remote_debugging_port=9222 and a second instance
  // cannot bind the same port (CefInitialize would fail). Build a fresh
  // environment block that strips DASK_* debug variables.
  std::vector<wchar_t> env_block;
  {
    wchar_t* strings = ::GetEnvironmentStringsW();
    if (strings) {
      for (wchar_t* p = strings; *p;) {
        const std::wstring entry(p);
        p += entry.size() + 1;
        if (entry.rfind(L"DASK_", 0) != 0) {
          env_block.insert(env_block.end(), entry.begin(), entry.end());
          env_block.push_back(L'\0');
        }
      }
      ::FreeEnvironmentStringsW(strings);
      env_block.push_back(L'\0');
    }
  }

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = std::wstring(L"\"") + exe + L"\"" +
                     (args.empty() ? L"" : L" " + args);
  const BOOL started = ::CreateProcessW(
      nullptr, cmd.data(), nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS,
      env_block.empty() ? nullptr : env_block.data(), nullptr, &si, &pi);
  if (started) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
  } else {
    std::lock_guard<std::mutex> lock(state_mutex_);
    db_note_ = L"تعذّر فتح نافذة جديدة.";
    PushUiEvent();
  }
}

void TabManager::AdjustZoom(double step) {
  // Chromium zoom level units: level = ln(scale) / ln(1.2). 0 = 100%.
  // Clamp to roughly 25% .. 500%. Applies to any active page, including
  // internal dask.ui pages (like Chromium's own zoom on the NTP).
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser) return;
  if (step == 0.0) {
    zoom_level_ = 0.0;
  } else {
    zoom_level_ = std::clamp(zoom_level_ + step, -6.6, 5.2);
  }
  tab->browser->GetHost()->SetZoomLevel(zoom_level_);
  PushUiEvent();
}

void TabManager::ToggleFullscreen() {
  // Borderless fullscreen: strip the window style + cover the monitor.
  const HWND hwnd = main_hwnd_;
  if (!hwnd) return;
  static WINDOWPLACEMENT saved = {};
  saved.length = sizeof(WINDOWPLACEMENT);
  const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (!fullscreen_) {
    ::GetWindowPlacement(hwnd, &saved);
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME));
    const HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi)) {
      ::SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = true;
  } else {
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CAPTION | WS_THICKFRAME);
    ::SetWindowPlacement(hwnd, &saved);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_FRAMECHANGED);
    fullscreen_ = false;
  }
  PushUiEvent();
}

void TabManager::FindInPage(const std::string& text, bool forward,
                            bool find_next) {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser || tab->internal) return;
  CefString query;
  query.FromString(text);
  tab->browser->GetHost()->Find(query, forward, /*matchCase=*/false,
                                find_next);
}

void TabManager::StopFind() {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser) return;
  tab->browser->GetHost()->StopFinding(/*clearSelection=*/true);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    find_active_ = false;
    find_count_ = 0;
    find_active_match_ = 0;
  }
  PushUiEvent();
}

void TabManager::ShowDevTools() {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser || tab->internal) return;
  if (tab->browser->GetHost()->HasDevTools()) {
    tab->browser->GetHost()->CloseDevTools();
  } else {
    tab->browser->GetHost()->ShowDevTools(CefWindowInfo(), nullptr,
                                          CefBrowserSettings(),
                                          CefPoint());
  }
}

void TabManager::SavePageAsPdf() {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser || tab->internal) return;
  // Name the PDF after the page title, sanitized like downloads.
  std::wstring name = tab->title.empty() ? L"صفحة" : tab->title;
  for (wchar_t& c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
  }
  if (name.size() > 120) name.resize(120);
  const std::wstring path =
      storage::UniqueDownloadPath(DownloadsDir(),
          WstrToUtf8(name) + ".pdf");
  tab->browser->GetHost()->PrintToPDF(path, CefPdfPrintSettings(), nullptr);
}

void TabManager::PrintPage() {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || !tab->browser || tab->internal) return;
  // Chromium's print pipeline handles window.print() (preview + dialog).
  tab->browser->GetMainFrame()->ExecuteJavaScript(
      CefString("window.print()"), tab->browser->GetMainFrame()->GetURL(), 0);
}

void TabManager::CopyPageLink() {
  Tab* tab = FindTab(active_tab_id_);
  if (!tab || tab->url.empty()) return;
  CopyTextToClipboard(tab->url);
}

void TabManager::CopyTextToClipboard(const std::wstring& text) {
  // Generic clipboard write (context-menu link/image URL copies too).
  if (text.empty()) return;
  if (!::OpenClipboard(main_hwnd_)) return;
  const HGLOBAL mem = ::GlobalAlloc(
      GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (mem) {
    void* dst = ::GlobalLock(mem);
    if (dst) {
      memcpy(dst, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
      ::GlobalUnlock(mem);
      ::EmptyClipboard();
      ::SetClipboardData(CF_UNICODETEXT, mem);
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
}

// ---- Brave-style Arabic context menu (Phase 8) ---------------------------

namespace {
// Custom context-menu command ids (kept far from CEF's built-in range).
constexpr int kCtxOpenLinkTab = 4001;
constexpr int kCtxCopyLink = 4002;
constexpr int kCtxSaveLink = 4003;
constexpr int kCtxOpenImageTab = 4004;
constexpr int kCtxSaveImage = 4005;
constexpr int kCtxCopyImage = 4006;
constexpr int kCtxCut = 4007;
constexpr int kCtxCopy = 4008;
constexpr int kCtxPaste = 4009;
constexpr int kCtxBack = 4010;
constexpr int kCtxForward = 4011;
constexpr int kCtxReload = 4012;
constexpr int kCtxSavePdf = 4013;
constexpr int kCtxPrint = 4014;
constexpr int kCtxFind = 4015;
constexpr int kCtxViewSource = 4016;
constexpr int kCtxInspect = 4017;

// Delivers a Ctrl+<key> editing keystroke into the given browser (the menu
// target browser keeps focus, so the renderer's editing pipeline applies).
void SendEditKey(CefRefPtr<CefBrowser> browser, wchar_t key) {
  if (!browser) return;
  CefKeyEvent raw_down;
  raw_down.type = KEYEVENT_RAWKEYDOWN;
  raw_down.windows_key_code = key;
  raw_down.modifiers = EVENTFLAG_CONTROL_DOWN;
  browser->GetHost()->SendKeyEvent(raw_down);
  CefKeyEvent ch = raw_down;
  ch.type = KEYEVENT_CHAR;
  ch.character = key;
  browser->GetHost()->SendKeyEvent(ch);
  CefKeyEvent up = raw_down;
  up.type = KEYEVENT_KEYUP;
  browser->GetHost()->SendKeyEvent(up);
}

}  // namespace

void TabManager::BuildContextMenu(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefContextMenuParams> params,
                                  CefRefPtr<CefMenuModel> model) {
  // Replace Chromium's default items entirely — our own Brave-style menu.
  model->Clear();
  const bool has_link = !params->GetLinkUrl().empty();
  const bool has_selection = !params->GetSelectionText().empty();
  const bool editable = params->IsEditable();
  const bool image = params->GetMediaType() == CM_MEDIATYPE_IMAGE;

  if (has_link) {
    model->AddItem(kCtxOpenLinkTab, L"فتح الرابط في تبويب جديد");
    model->AddItem(kCtxCopyLink, L"نسخ عنوان الرابط");
    model->AddItem(kCtxSaveLink, L"حفظ الرابط باسم…");
    model->AddSeparator();
  }
  if (image) {
    model->AddItem(kCtxOpenImageTab, L"فتح الصورة في تبويب جديد");
    model->AddItem(kCtxSaveImage, L"حفظ الصورة باسم…");
    model->AddItem(kCtxCopyImage, L"نسخ عنوان الصورة");
    model->AddSeparator();
  }
  if (editable) {
    model->AddItem(kCtxCut, L"قص");
    model->SetAccelerator(kCtxCut, 'X', false, true, false);
    model->AddItem(kCtxCopy, L"نسخ");
    model->SetAccelerator(kCtxCopy, 'C', false, true, false);
    model->AddItem(kCtxPaste, L"لصق");
    model->SetAccelerator(kCtxPaste, 'V', false, true, false);
    model->AddSeparator();
  } else if (has_selection) {
    model->AddItem(kCtxCopy, L"نسخ");
    model->SetAccelerator(kCtxCopy, 'C', false, true, false);
    model->AddSeparator();
  }
  if (browser->CanGoBack()) model->AddItem(kCtxBack, L"للخلف");
  if (browser->CanGoForward()) model->AddItem(kCtxForward, L"للأمام");
  if (browser->CanGoBack() || browser->CanGoForward()) model->AddSeparator();
  model->AddItem(kCtxReload, L"إعادة التحميل");
  model->AddItem(kCtxSavePdf, L"حفظ الصفحة باسم… (PDF)");
  model->AddItem(kCtxPrint, L"طباعة");
  model->AddItem(kCtxFind, L"بحث في الصفحة");
  model->AddSeparator();
  model->AddItem(kCtxViewSource, L"عرض المصدر");
  model->AddItem(kCtxInspect, L"فحص العنصر");
}

bool TabManager::HandleContextMenuCommand(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefContextMenuParams> params,
    int command_id) {
  switch (command_id) {
    case kCtxOpenLinkTab:
      CreateTab(params->GetLinkUrl());
      return true;
    case kCtxCopyLink:
      CopyTextToClipboard(params->GetLinkUrl().ToWString());
      return true;
    case kCtxSaveLink:
      browser->GetHost()->StartDownload(params->GetLinkUrl());
      return true;
    case kCtxOpenImageTab:
      CreateTab(params->GetSourceUrl());
      return true;
    case kCtxSaveImage:
      browser->GetHost()->StartDownload(params->GetSourceUrl());
      return true;
    case kCtxCopyImage:
      CopyTextToClipboard(params->GetSourceUrl().ToWString());
      return true;
    case kCtxCut:
      SendEditKey(browser, L'X');
      return true;
    case kCtxCopy:
      SendEditKey(browser, L'C');
      return true;
    case kCtxPaste:
      SendEditKey(browser, L'V');
      return true;
    case kCtxBack:
      browser->GoBack();
      return true;
    case kCtxForward:
      browser->GoForward();
      return true;
    case kCtxReload:
      browser->Reload();
      return true;
    case kCtxSavePdf:
      SavePageAsPdf();
      return true;
    case kCtxPrint:
      PrintPage();
      return true;
    case kCtxFind:
      OpenFindBar();
      return true;
    case kCtxViewSource: {
      const std::wstring url =
          browser->GetMainFrame()->GetURL().ToWString();
      if (!url.empty()) CreateTab(L"view-source:" + url);
      return true;
    }
    case kCtxInspect:
      ShowDevTools();
      return true;
    default:
      return false;
  }
}

void TabManager::ExitApp() {
  // Same safe path as the window close button.
  ::PostMessageW(main_hwnd_, WM_CLOSE, 0, 0);
}


// ---- Phase 10: DPAPI helpers (local-only password encryption) -------------

// Encrypts plaintext with the current Windows user's DPAPI key. The blob is
// only decryptable under the same Windows account on this machine.
std::string DpapiProtect(const std::string& plain) {
  DATA_BLOB in{}, out{};
  in.cbData = static_cast<DWORD>(plain.size());
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
  if (!::CryptProtectData(&in, L"DASK.PW.v1", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return {};
  }
  std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  // base64
  static const char* kAl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve((blob.size() + 2) / 3 * 4);
  for (size_t i = 0; i < blob.size(); i += 3) {
    const unsigned n = (static_cast<unsigned char>(blob[i]) << 16) |
        (i + 1 < blob.size() ? static_cast<unsigned char>(blob[i + 1]) << 8 : 0) |
        (i + 2 < blob.size() ? static_cast<unsigned char>(blob[i + 2]) : 0);
    b64 += kAl[(n >> 18) & 63]; b64 += kAl[(n >> 12) & 63];
    b64 += i + 1 < blob.size() ? kAl[(n >> 6) & 63] : '=';
    b64 += i + 2 < blob.size() ? kAl[n & 63] : '=';
  }
  return b64;
}

bool DpapiUnprotect(const std::string& b64, std::string* plain) {
  // base64 -> bytes
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string blob;
  blob.reserve(b64.size() / 4 * 3);
  int acc = 0, bits = 0;
  for (char c : b64) {
    const int v = val(c);
    if (v < 0) continue;  // skip '=' and whitespace
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      blob.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  DATA_BLOB in{}, out{};
  in.cbData = static_cast<DWORD>(blob.size());
  in.pbData = reinterpret_cast<BYTE*>(blob.data());
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    return false;
  }
  plain->assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return true;
}

// Does a userscript match pattern cover this host? Patterns: "host",
// "*.host", "suffix.*" (subdomain tail) — kept deliberately simple.
bool ScriptMatches(const std::string& patterns, const std::string& host) {
  size_t start = 0;
  while (start <= patterns.size()) {
    size_t comma = patterns.find(',', start);
    std::string pat = patterns.substr(start,
        (comma == std::string::npos ? patterns.size() : comma) - start);
    start = comma == std::string::npos ? patterns.size() + 1 : comma + 1;
    // trim
    const auto issp = [](char c) { return c == ' ' || c == '	'; };
    while (!pat.empty() && issp(pat.front())) pat.erase(pat.begin());
    while (!pat.empty() && issp(pat.back())) pat.pop_back();
    if (pat.empty()) continue;
    if (pat == "*" || pat == host) return true;
    if (pat.size() > 2 && pat[0] == '*' && pat[1] == '.') {
      // "*.example.com" matches example.com + subdomains
      const std::string tail = pat.substr(1);
      if (host.size() >= tail.size() &&
          host.compare(host.size() - tail.size(), tail.size(), tail) == 0) {
        return true;
      }
    }
  }
  return false;
}

std::string TabManager::ReadPrefString(const std::string& key) {
  // Synchronous read used before the DB thread starts (browser startup).
  ProfileStore store(AppDataDir());
  storage::Database db;
  std::string err;
  if (!db.Open(Utf8FromWide(store.db_path()), &err) || !db.Migrate(&err)) {
    return {};
  }
  storage::Settings prefs(&db);
  std::string value;
  if (!prefs.GetString(key, "", &value, &err)) value.clear();
  return value;
}

bool TabManager::ReadPrefBool(const std::string& key) {
  // Synchronous read used before the DB thread starts (browser startup).
  ProfileStore store(AppDataDir());
  storage::Database db;
  std::string err;
  if (!db.Open(Utf8FromWide(store.db_path()), &err) || !db.Migrate(&err)) {
    return false;
  }
  storage::Settings prefs(&db);
  bool value = false;
  prefs.GetBool(key, false, &value, &err);
  return value;
}

void TabManager::SetDnsResolver(const std::string& resolver) {
  // Phase 7.5: DoH provider choice. Chromium's DoH is a network-service
  // switch, so a change takes effect on the next launch (stated honestly
  // in the settings UI).
  dns_resolver_ = resolver;
  PostToDb([this, resolver]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetString("dnsResolver", resolver, &err);
  });
  PushUiEvent();
}

void TabManager::SetListEnabled(const std::string& name, bool enabled) {
  privacy_.SetListEnabled(CefString(name).ToWString(), enabled);
  privacy_.Save();
  lists_.SetDisabledLists(privacy_.DisabledLists());
  lists_.RebuildEngine();  // atomic engine swap on this (CEF UI) thread
  PushUiEvent();
}

void TabManager::ViewSourceActive() {
  PostToCef([this]() {
    Tab* tab = FindTab(active_tab_id_);
    if (!tab || tab->internal || tab->url.empty()) return;
    CreateTab(L"view-source:" + tab->url);
  });
}

void TabManager::EditCommandActive(const std::string& cmd) {
  // Phase 7.5: menu-driven cut/copy/paste into the focused web content.
  // Delivered as a real Ctrl+X/C/V key sequence so the renderer's own
  // editing pipeline handles it (selection, inputs, textareas).
  wchar_t key = 0;
  if (cmd == "cut") key = L'X';
  else if (cmd == "copy") key = L'C';
  else if (cmd == "paste") key = L'V';
  if (!key) return;
  PostToCef([this, key]() {
    Tab* tab = FindTab(active_tab_id_);
    if (!tab || !tab->browser) return;
    CefKeyEvent raw_down;
    raw_down.type = KEYEVENT_RAWKEYDOWN;
    raw_down.windows_key_code = key;
    raw_down.modifiers = EVENTFLAG_CONTROL_DOWN;
    tab->browser->GetHost()->SendKeyEvent(raw_down);
    CefKeyEvent ch = raw_down;
    ch.type = KEYEVENT_CHAR;
    ch.character = key;
    tab->browser->GetHost()->SendKeyEvent(ch);
    CefKeyEvent up = raw_down;
    up.type = KEYEVENT_KEYUP;
    tab->browser->GetHost()->SendKeyEvent(up);
  });
}

// ---- Launcher shortcuts (.lnk) -------------------------------------------

namespace {

// Writes a real .lnk shell shortcut pointing at dask_browser.exe with the
// given arguments, so it ALWAYS opens in DASK (a .url Internet shortcut
// would be opened by the system default browser instead).
bool WriteLauncherShortcut(const std::wstring& dir, std::wstring name,
                           const std::wstring& url,
                           const std::wstring& args) {
  wchar_t exe[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  for (wchar_t& c : name) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
  }
  const size_t budget = MAX_PATH - dir.size() - 12;
  if (name.size() > budget) name.resize(budget);
  std::wstring base = dir + L"\\" + name;
  std::wstring path = base + L".lnk";
  for (int i = 2;
       ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
    path = base + L" (" + std::to_wstring(i) + L").lnk";
  }
  // COM may already be initialized (MTA) on CEF's UI thread.
  const HRESULT coinited = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_here = SUCCEEDED(coinited);
  bool ok = false;
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
    link->SetPath(exe);
    link->SetArguments(args.c_str());
    link->SetIconLocation(exe, 0);
    link->SetDescription((L"DASK Browser — " + url).c_str());
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) {
      ok = SUCCEEDED(pf->Save(path.c_str(), TRUE));
      pf->Release();
    }
    link->Release();
  }
  if (com_here) ::CoUninitialize();
  return ok;
}

}  // namespace

void TabManager::CreatePageShortcut() {
  // "Create shortcut" — a launcher .lnk on the Desktop opening the page in
  // a normal DASK window.
  PostToCef([this]() {
    Tab* tab = FindTab(active_tab_id_);
    if (!tab || tab->internal || tab->url.empty()) return;
    wchar_t desktop[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr,
                                  0, desktop))) {
      return;
    }
    const std::wstring name =
        tab->title.empty() ? L"صفحة" : tab->title;
    WriteLauncherShortcut(desktop, name, tab->url,
                          L"\"" + tab->url + L"\"");
  });
}

void TabManager::InstallPageApp() {
  // "Install page as app" — a launcher .lnk in the Start menu's Programs
  // folder with --app <url>: it shows up in Start-menu search like an
  // installed app and opens DASK's own chromeless app window.
  PostToCef([this]() {
    Tab* tab = FindTab(active_tab_id_);
    if (!tab || tab->internal || tab->url.empty()) return;
    wchar_t programs[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0,
                                  programs))) {
      return;
    }
    // Name the "app" after the site host (stable across pages) falling
    // back to the page title.
    std::wstring name;
    CefURLParts parts;
    if (CefParseURL(tab->url, parts) && parts.host.length > 0) {
      name = CefString(&parts.host).ToWString();
      // Strip common prefixes for a cleaner name (example.com).
      for (const wchar_t* pfx : {L"www."}) {
        if (name.rfind(pfx, 0) == 0) name = name.substr(wcslen(pfx));
      }
    }
    if (name.empty()) {
      name = tab->title.empty() ? L"تطبيق" : tab->title;
    }
    WriteLauncherShortcut(programs, name, tab->url,
                          L"--app \"" + tab->url + L"\"");
  });
}

void TabManager::SetLightTheme(bool enabled) {
  light_theme_ = enabled;
  PostToDb([this, enabled]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetBool("lightTheme", enabled, &err);
  });
  PushUiEvent();
}

void TabManager::SetRestoreSession(bool enabled) {
  restore_session_ = enabled;
  PostToDb([this, enabled]() {
    if (!settings_store_) return;
    std::string err;
    settings_store_->SetBool("restoreSession", enabled, &err);
  });
}

// ---- Permissions & profiles (Phase 5) --------------------------------------

namespace {

// Permission-type bit -> DB key + Arabic display label. First mapped bit
// decides the DB row for a mixed request.
struct PermissionTypeEntry {
  uint32_t bit;
  const char* key;
  const wchar_t* label;
};

const PermissionTypeEntry kPermissionTypes[] = {
    {CEF_PERMISSION_TYPE_MIC_STREAM, "microphone", L"الميكروفون"},
    {CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, "camera", L"الكاميرا (تحكم)"},
    {CEF_PERMISSION_TYPE_CAMERA_STREAM, "camera", L"الكاميرا"},
    {CEF_PERMISSION_TYPE_GEOLOCATION, "geolocation", L"الموقع الجغرافي"},
    {CEF_PERMISSION_TYPE_NOTIFICATIONS, "notifications", L"الإشعارات"},
    {CEF_PERMISSION_TYPE_CLIPBOARD, "clipboard", L"الحافظة"},
    {CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, "windowManagement", L"إدارة النوافذ"},
    {CEF_PERMISSION_TYPE_POINTER_LOCK, "pointerLock", L"قفل المؤشر"},
    {CEF_PERMISSION_TYPE_IDLE_DETECTION, "idleDetection", L"كشف الخمول"},
    {CEF_PERMISSION_TYPE_LOCAL_FONTS, "localFonts", L"الخطوط المحلية"},
};

// Media-request bits -> DB keys (device capture only; desktop capture is
// not granted by this browser).
std::vector<std::string> MediaDbKeys(uint32_t requested) {
  std::vector<std::string> keys;
  if (requested & CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE)
    keys.push_back("microphone");
  if (requested & CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE)
    keys.push_back("camera");
  return keys;
}

uint32_t MediaBitsForKeys(const std::vector<std::string>& keys) {
  uint32_t bits = 0;
  for (const std::string& key : keys) {
    if (key == "microphone")
      bits |= CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE;
    else if (key == "camera")
      bits |= CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
  }
  return bits;
}

std::vector<std::string> PromptDbKeys(uint32_t requested) {
  std::vector<std::string> keys;
  for (const PermissionTypeEntry& t : kPermissionTypes) {
    if (requested & t.bit) {
      if (std::find(keys.begin(), keys.end(), t.key) == keys.end())
        keys.push_back(t.key);
    }
  }
  return keys;
}

std::vector<std::wstring> PromptLabels(uint32_t requested) {
  std::vector<std::wstring> labels;
  for (const PermissionTypeEntry& t : kPermissionTypes) {
    if (requested & t.bit) labels.push_back(t.label);
  }
  return labels;
}

}  // namespace

void TabManager::OnMediaAccessPermission(
    int64_t tab_id, const std::wstring& origin, uint32_t requested,
    CefRefPtr<CefMediaAccessCallback> callback) {
  const std::wstring host = adblock::UrlHost(origin);
  if (host.empty() || requested == 0 || MediaDbKeys(requested).empty()) {
    callback->Cancel();
    return;
  }
  bool private_tab = false;
  for (const Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      private_tab = tab.private_flag;
      break;
    }
  }
  const bool priv = private_tab;
  PostToDb([this, host, origin, requested, priv, callback]() {
    const std::vector<std::string> keys = MediaDbKeys(requested);
    int allow_count = 0;
    int deny_count = 0;
    std::vector<std::string> ask_keys;
    if (permissions_store_) {
      std::string err;
      for (const std::string& key : keys) {
        const int d = permissions_store_->Get(Utf8FromWide(host), key, &err);
        if (d == storage::kPermissionAllow) ++allow_count;
        else if (d == storage::kPermissionDeny) ++deny_count;
        else ask_keys.push_back(key);
      }
    } else {
      ask_keys = keys;
    }
    PostToCef([this, origin, requested, ask_keys, priv, callback, deny_count]() {
      if (deny_count > 0) {
        callback->Cancel();
        return;
      }
      if (ask_keys.empty()) {
        callback->Continue(requested);  // all stored decisions allow
        return;
      }
      // Needs the user: queue the prompt (media flow, CEF prompt id 0).
      QueuePermissionPrompt(0, origin, PromptLabels(requested), ask_keys,
                            requested, true, priv, callback, nullptr);
    });
  });
}

bool TabManager::OnPermissionPrompt(
    int64_t tab_id, uint64_t prompt_id, const std::wstring& origin,
    uint32_t requested,
    CefRefPtr<CefPermissionPromptCallback> callback) {
  const std::wstring host = adblock::UrlHost(origin);
  if (host.empty() || PromptDbKeys(requested).empty()) return false;
  bool private_tab = false;
  for (const Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      private_tab = tab.private_flag;
      break;
    }
  }
  const bool priv = private_tab;
  PostToDb([this, host, origin, requested, priv, prompt_id, callback]() {
    const std::vector<std::string> keys = PromptDbKeys(requested);
    int deny_count = 0;
    std::vector<std::string> ask_keys;
    if (permissions_store_) {
      std::string err;
      for (const std::string& key : keys) {
        const int d = permissions_store_->Get(Utf8FromWide(host), key, &err);
        if (d == storage::kPermissionDeny) ++deny_count;
        else if (d != storage::kPermissionAllow) ask_keys.push_back(key);
      }
    } else {
      ask_keys = keys;
    }
    PostToCef([this, origin, requested, ask_keys, priv, prompt_id, callback, deny_count]() {
      if (deny_count > 0) {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return;
      }
      if (ask_keys.empty()) {
        callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
        return;
      }
      QueuePermissionPrompt(prompt_id, origin, PromptLabels(requested),
                            ask_keys, 0, false, priv, nullptr, callback);
    });
  });
  return true;
}

void TabManager::DismissPermissionPrompt(uint64_t prompt_id) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t i = 0; i < permission_prompts_.size(); ++i) {
      if (permission_prompts_[i].cef_id == prompt_id) {
        permission_prompts_.erase(permission_prompts_.begin() + i);
        break;
      }
    }
  }
  PushUiEvent();
}

void TabManager::QueuePermissionPrompt(
    uint64_t cef_id, const std::wstring& origin,
    const std::vector<std::wstring>& labels,
    const std::vector<std::string>& db_keys, uint32_t media_bits,
    bool media, bool for_private,
    CefRefPtr<CefMediaAccessCallback> media_cb,
    CefRefPtr<CefPermissionPromptCallback> prompt_cb) {
  PendingPermission prompt;
  prompt.cef_id = cef_id;
  prompt.ui_id = prompt_seq_++;
  prompt.origin = origin;
  prompt.labels = labels;
  prompt.db_keys = db_keys;
  prompt.media_bits = media_bits;
  prompt.media = media;
  prompt.for_private = for_private;
  prompt.media_cb = media_cb;
  prompt.prompt_cb = prompt_cb;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Keep the queue small: one origin may replace its older pending prompt.
    for (auto it = permission_prompts_.begin();
         it != permission_prompts_.end(); ++it) {
      if (it->origin == origin && it->media == media) {
        permission_prompts_.erase(it);
        break;
      }
    }
    permission_prompts_.push_back(std::move(prompt));
  }
  PushUiEvent();
}

void TabManager::RespondPermission(uint64_t ui_id, const std::string& action) {
  PendingPermission prompt;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t i = 0; i < permission_prompts_.size(); ++i) {
      if (permission_prompts_[i].ui_id == ui_id) {
        prompt = permission_prompts_[i];
        permission_prompts_.erase(permission_prompts_.begin() + i);
        found = true;
        break;
      }
    }
  }
  if (!found) return;

  const bool allow = action == "allow" || action == "allowOnce";
  const bool remember = action == "allow" || action == "deny";
  if (prompt.media_cb) {
    if (allow) {
      prompt.media_cb->Continue(prompt.media_bits);
    } else {
      prompt.media_cb->Cancel();
    }
  } else if (prompt.prompt_cb) {
    prompt.prompt_cb->Continue(allow ? CEF_PERMISSION_RESULT_ACCEPT
                                     : CEF_PERMISSION_RESULT_DENY);
  }
  if (remember && !prompt.for_private) {
    const int decision =
        allow ? storage::kPermissionAllow : storage::kPermissionDeny;
    PostToDb([this, host = Utf8FromWide(adblock::UrlHost(prompt.origin)),
              keys = prompt.db_keys, decision]() {
      if (!permissions_store_ || host.empty()) return;
      std::string err;
      for (const std::string& key : keys) {
        permissions_store_->Set(host, key, decision, &err);
      }
      RefreshStorageCachesOnDb();
    });
  }
  PushUiEvent();
}

void TabManager::SetPermission(const std::string& host,
                               const std::string& type, int decision) {
  PostToDb([this, host, type, decision]() {
    if (!permissions_store_) return;
    std::string err;
    if (decision == storage::kPermissionAsk) {
      permissions_store_->Remove(host, type, &err);
    } else {
      permissions_store_->Set(host, type, decision, &err);
    }
    RefreshStorageCachesOnDb();
  });
}

void TabManager::ResetPermission(const std::string& host,
                                 const std::string& type) {
  PostToDb([this, host, type]() {
    if (!permissions_store_) return;
    std::string err;
    permissions_store_->Remove(host, type, &err);
    RefreshStorageCachesOnDb();
  });
}

// ---- Profiles (Phase 5) ----------------------------------------------------

void TabManager::AddProfile(const std::string& name) {
  std::string id;
  std::string err;
  profile_store_->CreateProfile(name, &id, &err);
  PushUiEvent();
}

void TabManager::SwitchProfile(const std::string& id) {
  if (profile_store_->active_id() == id || switching_profile_) return;
  for (const ProfileInfo& p : profile_store_->profiles()) {
    if (p.id != id) continue;
    // Persist the current session into the OLD profile, close every content
    // tab, then swap storage on the CEF UI thread (PerformProfileSwitch).
    SaveSession();
    pending_profile_switch_ = id;
    switching_profile_ = true;
    std::vector<int64_t> ids;
    for (const Tab& tab : tabs_) {
      if (tab.id != 0 && !tab.closing) ids.push_back(tab.id);
    }
    for (int64_t tab_id : ids) CloseTab(tab_id);
    return;
  }
}

void TabManager::DeleteProfile(const std::string& id) {
  profile_store_->DeleteProfile(id, nullptr);
  PushUiEvent();
}

// Runs on the CEF UI thread when every content tab of the old profile is
// gone (OnBrowserClosed sees only the UI browser left).
void TabManager::PerformProfileSwitch() {
  switching_profile_ = false;
  const std::string id = pending_profile_switch_;
  pending_profile_switch_.clear();
  if (id.empty()) return;

  StopDatabase();
  profile_context_ = nullptr;  // old cache directory is left untouched
  std::string err;
  if (profile_store_->SwitchProfile(id, &err)) {
    privacy_.Load(profile_store_->privacy_path());
    lists_.SetDisabledLists(privacy_.DisabledLists());
    lists_.RebuildEngine();
  }
  // Restore the new profile's session (same logic as Start()).
  {
    storage::Database prefs_db;
    std::string db_err;
    restore_session_ = true;
    history_enabled_ = true;
    retention_days_ = 90;
    if (prefs_db.Open(Utf8FromWide(profile_store_->db_path()), &db_err) &&
        prefs_db.Migrate(&db_err)) {
      storage::Settings prefs(&prefs_db);
      prefs.GetBool("restoreSession", false, &restore_session_, &db_err);
      prefs.GetBool("historyEnabled", true, &history_enabled_, &db_err);
      prefs.GetInt("historyRetentionDays", 90, &retention_days_, &db_err);
      prefs.GetBool("lightTheme", false, &light_theme_, &db_err);
      prefs.GetString("accent", "", &accent_, &db_err);
      if (accent_.empty() || accent_[0] != '#') accent_ = "#8b5cf6";
      prefs.GetString("onboarded", "", &onboarded_, &db_err);
    }
  }
  bool has_session = false;
  size_t restored_tabs = 0;
  std::ifstream file(profile_store_->session_path(), std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (!json.empty()) {
    CefRefPtr<CefValue> parsed = CefParseJSON(json, cef_json_parser_options_t(0));
    if (parsed && parsed->GetType() == VTYPE_DICTIONARY) {
      has_session = true;
      CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
      const std::wstring engine_id = dict->GetString("engineId").ToWString();
      if (engine_id == L"google" || engine_id == L"bing" ||
          engine_id == L"duckduckgo" || engine_id == L"custom") {
        engine_.id = engine_id;
        engine_.custom = dict->GetString("engineCustom").ToWString();
      }
      CefRefPtr<CefListValue> links = dict->GetList("quickLinks");
      if (links) {
        std::vector<QuickLink> restored;
        for (size_t i = 0; i < links->GetSize() && restored.size() < 24; ++i) {
          CefRefPtr<CefDictionaryValue> item = links->GetDictionary(i);
          if (!item) continue;
          QuickLink link;
          link.title = item->GetString("title").ToWString();
          link.url = item->GetString("url").ToWString();
          if (IsHttpUrl(link.url) && !link.title.empty()) {
            restored.push_back(link);
          }
        }
        if (!restored.empty()) quick_links_ = std::move(restored);
      }
      CefRefPtr<CefListValue> tabs = dict->GetList("tabs");
      if (tabs && restore_session_) {
        for (size_t i = 0;
             i < tabs->GetSize() && i < static_cast<size_t>(kMaxTabs); ++i) {
          CefRefPtr<CefDictionaryValue> item = tabs->GetDictionary(i);
          if (!item) continue;
          const std::wstring url = item->GetString("url").ToWString();
          const bool internal = url == kNewTabUrl ||
                                url.rfind(kDaskUiOrigin, 0) == 0;
          if (!internal && !IsHttpUrl(url)) continue;
          CreateTab(url);
          ++restored_tabs;
        }
      }
    }
  }
  if (!has_session || restored_tabs == 0) {
    CreateTab(kNewTabUrl);
  }
  StartDatabase();
  PushUiEvent();
}

// ---- Downloads (CEF UI thread) ----------------------------------------------

bool TabManager::OnBeforeDownload(int64_t tab_id,
                                  CefRefPtr<CefDownloadItem> item,
                                  const std::wstring& suggested_name,
                                  CefRefPtr<CefBeforeDownloadCallback> callback) {
  bool private_tab = false;
  for (const Tab& tab : tabs_) {
    if (tab.id == tab_id) {
      private_tab = tab.private_flag;
      break;
    }
  }
  const std::string name =
      storage::SanitizeDownloadFilename(Utf8FromWide(suggested_name));
  const std::wstring path = storage::UniqueDownloadPath(DownloadsDir(), name);
  const uint32_t cef_id = item->GetId();
  if (private_tab) {
    // Private tabs: the file is saved but never reaches persistent storage.
    download_ids_[cef_id] = 0;
    PostToCef([this, name]() {
      std::lock_guard<std::mutex> lock(state_mutex_);
      DownloadUi ui;
      ui.id = 0;
      ui.filename = WideFromUtf8(name);
      ui.state = storage::kDownloadActive;
      ui.risky = IsRiskyDownloadFilename(WideFromUtf8(name));
      downloads_cache_.push_back(std::move(ui));
    });
  } else {
    const std::string path_utf8 = Utf8FromWide(path);
    const std::string url_utf8 =
        Utf8FromWide(item->GetURL().ToWString());
    const uint32_t cef = cef_id;
    PostToDb([this, cef, url_utf8, path_utf8, name]() {
      if (!downloads_store_) return;
      std::string err;
      int64_t db_id = 0;
      downloads_store_->Insert(url_utf8, path_utf8, name,
                               static_cast<int64_t>(time(nullptr)), &db_id,
                               &err);
      RefreshStorageCachesOnDb();
      // Register the mapping only after the insert committed.
      PostToCef([this, cef, db_id]() {
        download_ids_[cef] = db_id;
        if (db_id > 0) download_db_to_cef_[db_id] = cef;
      });
    });
  }
  callback->Continue(path, false);
  return true;
}

void TabManager::OnDownloadUpdated(int64_t tab_id,
                                   CefRefPtr<CefDownloadItem> item,
                                   CefRefPtr<CefDownloadItemCallback> callback) {
  (void)tab_id;
  const uint32_t cef_id = item->GetId();

  if (item->IsInProgress()) {
    active_downloads_[cef_id] = callback;  // kept so the user can cancel
    const auto ids = download_ids_.find(cef_id);
    const int64_t db_id = ids == download_ids_.end() ? 0 : ids->second;
    // Live progress on the UI mirror.
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      for (DownloadUi& row : downloads_cache_) {
        if (db_id != 0 && row.id == db_id) {
          row.state = storage::kDownloadActive;
          row.received = item->GetReceivedBytes();
          row.total = item->GetTotalBytes();
        }
      }
    }
    if (db_id > 0) {
      const int64_t received = item->GetReceivedBytes();
      const int64_t total = item->GetTotalBytes();
      PostToDb([this, db_id, received, total]() {
        if (!downloads_store_) return;
        std::string err;
        downloads_store_->UpdateProgress(db_id, storage::kDownloadActive,
                                         received, total, &err);
      });
    }
    return;
  }
  // Terminal state.
  active_downloads_.erase(cef_id);
  int state = storage::kDownloadFailed;
  if (item->IsCanceled()) state = storage::kDownloadCanceled;
  else if (item->IsComplete()) state = storage::kDownloadComplete;
  const auto ids = download_ids_.find(cef_id);
  const int64_t db_id = ids == download_ids_.end() ? 0 : ids->second;
  download_ids_.erase(cef_id);
  download_db_to_cef_.erase(db_id);
  if (db_id > 0) {
    const int64_t received = item->GetReceivedBytes();
    const int64_t total = item->GetTotalBytes();
    PostToDb([this, db_id, state, received, total]() {
      if (!downloads_store_) return;
      std::string err;
      downloads_store_->UpdateProgress(db_id, state, received, total, &err);
      RefreshStorageCachesOnDb();
    });
  } else {
    PushUiEvent();
  }
}

void TabManager::CancelDownload(int64_t db_id) {
  const auto it = download_db_to_cef_.find(db_id);
  if (it == download_db_to_cef_.end()) return;
  const auto cb = active_downloads_.find(it->second);
  if (cb != active_downloads_.end()) {
    cb->second->Cancel();
  }
}

void TabManager::OpenDownload(int64_t db_id) {
  std::wstring path;
  int state = -1;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const DownloadUi& row : downloads_cache_) {
      if (row.id == db_id) {
        path = row.path;
        state = row.state;
        break;
      }
    }
  }
  if (state == storage::kDownloadComplete && !path.empty() &&
      ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
  }
}

void TabManager::OpenDownloadsFolder() {
  const std::wstring dir = DownloadsDir();
  if (!dir.empty()) {
    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
  }
}

}  // namespace dask
