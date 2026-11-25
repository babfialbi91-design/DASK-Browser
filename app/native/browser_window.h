// DASK Browser - native Win32 window hosting a CEF browser.
#pragma once

#include <windows.h>

namespace dask {

// Creates, registers and runs the main browser window.
// Returns the Win32 exit code.
int RunBrowserWindow(HINSTANCE h_instance, int n_cmd_show);

}  // namespace dask
