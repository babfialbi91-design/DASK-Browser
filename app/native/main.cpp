// DASK Browser - entry point of the main (browser-process) executable.
#include <windows.h>

#include "browser_window.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"

int WINAPI wWinMain(HINSTANCE h_instance,
                    HINSTANCE h_prev_instance,
                    PWSTR p_cmd_line,
                    int n_cmd_show) {
  UNREFERENCED_PARAMETER(h_prev_instance);
  UNREFERENCED_PARAMETER(p_cmd_line);

  // This executable is always the browser process: child processes are spawned
  // from dask_browser_subprocess.exe (see CefSettings.browser_subprocess_path).
  return dask::RunBrowserWindow(h_instance, n_cmd_show);
}
