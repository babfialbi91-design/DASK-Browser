// DASK Browser - shared CEF application implementation.
// Used by both the browser-process executable and the subprocess executable.
#pragma once

#include "include/cef_app.h"

namespace dask {

// Process-specific command-line adjustments. Keeping this explicit documents
// which switches the browser passes to child processes in Phase 1.
void OnBeforeCommandLineProcessing(const CefString& process_type,
                                   CefRefPtr<CefCommandLine> command_line);

// Creates the shared CefApp instance used by every process type.
CefRefPtr<CefApp> CreateDaskApp();

}  // namespace dask
