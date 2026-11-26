// DASK Browser - privacy settings implementation (privacy.json).
#include "privacy_settings.h"

#include <fstream>
#include <iterator>

#include "include/cef_parser.h"
#include "include/cef_values.h"
#include "adblock/site_util.h"

namespace dask {
namespace {

constexpr wchar_t kPrivacyFile[] = L"privacy.json";

std::wstring HostOf(const std::wstring& input) {
  return adblock::UrlHost(input);
}

}  // namespace

void PrivacySettings::Load(const std::wstring& appdata_dir) {
  file_path_ = appdata_dir + L"\\" + kPrivacyFile;
  blocking_enabled_ = true;
  third_party_cookies_ = true;
  site_exceptions_.clear();

  std::ifstream file(file_path_, std::ios::binary);
  std::string json((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (json.empty()) return;
  CefRefPtr<CefValue> parsed = CefParseJSON(json, cef_json_parser_options_t(0));
  if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) return;
  CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
  blocking_enabled_ = dict->GetBool("blockingEnabled");
  third_party_cookies_ = dict->GetBool("thirdPartyCookies");
  CefRefPtr<CefListValue> exc = dict->GetList("siteExceptions");
  if (exc) {
    for (size_t i = 0; i < exc->GetSize(); ++i) {
      CefRefPtr<CefDictionaryValue> item = exc->GetDictionary(i);
      if (!item) continue;
      const std::wstring host = item->GetString("host").ToWString();
      if (!HostOf(host).empty()) {
        site_exceptions_.emplace_back(HostOf(host),
                                      item->GetBool("allowed"));
      }
    }
  }
}

void PrivacySettings::Save() {
  if (file_path_.empty()) return;
  CefRefPtr<CefDictionaryValue> root_dict = CefDictionaryValue::Create();
  root_dict->SetBool("blockingEnabled", blocking_enabled_);
  root_dict->SetBool("thirdPartyCookies", third_party_cookies_);
  CefRefPtr<CefListValue> exc = CefListValue::Create();
  for (size_t i = 0; i < site_exceptions_.size(); ++i) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetString("host", site_exceptions_[i].first);
    item->SetBool("allowed", site_exceptions_[i].second);
    exc->SetDictionary(i, item);
  }
  root_dict->SetList("siteExceptions", exc);
  CefRefPtr<CefValue> root = CefValue::Create();
  root->SetDictionary(root_dict);
  const std::string json =
      CefWriteJSON(root, cef_json_writer_options_t(0)).ToString();
  std::ofstream file(file_path_, std::ios::binary | std::ios::trunc);
  file.write(json.data(), static_cast<std::streamsize>(json.size()));
}

bool PrivacySettings::IsSiteAllowed(const std::wstring& host) const {
  for (const auto& [h, allowed] : site_exceptions_) {
    if (h == host) return allowed;
  }
  return false;
}

void PrivacySettings::SetSiteAllowed(const std::wstring& host, bool allowed) {
  for (auto& [h, a] : site_exceptions_) {
    if (h == host) {
      a = allowed;
      return;
    }
  }
  site_exceptions_.emplace_back(host, allowed);
}

void PrivacySettings::RemoveSiteException(const std::wstring& host) {
  for (auto it = site_exceptions_.begin(); it != site_exceptions_.end(); ++it) {
    if (it->first == host) {
      site_exceptions_.erase(it);
      return;
    }
  }
}

std::vector<std::pair<std::wstring, bool>> PrivacySettings::SiteExceptions()
    const {
  return site_exceptions_;
}

}  // namespace dask
