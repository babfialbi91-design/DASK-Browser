// DASK Browser - UI scheme handler.
// Serves the React UI bundle from the "ui" directory next to the executable,
// intercepted at http://dask.ui/. Path traversal and absolute paths are
// rejected; nothing outside the ui directory is ever served.
//
// Why not a custom "dask://" scheme? Custom schemes are restricted in some
// Chromium versions (document rendering, module scripts). Intercepting a
// dedicated host on the standard http scheme gives full document handling
// with zero quirks, while still being fully under our control: the host
// dask.ui is always served by this factory, never by the network stack.
#pragma once

#include <string>

#include "include/cef_base.h"
#include "include/cef_scheme.h"

namespace dask {

// The origin that identifies DASK's own UI pages.
constexpr wchar_t kUiOrigin[] = L"http://dask.ui";
constexpr wchar_t kUiHost[] = L"dask.ui";

// Installs the UI handler factory after context initialization (browser
// process only). |ui_dir| is the directory holding the built React bundle.
void InstallUiSchemeHandler(const std::wstring& ui_dir);

}  // namespace dask
