// DASK Browser - storage repositories on top of Database (Phase 4).
// All SQL is parameterized. Titles/URLs are stored as UTF-8.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "database.h"

namespace dask::storage {

// ---- Bookmarks -------------------------------------------------------------

struct Bookmark {
  int64_t id = 0;
  std::string title;
  std::string url;
  int64_t created_at = 0;
};

class Bookmarks {
 public:
  explicit Bookmarks(Database* db) : db_(db) {}

  // Adds or updates (same URL replaces the title). Returns the row id.
  bool Add(const std::string& title, const std::string& url, int64_t* id,
           std::string* error);
  bool Update(int64_t id, const std::string& title, const std::string& url,
              std::string* error);
  bool Remove(int64_t id, std::string* error);
  bool FindByUrl(const std::string& url, Bookmark* out, std::string* error);
  // Case-insensitive substring search over title and url (LIKE). Empty query
  // returns everything (newest first). Limited to 200 rows.
  bool Search(const std::string& query, std::vector<Bookmark>* out,
              std::string* error);

 private:
  Database* db_;
};

// ---- History ---------------------------------------------------------------

struct HistoryItem {
  int64_t id = 0;
  std::string url;
  std::string title;
  int64_t visited_at = 0;
};

class History {
 public:
  explicit History(Database* db) : db_(db) {}

  // Records a visit; a repeat visit to the same URL within 60 seconds only
  // refreshes the timestamp (dedupe). Returns the row id (0 on no-op).
  bool Add(const std::string& url, const std::string& title, int64_t now,
           int64_t* id, std::string* error);
  bool Search(const std::string& query, int limit,
              std::vector<HistoryItem>* out, std::string* error);
  // Deletes everything, or visits older than |days| when days > 0.
  bool Clear(int days, std::string* error);
  // Deletes visits older than |older_than| (unix seconds). Used at startup
  // for the retention policy.
  bool Prune(int64_t older_than, std::string* error);

 private:
  Database* db_;
};

// ---- Downloads -------------------------------------------------------------

// Download states stored in the DB.
enum DownloadState : int {
  kDownloadActive = 0,
  kDownloadComplete = 1,
  kDownloadCanceled = 2,
  kDownloadFailed = 3,
};

struct DownloadItemRow {
  int64_t id = 0;
  std::string url;
  std::string path;      // full file path
  std::string filename;  // display name
  int state = kDownloadActive;
  int64_t received = 0;
  int64_t total = -1;    // -1 = unknown
  int64_t started_at = 0;
};

class Downloads {
 public:
  explicit Downloads(Database* db) : db_(db) {}

  bool Insert(const std::string& url, const std::string& path,
              const std::string& filename, int64_t started_at, int64_t* id,
              std::string* error);
  bool UpdateProgress(int64_t id, int state, int64_t received, int64_t total,
                      std::string* error);
  // Newest first, limited.
  bool List(int limit, std::vector<DownloadItemRow>* out, std::string* error);
  bool Delete(int64_t id, std::string* error);

 private:
  Database* db_;
};

// ---- Settings (key/value) ---------------------------------------------------

class Settings {
 public:
  explicit Settings(Database* db) : db_(db) {}

  bool GetString(const std::string& key, const std::string& def,
                 std::string* out, std::string* error);
  bool SetString(const std::string& key, const std::string& value,
                 std::string* error);
  bool GetBool(const std::string& key, bool def, bool* out,
               std::string* error);
  bool SetBool(const std::string& key, bool value, std::string* error);
  bool GetInt(const std::string& key, int def, int* out, std::string* error);
  bool SetInt(const std::string& key, int value, std::string* error);

 private:
  Database* db_;
};

// ---- Permissions (per-site decisions) --------------------------------------

enum PermissionDecision {
  kPermissionAsk = 0,   // default: prompt every time
  kPermissionAllow = 1, // remembered allow
  kPermissionDeny = 2,  // remembered deny
};

struct PermissionRow {
  std::string host;
  std::string type;  // camera / microphone / geolocation / notifications / clipboard
  int decision = kPermissionAsk;
  int64_t updated_at = 0;
};

class Permissions {
 public:
  explicit Permissions(Database* db) : db_(db) {}

  // Stored decision for (host, type); kPermissionAsk when absent.
  int Get(const std::string& host, const std::string& type,
          std::string* error);
  // Upsert. decision must be kPermissionAllow/kPermissionDeny/kPermissionAsk.
  bool Set(const std::string& host, const std::string& type, int decision,
           std::string* error);
  bool List(std::vector<PermissionRow>* out, std::string* error);
  bool Remove(const std::string& host, const std::string& type,
              std::string* error);
  bool ClearAll(std::string* error);

 private:
  Database* db_;
};

}  // namespace dask::storage
