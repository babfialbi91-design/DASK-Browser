// DASK Browser - shared CEF application implementation.
// Used by both the browser-process executable and the subprocess executable.
#pragma once

#include <string>

#include "include/cef_app.h"

namespace dask {

// Process-specific command-line adjustments. Keeping this explicit documents
// which switches the browser passes to child processes in Phase 2.
void OnBeforeCommandLineProcessing(const CefString& process_type,
                                   CefRefPtr<CefCommandLine> command_line);

// Phase 7.5: DoH provider chosen in Settings; set by the browser process
// before CefInitialize. Applied via Chromium network-service switches.
void SetDoHResolver(const std::string& resolver);

// Creates the shared CefApp instance used by every process type. When
// |ui_dir| is provided, the "dask://" scheme handler factory serving the
// React bundle is registered after context initialization (browser process
// only); pass an empty string for child processes.
CefRefPtr<CefApp> CreateDaskApp(const std::wstring& ui_dir = L"");

}  // namespace dask
