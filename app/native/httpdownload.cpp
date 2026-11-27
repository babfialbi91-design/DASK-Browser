// DASK Browser - minimal HTTPS downloader used by the update checker.
#include "httpdownload.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace dask {

bool DownloadToString(const std::string& url_utf8, std::string* out,
                      int timeout_ms) {
  if (!out) return false;
  // Minimal URL split (https only; api.github.com is all we need today).
  std::wstring host, path;
  {
    std::string u = url_utf8;
    const std::string pfx = "https://";
    if (u.rfind(pfx, 0) != 0) return false;
    u = u.substr(pfx.size());
    const size_t slash = u.find('/');
    std::string h = u.substr(0, slash);
    path = std::wstring(u.substr(slash).begin(), u.substr(slash).end());
    host = std::wstring(h.begin(), h.end());
  }
  out->clear();

  HINTERNET session = ::WinHttpOpen(L"DASK Browser Update",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = nullptr;
  bool ok = false;
  do {
    if (!connect) break;
    request = ::WinHttpOpenRequest(
        connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) break;
    // TLS 1.2 is the default on Win10/11 SDKs; nothing to set explicitly.
    ::WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms,
                         timeout_ms);
    // GitHub API requires a User-Agent.
    const wchar_t* hdrs = L"User-Agent: DASK-Browser-Updater\r\n";
    if (!::WinHttpSendRequest(request, hdrs, static_cast<DWORD>(-1L),
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
      break;
    if (!::WinHttpReceiveResponse(request, nullptr)) break;

    for (;;) {
      DWORD avail = 0;
      if (!::WinHttpQueryDataAvailable(request, &avail)) break;
      if (avail == 0) {
        DWORD status = 0, sz = sizeof(status);
        ::WinHttpQueryHeaders(request,
                              WINHTTP_QUERY_STATUS_CODE |
                                  WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                              WINHTTP_NO_HEADER_INDEX);
        ok = status == 200 && !out->empty();
        break;
      }
      std::vector<char> buf(avail);
      DWORD read = 0;
      if (!::WinHttpReadData(request, buf.data(), avail, &read)) break;
      out->append(buf.data(), read);
      if (out->size() > (4u << 20)) break;  // API responses are tiny
    }
  } while (false);
  if (request) ::WinHttpCloseHandle(request);
  if (connect) ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return ok;
}

}  // namespace dask
