// DASK Browser - CEF subprocess entry point.
//
// Handles every child process type (renderer / gpu / utility / crashpad).
// CEF decides the role from the --type= command-line switch it passes.
#include <windows.h>

#include "dask_app.h"
#include "include/cef_app.h"

int WINAPI wWinMain(HINSTANCE h_instance,
                    HINSTANCE h_prev_instance,
                    PWSTR p_cmd_line,
                    int n_cmd_show) {
  UNREFERENCED_PARAMETER(h_prev_instance);
  UNREFERENCED_PARAMETER(p_cmd_line);

  CefMainArgs main_args(h_instance);
  CefRefPtr<CefApp> app = dask::CreateDaskApp();

  // Blocks and dispatches the subprocess role; returns the process exit code.
  return CefExecuteProcess(main_args, app, nullptr);
}
