// DASK Browser - tab manager implementation. See tab_manager.h for the
// threading contract.
#include "tab_manager.h"

#include <shlobj.h>
#include <shlwapi.h>

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
      mgr_->CreateTab(url);
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
                  public CefLoadHandler,
                  public CefLifeSpanHandler,
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
    if (frame->IsMain()) mgr_->OnAddressChanged(tab_id_, url.ToWString());
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

  // Phase 3: privacy settings and filter lists.
  privacy_.Load(AppDataDir());
  lists_.Initialize(AppDataDir());

  CefMessageRouterConfig config;
  router_ = CefMessageRouterBrowserSide::Create(config);
  router_->AddHandler(new DaskQueryHandler(this), false);
}

void TabManager::Start() {
  // Restore the persisted session (read on this thread, applied on CEF's).
  std::ifstream file(std::wstring(AppDataDir()) + L"\\" + kSessionFile,
                     std::ios::binary);
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
      if (tabs) {
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

void TabManager::CreateTab(std::wstring url) {
  PostToCef([this, url]() {
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
    CefString url_str;
    url_str.FromWString(url);
    CefBrowserHost::CreateBrowser(info, client, url_str, settings, nullptr,
                                  nullptr);

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
    // Closing the last tab closes the window (Chrome-like behavior).
    ::PostMessageW(main_hwnd_, WM_CLOSE, 0, 0);
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
    AppendDict(tabs_list, item);
  }
  root_dict->SetList("tabs", tabs_list);
  root_dict->SetInt("activeTabId", static_cast<int>(active_tab_id_));

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
    if (tab.closing) continue;
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
  std::ofstream file(std::wstring(AppDataDir()) + L"\\" + kSessionFile,
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

}  // namespace dask
