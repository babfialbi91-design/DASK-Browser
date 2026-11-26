// DASK Browser - adblock filter engine (Phase 3).
//
// A focused, dependency-free parser and matcher for the network-level subset
// of the Adblock Plus filter syntax used by EasyList / EasyPrivacy.
//
// Scope (documented in README "Privacy & Ad Blocking"):
//   * Network rules only: ||, |, ^, *, and the $ options third-party,
//     ~third-party, resource types (script/image/stylesheet/subdocument/
//     xmlhttprequest/media/font/object/other), domain=, ~domain=, important.
//   * Cosmetic rules (##, #@#, #$#, #%#, ...) are counted but NOT applied -
//     the engine never hides page elements.
//   * Rules with unsupported or unknown $ options (popup, document, csp=,
//     genericblock, ...) are skipped rather than mis-applied.
//
// Why a local parser instead of a third-party engine library? There is no
// maintained C++ filtering library that builds cleanly with CEF's CMake/MSVC
// setup (libadblockplus requires a bundled V8; adblock-rust needs a Rust
// toolchain; JS engines embed a second V8). The network-rule subset above is
// small, well-specified and fully covered by unit tests.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace dask::adblock {

// Resource categories a request can belong to. Mirrors the CEF resource types
// we care about; kept independent of CEF so the engine stays unit-testable.
enum class ResourceKind : int {
  Other = 0,
  Script,
  Image,
  Stylesheet,
  Subdocument,
  XmlHttpRequest,
  Media,
  Font,
  Object,
};

struct RequestInfo {
  std::wstring url;          // full URL, lowercased by the caller's helper
  std::wstring host;         // host of |url|, lowercased
  std::wstring first_party;  // top-level document host, lowercased ("" if none)
  ResourceKind kind = ResourceKind::Other;
  bool third_party = false;  // request URL host differs from first-party site
};

struct ParseReport {
  size_t accepted = 0;              // usable network rules
  size_t exceptions = 0;            // accepted @@ exception rules
  size_t cosmetic_skipped = 0;      // element-hiding rules (not supported)
  size_t unsupported_skipped = 0;   // unknown/unsupported $ options
  size_t invalid_skipped = 0;       // malformed lines
  size_t comments = 0;              // ! comments and [Adblock] headers
};

class FilterEngine {
 public:
  // Parses EasyList-format text into a matching engine. Never throws; lines
  // that cannot be used are counted in |report| instead of aborting.
  static std::unique_ptr<FilterEngine> Parse(const std::string& text,
                                             ParseReport* report = nullptr);

  // Returns true when the request should be blocked. Exception rules win
  // unless a blocking rule marked $important also matches.
  bool ShouldBlock(const RequestInfo& req,
                   std::string* matched_rule = nullptr) const;

  // True when the text is a plausible filter list (used to reject truncated
  // or corrupted downloads before swapping the engine).
  static bool LooksLikeFilterList(const std::string& text, ParseReport* report);

 ~FilterEngine();

 private:
  FilterEngine() = default;

  struct Rule;  // defined in filter_engine.cpp

  // ||host-anchored rules, indexed by the domain suffix they anchor to.
  std::vector<std::unique_ptr<Rule>> host_rules_;
  std::vector<size_t> host_rule_ids_;   // indices into rules_
  // Plain substring rules (no || anchor) - small subset, linear scan.
  std::vector<size_t> plain_rule_ids_;
  std::vector<std::unique_ptr<Rule>> rules_;
  // Suffix -> positions in host_rule_ids_.
  std::vector<std::pair<std::wstring, size_t>> host_index_;
};

}  // namespace dask::adblock
