// DASK Browser - privacy settings persisted in privacy.json (Phase 3).
// Owns: global blocking toggle, third-party cookie policy, and the
// user-reviewable per-site exceptions. No telemetry, no cloud sync.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace dask {

class PrivacySettings {
 public:
  void Load(const std::wstring& appdata_dir);  // CEF UI thread
  void Save();                                 // CEF UI thread

  bool blocking_enabled() const { return blocking_enabled_; }
  void set_blocking_enabled(bool v) { blocking_enabled_ = v; }

  bool block_third_party_cookies() const { return third_party_cookies_; }
  void set_block_third_party_cookies(bool v) { third_party_cookies_ = v; }

  // Site exceptions: "allowed" hosts bypass blocking.
  bool IsSiteAllowed(const std::wstring& host) const;
  void SetSiteAllowed(const std::wstring& host, bool allowed);
  void RemoveSiteException(const std::wstring& host);
  std::vector<std::pair<std::wstring, bool>> SiteExceptions() const;

  // Phase 7.5: per-list enable/disable. Disabled lists stay on disk but are
  // not parsed into the engine.
  bool IsListEnabled(const std::wstring& name) const;
  void SetListEnabled(const std::wstring& name, bool enabled);
  std::vector<std::wstring> DisabledLists() const { return disabled_lists_; }

 private:
  bool blocking_enabled_ = true;
  bool third_party_cookies_ = true;
  // host -> user allows ads (bypasses blocking) on this site.
  std::vector<std::pair<std::wstring, bool>> site_exceptions_;
  std::vector<std::wstring> disabled_lists_;
  std::wstring file_path_;
};

}  // namespace dask
