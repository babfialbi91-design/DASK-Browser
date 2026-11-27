// DASK Browser - automatic update checker.
// Checks GitHub's public releases API for a newer version. No identifiers
// are ever sent: a single anonymous HTTPS GET at most once per day, and a
// manual check from the settings page. The result only enables a download
// link shown to the user.
#include "updater.h"

#include <windows.h>
#include <httpdownload.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "tab_manager.h"

namespace dask {

namespace {

// DASK Browser version (kept in sync with the installer / CMake).
constexpr char kAppVersion[] = "1.1.0";

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
  return out;
}

bool VersionNewer(const std::string& remote, const std::string& local) {
  int r[3] = {0, 0, 0}, l[3] = {0, 0, 0};
  if (::sscanf_s(remote.c_str(), "v%d.%d.%d", &r[0], &r[1], &r[2]) < 2)
    return false;
  ::sscanf_s(local.c_str(), "%d.%d.%d", &l[0], &l[1], &l[2]);
  for (int i = 0; i < 3; ++i) {
    if (r[i] != l[i]) return r[i] > l[i];
  }
  return false;
}

}  // namespace

void CheckForUpdate(bool manual) {
  // Worker thread; never blocks the browser. One silent check per day
  // unless the user asked for it from settings.
  if (!manual) {
    const std::string last = TabManager::ReadPrefString("lastUpdateCheck");
    const long long today = ::GetTickCount64() / (24ULL * 3600 * 1000);
    if (!last.empty() && std::atoll(last.c_str()) == today) return;
  }

  std::thread([manual]() {
    const std::string url =
        "https://api.github.com/repos/babfialbi91-design/"
        "DASK-Browser/releases/latest";
    std::string body;
    if (!DownloadToString(url, &body, /*timeout_ms=*/15000)) {
      if (manual) {
        CefPostTask(TID_UI, base::BindOnce([]() {
          TabManager::Get()->UpdateAvailable("", "", false, true);
        }));
      }
      return;
    }
    if (!manual) {
      // Only persist the "checked today" marker on successful checks.
      TabManager::Get()->SetPrefString(
          "lastUpdateCheck", std::to_string(
              ::GetTickCount64() / (24ULL * 3600 * 1000)));
    }
    CefRefPtr<CefValue> parsed =
        CefParseJSON(body, cef_json_parser_options_t(0));
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) return;
    CefRefPtr<CefDictionaryValue> rel = parsed->GetDictionary();
    const std::string tag = rel->GetString("tag_name").ToString();
    const std::string html = rel->GetString("html_url").ToString();
    std::string installer_url;
    CefRefPtr<CefListValue> assets = rel->GetList("assets");
    if (assets) {
      for (size_t i = 0; i < assets->GetSize(); ++i) {
        CefRefPtr<CefDictionaryValue> a = assets->GetDictionary(i);
        if (!a) continue;
        const std::string name = a->GetString("name").ToString();
        const std::string lower = [&] {
          std::string s = name;
          std::transform(s.begin(), s.end(), s.begin(),
                         [](unsigned char c) { return (char)tolower(c); });
          return s;
        }();
        if (lower.find("setup") != std::string::npos) {
          installer_url = a->GetString("browser_download_url").ToString();
          break;
        }
      }
      if (installer_url.empty() && assets->GetSize() > 0) {
        installer_url = assets->GetDictionary(0)
                            ->GetString("browser_download_url")
                            .ToString();
      }
    }
    const bool newer = VersionNewer(tag, kAppVersion);
    CefPostTask(TID_UI, base::BindOnce(
        [](std::string tag, std::string page, std::string installer,
           bool newer) {
          TabManager::Get()->UpdateAvailable(tag, !installer.empty()
                                                 ? installer : page,
                                             newer, false);
        },
        tag, html, installer_url, newer));
  }).detach();
}

}  // namespace dask
