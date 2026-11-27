// DASK Browser - filter list storage and updates (Phase 3).
//
// Stores EasyList / EasyPrivacy under %LOCALAPPDATA%\DaskBrowser\filters,
// downloads updates with WinHTTP (system TLS, sandbox-safe), validates
// content before accepting it, and swaps the matching engine atomically.
// A failed download keeps the previous list (never leaves the engine empty
// because of a transient network problem).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adblock/filter_engine.h"

namespace dask {

struct FilterListStatus {
  std::wstring name;    // "easylist" | "easyprivacy"
  std::wstring url;     // source URL
  size_t rules = 0;     // accepted network rules in the stored file
  std::wstring updated; // ISO-ish timestamp of the last successful update
  std::string status;   // "ok" | "loading" | "error: <reason>" | "not loaded"
};

class FilterLists {
 public:
  // Loads stored lists (if any) and builds the engine. Call once at startup
  // on the CEF UI thread.
  void Initialize(const std::wstring& appdata_dir);

  // Current engine (empty engine object when no lists are loaded yet).
  std::shared_ptr<const adblock::FilterEngine> engine() const {
    return engine_.load();
  }

  // Phase 7.5: rebuild the engine from stored files (after a disabled-list
  // change or a profile switch). Safe on the CEF UI thread.
  void RebuildEngine();

  std::vector<FilterListStatus> Statuses() const;   // any thread
  bool updating() const { return updating_.load(); }

  // Downloads all sources in the background, validates, writes atomically,
  // rebuilds the engine, then runs |on_done| on the CEF UI thread.
  void UpdateAll(std::function<void()> on_done);

  // Phase 7.5: lists the user disabled (kept on disk, excluded from engine).
  void SetDisabledLists(const std::vector<std::wstring>& names);
  bool IsListEnabled(const std::wstring& name) const;

 private:
  struct Source {
    const wchar_t* name;
    const wchar_t* url;
    bool domains;  // one bare hostname per line (converted to ||host^ rules)
  };
  static const Source kSources[];
  static constexpr size_t kSourceCount = 3;

  bool LoadStored(const Source& src);  // parse from disk into the engine
  // (RebuildEngine is public since Phase 7.5)
  void PersistMeta();                  // write meta.json

  std::wstring dir_;
  mutable std::mutex disabled_mutex_;
  std::vector<std::wstring> disabled_lists_;  // guarded by disabled_mutex_
  std::atomic<std::shared_ptr<const adblock::FilterEngine>> engine_;
  std::atomic<bool> updating_{false};
  std::vector<FilterListStatus> statuses_;  // guarded by status_mutex_
  mutable std::mutex status_mutex_;
};

}  // namespace dask
