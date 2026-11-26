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
#include "storage/download_filename.h"

namespace dask {

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
    if (!mgr_->ui_browser_ || !browser->IsSame(mgr_->ui_browser_)) {
      TabManager::Tab* tab = mgr_->FindTabByBrowser(browser);
      const std::wstring frame_url = frame->GetURL().ToWString();
      const bool trusted = tab && tab->internal &&
                           frame_url.rfind(kDaskUiOrigin, 0) == 0;
      if (!trusted) {
        callback->Failure(403, "bridge access denied for this page");
        return true;
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
        "addProfile", "switchProfile", "deleteProfile"};
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
                  public CefDownloadHandler,
                  public CefLoadHandler,
                  public CefLifeSpanHandler,
                  public CefPermissionHandler,
                  public CefRequestHandler {
 public:
  TabClient(TabManager* mgr, int64_t tab_id) : mgr_(mgr), tab_id_(tab_id) {}

  // CefClient
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

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
      prefs.GetBool("restoreSession", true, &restore_session_, &db_err);
      prefs.GetBool("historyEnabled", true, &history_enabled_, &db_err);
      prefs.GetInt("historyRetentionDays", 90, &retention_days_, &db_err);
    }
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
      if (tabs && restore_session_) {
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
  if (!has_session || restored_tabs == 0) {
    CreateTab(kNewTabUrl);
  }

  // Phase 4: open the local database (thread + stores).
  StartDatabase();

  // The UI browser (React toolbar) on top of the window. Tab id 0.
  PostToCef([this]() {
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
    tab.private_flag = private_tab;
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
    if (private_tab) {
      if (!private_context_) {
        private_context_ = CefRequestContext::CreateContext(
            CefRequestContextSettings(), nullptr);
      }
      context = private_context_;
    } else {
      // Phase 5: the active profile gets an isolated CEF request context
      // (cookies/cache under profiles/<id>/cef_cache).
      if (!profile_context_) {
        CefRequestContextSettings ctx_settings;
        CefString(&ctx_settings.cache_path)
            .FromWString(profile_store_->cef_cache_dir());
        profile_context_ = CefRequestContext::CreateContext(ctx_settings, nullptr);
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
  {
    ::EnterCriticalSection(&layout_lock_);
    layout_order_.clear();
    layout_order_.reserve(tabs_.size());
    for (const Tab& t : tabs_) layout_order_.push_back(t.id);
    layout_active_ = active_tab_id_;
    ::LeaveCriticalSection(&layout_lock_);
  }
  if (tab->browser) tab->browser->GetHost()->SetFocus(true);
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

      std::string db_err;
      settings_store_->GetBool("restoreSession", true, &restore_session_,
                               &db_err);
      settings_store_->GetBool("historyEnabled", true, &history_enabled_,
                               &db_err);
      settings_store_->GetInt("historyRetentionDays", 90, &retention_days_,
                              &db_err);
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
      prefs.GetBool("restoreSession", true, &restore_session_, &db_err);
      prefs.GetBool("historyEnabled", true, &history_enabled_, &db_err);
      prefs.GetInt("historyRetentionDays", 90, &retention_days_, &db_err);
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
