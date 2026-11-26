// DASK Browser - unit tests for the adblock filter engine (Phase 3).
// Plain assert-based tests, no framework dependency.
//
// Build target: dask_filter_tests (see CMakeLists.txt).
#include <cassert>
#include <cstdio>
#include <string>

#include "adblock/filter_engine.h"

using dask::adblock::FilterEngine;
using dask::adblock::ParseReport;
using dask::adblock::RequestInfo;
using dask::adblock::ResourceKind;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static RequestInfo MakeReq(const std::wstring& url,
                           const std::wstring& first_party,
                           ResourceKind kind = ResourceKind::Other) {
  RequestInfo req;
  req.url.resize(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    req.url[i] = (url[i] >= L'A' && url[i] <= L'Z') ? url[i] - L'A' + L'a'
                                                    : url[i];
  }
  const size_t scheme_end = req.url.find(L"://");
  const size_t host_start = scheme_end == std::wstring::npos ? 0 : scheme_end + 3;
  const size_t slash = req.url.find(L'/', host_start);
  req.host = req.url.substr(
      host_start, slash == std::wstring::npos ? std::wstring::npos
                                              : slash - host_start);
  req.first_party.resize(first_party.size());
  for (size_t i = 0; i < first_party.size(); ++i) {
    req.first_party[i] = (first_party[i] >= L'A' && first_party[i] <= L'Z')
                             ? first_party[i] - L'A' + L'a'
                             : first_party[i];
  }
  req.kind = kind;
  req.third_party = !req.first_party.empty() &&
                    req.host != req.first_party &&
                    !(req.host.size() > req.first_party.size() &&
                      req.host.compare(req.host.size() - req.first_party.size(),
                                       req.first_party.size(),
                                       req.first_party) == 0 &&
                      req.host[req.host.size() - req.first_party.size() - 1] == L'.');
  return req;
}

static std::unique_ptr<FilterEngine> Parse(const std::string& text,
                                           ParseReport* rep = nullptr) {
  ParseReport local;
  return FilterEngine::Parse(text, rep ? rep : &local);
}

static void TestBasicHostBlocking() {
  auto engine = Parse(
      "[Adblock Plus 2.0]\n"
      "! comment\n"
      "||ads.example.com^\n"
      "||cdn.ads.example.net/banners/\n");
  CHECK(engine);
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/img.png",
                                    L"news.example.com")));
  CHECK(engine->ShouldBlock(MakeReq(L"https://www.ads.example.com/x.js",
                                    L"news.example.com", ResourceKind::Script)));
  // Subdomain match through the host index.
  CHECK(engine->ShouldBlock(MakeReq(L"https://b.cdn.ads.example.net/banners/x.png",
                                    L"example.org", ResourceKind::Image)));
  // The main site must NOT be blocked.
  CHECK(!engine->ShouldBlock(MakeReq(L"https://news.example.com/",
                                     L"news.example.com")));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://notads.example.com/img.png",
                                     L"notads.example.com")));
}

static void TestLookalikeHost() {
  auto engine = Parse("||example.com^\n");
  // ||example.com must not match example.com.evil.com (host boundary).
  CHECK(!engine->ShouldBlock(MakeReq(L"https://example.com.evil.com/a.png",
                                     L"evil.com")));
  CHECK(engine->ShouldBlock(MakeReq(L"https://example.com/a.png", L"evil.com")));
  CHECK(engine->ShouldBlock(MakeReq(L"https://www.example.com/a.png",
                                    L"evil.com")));
}

static void TestExceptionsAndImportant() {
  auto engine = Parse(
      "||ads.example.com^\n"
      "@@||ads.example.com^$script\n"
      "||tracker.example.com^$important\n"
      "@@||tracker.example.com^$image\n");
  // Exception with matching kind wins.
  CHECK(!engine->ShouldBlock(MakeReq(L"https://ads.example.com/s.js",
                                     L"site.com", ResourceKind::Script)));
  // Exception with different kind does not save the request.
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/s.js",
                                    L"site.com", ResourceKind::Image)));
  // $important block overrides a matching exception.
  CHECK(engine->ShouldBlock(MakeReq(L"https://tracker.example.com/p.png",
                                    L"site.com", ResourceKind::Image)));
}

static void TestThirdPartyOption() {
  auto engine = Parse("||track.example.com^$third-party\n"
                      "||cdn.example.com^$~third-party\n");
  // third-party only: same-site request is allowed.
  CHECK(!engine->ShouldBlock(MakeReq(L"https://track.example.com/x",
                                     L"track.example.com")));
  CHECK(engine->ShouldBlock(MakeReq(L"https://track.example.com/x",
                                    L"other.com")));
  // ~third-party: only same-site requests are blocked.
  CHECK(engine->ShouldBlock(MakeReq(L"https://cdn.example.com/x",
                                    L"cdn.example.com")));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://cdn.example.com/x",
                                     L"other.com")));
}

static void TestResourceKindOptions() {
  auto engine = Parse("||ads.example.com^$script\n"
                      "||img.example.com^$image,stylesheet\n");
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/a.js",
                                    L"site.com", ResourceKind::Script)));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://ads.example.com/a.png",
                                     L"site.com", ResourceKind::Image)));
  CHECK(engine->ShouldBlock(MakeReq(L"https://img.example.com/s.css",
                                    L"site.com", ResourceKind::Stylesheet)));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://img.example.com/s.js",
                                     L"site.com", ResourceKind::Script)));
}

static void TestDomainOptions() {
  auto engine = Parse("||ads.example.com^$domain=example.com|~sub.example.com\n");
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/x",
                                    L"example.com")));
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/x",
                                    L"other.example.com")));
  // ~sub.example.com excludes that subtree.
  CHECK(!engine->ShouldBlock(MakeReq(L"https://ads.example.com/x",
                                     L"sub.example.com")));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://ads.example.com/x",
                                     L"unrelated.org")));
}

static void TestWildcardsAndSeparator() {
  auto engine = Parse("||ads.example.com*/track*pixel\n"
                      "/adserver/^$third-party\n");
  CHECK(engine->ShouldBlock(MakeReq(
      L"https://ads.example.com/track/x?pixel=1", L"site.com")));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://ads.example.com/other",
                                     L"site.com")));
  // Plain pattern with ^: /adserver/ followed by a separator (end, ?, ...).
  CHECK(engine->ShouldBlock(MakeReq(L"https://cdn.other.com/adserver/?q=1",
                                    L"news.com")));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://cdn.other.com/adserver/req",
                                     L"news.com")));  // no separator there
  CHECK(!engine->ShouldBlock(MakeReq(L"https://cdn.other.com/adserverfile",
                                     L"news.com")));
}

static void TestUnsupportedAndCosmetic() {
  ParseReport rep;
  auto engine = Parse(
      "##.banner\n"
      "#@#.promo\n"
      "example.com##div#top\n"
      "||ads.example.com^$popup\n"
      "||x.example.com^$csp=script-src 'none'\n"
      "/regexp.*[0-9]{2}/\n"
      "||ads.example.com^\n",
      &rep);
  CHECK(rep.cosmetic_skipped == 3);
  CHECK(rep.unsupported_skipped == 2);
  CHECK(rep.accepted == 1);
  // Cosmetic rules must not affect network matching.
  CHECK(engine->ShouldBlock(MakeReq(L"https://ads.example.com/a.png",
                                    L"site.com", ResourceKind::Image)));
  CHECK(!engine->ShouldBlock(MakeReq(L"https://site.com/.banner/x.png",
                                     L"site.com", ResourceKind::Image)));
}

static void TestInvalidLines() {
  ParseReport rep;
  Parse("$document,domain=example.org\n"  // only unsupported options
        "\n"
        "   \n",  // whitespace line
        &rep);
  CHECK(rep.accepted == 0);
}

static void TestLooksLikeFilterList() {
  std::string tiny = "||ads.com^\n||ads2.com^\n";
  CHECK(!FilterEngine::LooksLikeFilterList(tiny, nullptr));
  std::string big = "[Adblock Plus 2.0]\n";
  for (int i = 0; i < 50; ++i) {
    big += "||ads" + std::to_string(i) + ".example.com^\n";
  }
  CHECK(FilterEngine::LooksLikeFilterList(big, nullptr));
  std::string html_page = "<html><body>hello world this is not a filter list "
                          "but it is long enough to pass the size check for "
                          "sure because we keep writing text and text</body></html>";
  CHECK(!FilterEngine::LooksLikeFilterList(html_page, nullptr));
}

// Phase 5 helper mirror: domains-only feeds convert to ||host^ rules.
static std::string DomainsToRulesMirror(const std::string& text) {
  std::string out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (!line.empty() && line[0] == '.') line.erase(line.begin());
    bool valid = !line.empty() && line.size() <= 253;
    if (valid) {
      for (char c : line) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                        c == '_';
        if (!ok) {
          valid = false;
          break;
        }
      }
    }
    if (valid) {
      out += "||";
      out += line;
      out += '^';
    } else if (!line.empty()) {
      out += "! skipped: ";
      out += line;
    }
    out += '\n';
  }
  return out;
}

static void TestDomainBlocklistConversion() {
  const std::string feed =
      "# phishing feed\n"
      "phishing-example.com\n"
      ".sub.evil-host.net\n"
      "not a domain line\n"
      "valid-host.org\r\n";
  auto engine = Parse(DomainsToRulesMirror(feed));
  CHECK(engine->ShouldBlock(MakeReq(L"http://phishing-example.com/x",
                                   L"example.com", ResourceKind::Other)));
  CHECK(engine->ShouldBlock(MakeReq(L"http://sub.evil-host.net/x",
                                   L"example.com", ResourceKind::Other)));
  CHECK(engine->ShouldBlock(MakeReq(L"http://valid-host.org/x", L"example.com",
                                   ResourceKind::Other)));
  CHECK(!engine->ShouldBlock(MakeReq(L"http://unlisted-host.net/x",
                                   L"unlisted-host.net",
                                   ResourceKind::Other)));
}

int main() {
  TestBasicHostBlocking();
  TestLookalikeHost();
  TestExceptionsAndImportant();
  TestThirdPartyOption();
  TestResourceKindOptions();
  TestDomainOptions();
  TestWildcardsAndSeparator();
  TestUnsupportedAndCosmetic();
  TestInvalidLines();
  TestLooksLikeFilterList();
  TestDomainBlocklistConversion();
  if (g_failures == 0) {
    std::printf("FILTER ENGINE TESTS PASSED\n");
    return 0;
  }
  std::printf("FILTER ENGINE TESTS FAILED (%d)\n", g_failures);
  return 1;
}
