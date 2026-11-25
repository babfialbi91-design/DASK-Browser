// DASK Browser - shared CEF application implementation.
#include "dask_app.h"

#include "include/cef_command_line.h"

namespace dask {

void OnBeforeCommandLineProcessing(const CefString& process_type,
                                   CefRefPtr<CefCommandLine> command_line) {
  // Phase 1: no custom switches. Do not disable the Chromium sandbox here
  // (or anywhere) merely to make startup easier; sandboxing stays enabled.
  (void)process_type;
  (void)command_line;
}

namespace {

// Bare-bones CefApp: forwards command-line processing and otherwise relies on
// CEF defaults. Browser-process-specific behavior lives in browser_window.*.
class DaskApp : public CefApp {
 public:
  DaskApp() = default;

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    dask::OnBeforeCommandLineProcessing(process_type, command_line);
  }

 private:
  IMPLEMENT_REFCOUNTING(DaskApp);
  DISALLOW_COPY_AND_ASSIGN(DaskApp);
};

}  // namespace

CefRefPtr<CefApp> CreateDaskApp() {
  return new DaskApp();
}

}  // namespace dask
