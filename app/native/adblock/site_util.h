// DASK Browser - host/site helpers shared by the filter engine users.
#pragma once

#include <string>

namespace dask::adblock {

// Does |host| equal |domain| or end with "." + domain?
inline bool HostSuffixMatch(const std::wstring& host,
                            const std::wstring& domain) {
  if (host.empty() || domain.empty()) return false;
  if (host == domain) return true;
  if (host.size() > domain.size()) {
    const size_t off = host.size() - domain.size();
    if (host.compare(off, domain.size(), domain) == 0 && host[off - 1] == L'.') {
      return true;
    }
  }
  return false;
}

// Host of |url| ("" when it cannot be parsed).
inline std::wstring UrlHost(const std::wstring& url) {
  const size_t scheme_end = url.find(L"://");
  const size_t host_start = scheme_end == std::wstring::npos ? 0 : scheme_end + 3;
  size_t end = host_start;
  while (end < url.size() && url[end] != L'/' && url[end] != L':' &&
         url[end] != L'?' && url[end] != L'#') {
    ++end;
  }
  if (end == host_start) return std::wstring();
  std::wstring host = url.substr(host_start, end - host_start);
  // Strip userinfo and lowercase ASCII.
  const size_t at = host.rfind(L'@');
  if (at != std::wstring::npos) host = host.substr(at + 1);
  for (wchar_t& c : host) {
    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
  }
  return host;
}

// Approximate same-site test without a public-suffix list: equal hosts or
// identical last two labels (e.g. img.example.com ~ example.com). Documented
// as an approximation in README - it may treat some multi-label public
// suffixes (e.g. co.uk) as third-party boundaries incorrectly.
inline bool SameSiteApprox(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) return false;
  if (a == b) return true;
  const auto tail = [](const std::wstring& h) {
    size_t dots = 0;
    size_t i = h.size();
    while (i > 0) {
      if (h[i - 1] == L'.') {
        ++dots;
        if (dots == 2) break;
      }
      --i;
    }
    return h.substr(i);
  };
  return tail(a) == tail(b);
}

}  // namespace dask::adblock
