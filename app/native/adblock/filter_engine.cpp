// DASK Browser - adblock filter engine implementation. See filter_engine.h
// for the supported syntax subset and design rationale.
#include "filter_engine.h"

#include <algorithm>

namespace dask::adblock {
namespace {

std::wstring AsciiLower(std::wstring s) {
  for (wchar_t& c : s) {
    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
  }
  return s;
}

bool IsSeparatorChar(wchar_t c) {
  // Adblock "separator" ^: any character that is not a letter, digit, or one
  // of _ - . % (the excluded chars appear inside hosts and encoded paths).
  if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) return false;
  return !(c == L'_' || c == L'-' || c == L'.' || c == L'%');
}

bool IsOptionChar(wchar_t c) {
  return c == L',' || c == L'|' || c == L'~' || c == L'=' || c == L'-' ||
         c == L'.' || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}

// Splits a filter line into pattern + option string ("$..." removed).
// Returns false when the line contains a '$' that is not followed by a valid
// option list (such patterns are skipped as invalid).
bool SplitPatternOptions(const std::wstring& line, std::wstring* pattern,
                         std::wstring* options) {
  const size_t dollar = line.rfind(L'$');
  if (dollar == std::wstring::npos) {
    *pattern = line;
    options->clear();
    return true;
  }
  std::wstring opts = line.substr(dollar + 1);
  const bool looks_like_options =
      !opts.empty() && std::all_of(opts.begin(), opts.end(), IsOptionChar);
  if (looks_like_options) {
    *pattern = line.substr(0, dollar);
    *options = opts;
    return true;
  }
  return false;  // '$' without a valid option list
}

// One segment (no '*'): matches |seg| against |url| starting at |pos|.
bool MatchSegment(const std::wstring& seg, const std::wstring& url, size_t pos,
                  size_t* end_pos) {
  size_t u = pos;
  for (size_t s = 0; s < seg.size(); ++s) {
    const wchar_t sc = seg[s];
    if (u >= url.size()) {
      // A trailing '^' may match the end of the URL.
      if (sc == L'^' && s + 1 == seg.size()) {
        *end_pos = u;
        return true;
      }
      return false;
    }
    if (sc == L'^') {
      if (!IsSeparatorChar(url[u])) return false;
    } else if (url[u] != sc) {
      return false;
    }
    ++u;
  }
  *end_pos = u;
  return true;
}

// Matches segments starting at |i|, allowing each to appear anywhere from
// |pos| onward (the '*' before them grants the gap).
bool MatchFrom(const std::vector<std::wstring>& segs, size_t i,
               const std::wstring& url, size_t pos, bool end_anchor) {
  while (i < segs.size() && segs[i].empty()) ++i;
  if (i == segs.size()) {
    return end_anchor ? pos == url.size() : true;
  }
  for (size_t p = pos; p <= url.size(); ++p) {
    size_t end = 0;
    if (!MatchSegment(segs[i], url, p, &end)) continue;
    size_t next = i + 1;
    while (next < segs.size() && segs[next].empty()) ++next;
    if (next == segs.size()) {
      if (!end_anchor || end == url.size()) return true;
      continue;  // keep scanning for a match that ends at the URL end
    }
    if (MatchFrom(segs, next, url, end, end_anchor)) return true;
  }
  return false;
}

// Matches |segs| (joined by '*') with the first segment fixed at |pos|:
// used for '|' and '||' anchored patterns.
bool MatchRuleAt(const std::vector<std::wstring>& segs, const std::wstring& url,
                 size_t pos, bool end_anchor) {
  size_t i = 0;
  while (i < segs.size() && segs[i].empty()) ++i;
  if (i == segs.size()) {
    return end_anchor ? pos == url.size() : true;
  }
  size_t end = 0;
  if (!MatchSegment(segs[i], url, pos, &end)) return false;
  size_t next = i + 1;
  while (next < segs.size() && segs[next].empty()) ++next;
  if (next == segs.size()) {
    return !end_anchor || end == url.size();
  }
  return MatchFrom(segs, next, url, end, end_anchor);
}

// Does |host| equal |domain| or end with "." + domain?
bool HostSuffixMatch(const std::wstring& host, const std::wstring& domain) {
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

}  // namespace

FilterEngine::~FilterEngine() = default;

struct FilterEngine::Rule {
  enum class Anchor : uint8_t { None, Start, Host };
  // Pattern split on '*'. '^' inside a segment matches any separator.
  std::vector<std::wstring> segments;
  Anchor anchor = Anchor::None;  // Host means '||' prefix.
  bool end_anchor = false;
  bool exception = false;
  bool important = false;
  bool third_party_only = false;
  bool first_party_only = false;
  // Empty set matches every kind; otherwise the rule applies only to these.
  std::vector<ResourceKind> kinds;
  // domain= / ~domain= restrictions (suffix match on the first-party host).
  std::vector<std::wstring> include_domains;
  std::vector<std::wstring> exclude_domains;
  std::string raw;  // original line for diagnostics
};

std::unique_ptr<FilterEngine> FilterEngine::Parse(const std::string& text,
                                                  ParseReport* report) {
  ParseReport local;
  ParseReport& rep = report ? *report : local;
  std::unique_ptr<FilterEngine> engine(new FilterEngine());

  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    size_t begin = pos;
    size_t end = eol;
    pos = eol + 1;
    if (end > begin && text[end - 1] == '\r') --end;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    if (begin == end) continue;

    std::string line_utf8 = text.substr(begin, end - begin);
    if (line_utf8[0] == '!' ||
        (line_utf8[0] == '[' &&
         line_utf8.compare(0, 12, "[Adblock Plus") == 0)) {
      ++rep.comments;
      continue;
    }

    // Keep ASCII bytes; map others to '?' (non-ASCII patterns are not
    // supported at network level).
    std::wstring line;
    line.reserve(line_utf8.size());
    for (unsigned char c : line_utf8) {
      line.push_back(c < 0x80 ? static_cast<wchar_t>(c) : L'?');
    }
    line = AsciiLower(std::move(line));

    // Any '#...' marker means an element-hiding / cosmetic rule: counted,
    // never applied.
    if (line.find(L'#') != std::wstring::npos) {
      ++rep.cosmetic_skipped;
      continue;
    }

    bool exception = false;
    if (line.rfind(L"@@", 0) == 0) {
      exception = true;
      line.erase(0, 2);
    }

    std::wstring pattern, options;
    if (!SplitPatternOptions(line, &pattern, &options)) {
      ++rep.invalid_skipped;  // '$' without a valid option list
      continue;
    }

    Rule rule;
    rule.exception = exception;
    rule.raw = line_utf8;

    if (!options.empty()) {
      size_t opos = 0;
      bool skip_rule = false;
      while (opos <= options.size() && !skip_rule) {
        size_t next = options.find(L',', opos);
        if (next == std::wstring::npos) next = options.size();
        const std::wstring opt = options.substr(opos, next - opos);
        opos = next + 1;
        if (opt.empty()) continue;
        if (opt == L"third-party") {
          rule.third_party_only = true;
        } else if (opt == L"~third-party") {
          rule.first_party_only = true;
        } else if (opt == L"important") {
          if (!exception) rule.important = true;
        } else if (opt.rfind(L"domain=", 0) == 0) {
          const std::wstring domains = opt.substr(7);
          size_t dpos = 0;
          while (dpos <= domains.size()) {
            size_t dnext = domains.find(L'|', dpos);
            if (dnext == std::wstring::npos) dnext = domains.size();
            std::wstring dom = domains.substr(dpos, dnext - dpos);
            dpos = dnext + 1;
            if (dom.empty()) continue;
            if (dom[0] == L'~') {
              rule.exclude_domains.push_back(dom.substr(1));
            } else {
              rule.include_domains.push_back(dom);
            }
          }
          if (rule.include_domains.empty() && rule.exclude_domains.empty()) {
            skip_rule = true;
          }
        } else if (opt == L"script") {
          rule.kinds.push_back(ResourceKind::Script);
        } else if (opt == L"image") {
          rule.kinds.push_back(ResourceKind::Image);
        } else if (opt == L"stylesheet") {
          rule.kinds.push_back(ResourceKind::Stylesheet);
        } else if (opt == L"subdocument") {
          rule.kinds.push_back(ResourceKind::Subdocument);
        } else if (opt == L"xmlhttprequest") {
          rule.kinds.push_back(ResourceKind::XmlHttpRequest);
        } else if (opt == L"media") {
          rule.kinds.push_back(ResourceKind::Media);
        } else if (opt == L"font") {
          rule.kinds.push_back(ResourceKind::Font);
        } else if (opt == L"object") {
          rule.kinds.push_back(ResourceKind::Object);
        } else if (opt == L"other") {
          rule.kinds.push_back(ResourceKind::Other);
        } else {
          // popup, document, elemhide, generichide, genericblock, csp=,
          // websocket, ping, match-case, unknown - skip the rule safely.
          skip_rule = true;
        }
      }
      if (skip_rule) {
        ++rep.unsupported_skipped;
        continue;
      }
    }

    // Pattern anchors.
    std::wstring body = pattern;
    if (body.rfind(L"||", 0) == 0) {
      rule.anchor = Rule::Anchor::Host;
      body.erase(0, 2);
    } else if (!body.empty() && body[0] == L'|') {
      rule.anchor = Rule::Anchor::Start;
      body.erase(0, 1);
    }
    if (body.size() >= 2 && body[body.size() - 1] == L'|') {
      rule.end_anchor = true;
      body.pop_back();
    }
    if (body.empty()) {
      ++rep.invalid_skipped;
      continue;
    }
    // Regular-expression patterns: only when the inner text has real regex
    // metacharacters. Plain '/text/' patterns are treated as substring
    // patterns (which is what a literal regex would match anyway).
    if (body.front() == L'/' && body.back() == L'/' && body.size() >= 2) {
      static const std::wstring kRegexChars = L"\\^$[](){}|?";
      bool regex_like = false;
      for (size_t i = 1; i + 1 < body.size(); ++i) {
        if (kRegexChars.find(body[i]) != std::wstring::npos) {
          regex_like = true;
          break;
        }
      }
      if (regex_like) {
        ++rep.unsupported_skipped;
        continue;
      }
      body.erase(0, 1);
      body.pop_back();
    }

    // Split on '*'.
    {
      std::vector<std::wstring> segs;
      std::wstring cur;
      for (wchar_t c : body) {
        if (c == L'*') {
          segs.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
      segs.push_back(cur);
      rule.segments = std::move(segs);
    }

    const size_t rule_id = engine->rules_.size();
    engine->rules_.push_back(std::make_unique<Rule>(std::move(rule)));
    const FilterEngine::Rule& stored = *engine->rules_.back();

    if (stored.anchor == Rule::Anchor::Host) {
      // Index by the literal host part (characters before ^ * / ? :).
      std::wstring host_part;
      bool literal = true;
      for (wchar_t c : stored.segments[0]) {
        if (c == L'^' || c == L'*' || c == L'/' || c == L'?' || c == L':') break;
        if (c == L'~' || c == L'@' || c == L'|') {
          literal = false;
          break;
        }
        host_part.push_back(c);
      }
      if (literal && host_part.size() >= 2) {
        engine->host_rule_ids_.push_back(rule_id);
        engine->host_index_.emplace_back(host_part, rule_id);
        ++rep.accepted;
        if (stored.exception) ++rep.exceptions;
        continue;
      }
    }
    engine->plain_rule_ids_.push_back(rule_id);
    ++rep.accepted;
    if (stored.exception) ++rep.exceptions;
  }

  // The host index must be sorted for equal_range lookups.
  std::sort(engine->host_index_.begin(), engine->host_index_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return engine;
}

bool FilterEngine::LooksLikeFilterList(const std::string& text,
                                       ParseReport* report) {
  if (text.size() < 1024 || text.size() > 32 * 1024 * 1024) return false;
  ParseReport rep;
  ParseReport& r = report ? *report : rep;
  Parse(text, &r);
  return r.accepted >= 10;
}

bool FilterEngine::ShouldBlock(const RequestInfo& req,
                               std::string* matched_rule) const {
  if (matched_rule) matched_rule->clear();
  if (req.url.empty()) return false;

  bool block_matched = false;
  bool important_matched = false;
  bool exception_matched = false;
  std::string important_rule;

  auto consider = [&](const Rule& rule) -> void {
    // Option gating.
    if (rule.third_party_only && !req.third_party) return;
    if (rule.first_party_only && req.third_party) return;
    if (!rule.kinds.empty() &&
        std::find(rule.kinds.begin(), rule.kinds.end(), req.kind) ==
            rule.kinds.end()) {
      return;
    }
    if (!rule.include_domains.empty() &&
        !std::any_of(rule.include_domains.begin(), rule.include_domains.end(),
                     [&](const std::wstring& d) {
                       return HostSuffixMatch(req.first_party, d);
                     })) {
      return;
    }
    if (std::any_of(rule.exclude_domains.begin(), rule.exclude_domains.end(),
                    [&](const std::wstring& d) {
                      return HostSuffixMatch(req.first_party, d);
                    })) {
      return;
    }

    // Pattern matching. '||' matches at the hostname start or at any dot
    // boundary inside the hostname (Adblock domain-boundary semantics).
    bool hit = false;
    if (rule.anchor == Rule::Anchor::Host) {
      const size_t scheme_end = req.url.find(L"://");
      const size_t host_start =
          scheme_end == std::wstring::npos ? 0 : scheme_end + 3;
      for (size_t p = host_start; p <= req.url.size(); ++p) {
        if (p == host_start ||
            (p > host_start && req.url[p - 1] == L'.')) {
          if (MatchRuleAt(rule.segments, req.url, p, rule.end_anchor)) {
            hit = true;
            break;
          }
        }
        if (p < req.url.size() && req.url[p] == L'/') break;  // left hostname
      }
    } else if (rule.anchor == Rule::Anchor::Start) {
      hit = MatchRuleAt(rule.segments, req.url, 0, rule.end_anchor);
    } else {
      // Plain substring: try every occurrence of the first literal run of
      // the first segment (up to the first '^' or '*', which are matcher
      // characters, not literal text).
      std::wstring search_key;
      for (const std::wstring& seg : rule.segments) {
        if (!seg.empty()) {
          for (wchar_t ch : seg) {
            if (ch == L'^' || ch == L'*') break;
            search_key.push_back(ch);
          }
          break;
        }
      }
      size_t from = 0;
      while (true) {
        size_t first_literal;
        if (search_key.empty()) {
          if (from > req.url.size()) break;
          first_literal = from++;  // no literal anchor: brute force
        } else {
          first_literal = req.url.find(search_key, from);
          if (first_literal == std::wstring::npos) break;
          from = first_literal + 1;
        }
        if (MatchRuleAt(rule.segments, req.url, first_literal,
                        rule.end_anchor)) {
          hit = true;
          break;
        }
      }
    }
    if (!hit) return;

    if (rule.exception) {
      exception_matched = true;
    } else if (rule.important) {
      important_matched = true;
      if (important_rule.empty()) important_rule = rule.raw;
    } else {
      block_matched = true;
    }
  };

  // Host-indexed || rules: candidates are the request host and every
  // subdomain suffix, plus prefix truncations at dot boundaries (blocks
  // lookalike hosts like example.com.evil.com for ||example.com^).
  {
    const std::wstring& host = req.host;
    if (!host.empty()) {
      std::vector<std::wstring> candidates;
      candidates.push_back(host);
      for (size_t i = 0; i < host.size(); ++i) {
        if (host[i] == L'.') {
          candidates.push_back(host.substr(0, i));   // prefix truncation
          candidates.push_back(host.substr(i + 1));  // subdomain suffix
        }
      }
      for (const std::wstring& key : candidates) {
        if (key.size() < 2) continue;
        const std::pair<std::wstring, size_t> probe{key, 0};
        const auto range = std::equal_range(
            host_index_.begin(), host_index_.end(), probe,
            [](const std::pair<std::wstring, size_t>& a,
               const std::pair<std::wstring, size_t>& b) {
              return a.first < b.first;
            });
        for (auto it = range.first; it != range.second; ++it) {
          consider(*rules_[it->second]);
        }
        if (important_matched) break;
      }
    }
  }

  // Plain rules (small subset): linear scan.
  for (size_t id : plain_rule_ids_) {
    consider(*rules_[id]);
    if (important_matched) break;
  }

  if (important_matched) {
    if (matched_rule) *matched_rule = important_rule;
    return true;
  }
  if (block_matched && !exception_matched) {
    return true;
  }
  return false;
}

}  // namespace dask::adblock
