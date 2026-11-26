// DASK Browser - shared CEF application implementation.
#include "dask_app.h"

#include "dask_scheme.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_message_router.h"

namespace dask {

void OnBeforeCommandLineProcessing(const CefString& process_type,
                                   CefRefPtr<CefCommandLine> command_line) {
  // No custom switches. Do not disable the Chromium sandbox here (or anywhere)
  // merely to make startup easier; sandboxing stays enabled.
  (void)process_type;
  (void)command_line;
}

namespace {

// Renderer-side wiring for the UI bridge. The router injects window.cefQuery /
// window.cefQueryCancel into every renderer context; authorization happens on
// the browser side (see tab_manager), where only DASK's own UI pages are
// allowed to issue requests.
class DaskRenderHandler : public CefRenderProcessHandler {
 public:
  DaskRenderHandler() {
    CefMessageRouterConfig config;
    renderer_router_ = CefMessageRouterRendererSide::Create(config);
  }

  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    renderer_router_->OnContextCreated(browser, frame, context);
  }

  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    renderer_router_->OnContextReleased(browser, frame, context);
  }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    return renderer_router_->OnProcessMessageReceived(browser, frame,
                                                      source_process, message);
  }

 private:
  IMPLEMENT_REFCOUNTING(DaskRenderHandler);
  DISALLOW_COPY_AND_ASSIGN(DaskRenderHandler);

  CefRefPtr<CefMessageRouterRendererSide> renderer_router_;
};

// Bare-bones CefApp: forwards command-line processing, registers the dask
// scheme, wires the renderer-side bridge and (in the browser process only)
// installs the http://dask.ui UI handler factory.
class DaskApp : public CefApp, public DaskRenderHandler {
 public:
  explicit DaskApp(std::wstring ui_dir)
      : ui_dir_(std::move(ui_dir)),
        browser_handler_(new DaskBrowserHandler(ui_dir_)) {}

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    dask::OnBeforeCommandLineProcessing(process_type, command_line);
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    // DaskApp inherits DaskRenderHandler, so returning this exposes the real
    // message-router wiring to the renderer process.
    return this;
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return browser_handler_;
  }

 private:
  // Installs the http://dask.ui UI handler factory once the context is ready
  // (browser process only).
  class DaskBrowserHandler : public CefBrowserProcessHandler {
   public:
    explicit DaskBrowserHandler(std::wstring ui_dir)
        : ui_dir_(std::move(ui_dir)) {}

    void OnContextInitialized() override {
      if (!ui_dir_.empty()) {
        InstallUiSchemeHandler(ui_dir_);
      }
    }

   private:
    IMPLEMENT_REFCOUNTING(DaskBrowserHandler);
    DISALLOW_COPY_AND_ASSIGN(DaskBrowserHandler);

    std::wstring ui_dir_;
  };

  IMPLEMENT_REFCOUNTING(DaskApp);
  DISALLOW_COPY_AND_ASSIGN(DaskApp);

  std::wstring ui_dir_;
  CefRefPtr<DaskBrowserHandler> browser_handler_;
};

}  // namespace

CefRefPtr<CefApp> CreateDaskApp(const std::wstring& ui_dir) {
  return new DaskApp(ui_dir);
}

}  // namespace dask
