// DASK Browser - local database implementation.
#include "database.h"

#include <memory>
#include <windows.h>

namespace dask::storage {
namespace {

bool IsCorruptCode(int code) {
  return code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
}

std::string ErrOf(sqlite3* db) {
  const char* msg = db ? sqlite3_errmsg(db) : "database not open";
  return msg ? msg : "unknown error";
}

// Converts a UTF-8 path for the Windows filesystem API.
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
  return out;
}

}  // namespace

Database::~Database() { Close(); }

bool Database::Open(const std::string& path_utf8, std::string* error) {
  path_utf8_ = path_utf8;
  corrupted_ = false;
  const int rc = sqlite3_open_v2(
      path_utf8.c_str(), &db_,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (rc != SQLITE_OK) {
    if (error) *error = "open failed: " + ErrOf(db_);
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    return false;
  }
  // Phase 7: several browser windows (second process) may open the same
  // profile database. A generous busy timeout turns rare writer contention
  // into a short wait instead of an SQLITE_BUSY error surfaced to the UI.
  sqlite3_busy_timeout(db_, 10000);
  return true;
}

void Database::Close() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

bool Database::Exec(const char* sql, std::string* error) {
  if (!db_) {
    if (error) *error = "database not open";
    return false;
  }
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    if (IsCorruptCode(rc) || IsCorruptCode(sqlite3_errcode(db_))) {
      corrupted_ = true;
    }
    if (error) *error = ErrOf(db_);
    return false;
  }
  return true;
}

std::unique_ptr<Database::Stmt> Database::Prepare(const char* sql) {
  std::unique_ptr<Stmt> stmt(new Stmt(this, nullptr));
  if (!db_) return stmt;
  sqlite3_stmt* raw = nullptr;
  const int rc =
      sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
  if (rc != SQLITE_OK) {
    if (IsCorruptCode(rc)) corrupted_ = true;
    if (raw) sqlite3_finalize(raw);
    return stmt;
  }
  return std::unique_ptr<Stmt>(new Stmt(this, raw));
}

int64_t Database::LastInsertId() {
  return db_ ? sqlite3_last_insert_rowid(db_) : 0;
}

int Database::Changes() {
  return db_ ? sqlite3_changes(db_) : 0;
}

bool Database::Migrate(std::string* error) {
  if (!db_) {
    if (error) *error = "database not open";
    return false;
  }
  int version = SchemaVersion(this);
  if (version < 0) {
    if (error) *error = "cannot read schema version: " + ErrOf(db_);
    return false;
  }

  // Migration steps: each appends one version bump. Step vN must transform
  // the schema from version N-1 to N.
  static const char* kMigrations[] = {
      // v0 -> v1: initial schema.
      "CREATE TABLE IF NOT EXISTS bookmarks ("
      "  id INTEGER PRIMARY KEY,"
      "  title TEXT NOT NULL,"
      "  url TEXT NOT NULL UNIQUE,"
      "  created_at INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS history ("
      "  id INTEGER PRIMARY KEY,"
      "  url TEXT NOT NULL,"
      "  title TEXT NOT NULL,"
      "  visited_at INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_history_time ON history (visited_at);"
      "CREATE INDEX IF NOT EXISTS idx_history_url ON history (url);"
      "CREATE TABLE IF NOT EXISTS downloads ("
      "  id INTEGER PRIMARY KEY,"
      "  url TEXT NOT NULL,"
      "  path TEXT NOT NULL,"
      "  filename TEXT NOT NULL,"
      "  state INTEGER NOT NULL,"
      "  received INTEGER NOT NULL DEFAULT 0,"
      "  total INTEGER NOT NULL DEFAULT -1,"
      "  started_at INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_downloads_time ON downloads (started_at);"
      "CREATE TABLE IF NOT EXISTS settings ("
      "  key TEXT PRIMARY KEY,"
      "  value TEXT NOT NULL);",
      // v1 -> v2: per-site permission decisions (Phase 5).
      "CREATE TABLE IF NOT EXISTS permissions ("
      "  host TEXT NOT NULL,"
      "  type TEXT NOT NULL,"
      "  decision INTEGER NOT NULL,"
      "  updated_at INTEGER NOT NULL,"
      "  PRIMARY KEY (host, type));",
      // v2 -> v3: local password manager (DPAPI-encrypted secrets) +
      // user-script extensions (Phase 10).
      "CREATE TABLE IF NOT EXISTS passwords ("
      "  id INTEGER PRIMARY KEY,"
      "  origin TEXT NOT NULL,"
      "  username TEXT NOT NULL,"
      "  secret TEXT NOT NULL,"  // DPAPI blob, base64 — never plaintext
      "  updated_at INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_passwords_origin ON passwords (origin);"
      "CREATE TABLE IF NOT EXISTS user_scripts ("
      "  id INTEGER PRIMARY KEY,"
      "  name TEXT NOT NULL,"
      "  matches TEXT NOT NULL,"
      "  code TEXT NOT NULL,"
      "  enabled INTEGER NOT NULL DEFAULT 1,"
      "  created_at INTEGER NOT NULL);",
  };
  constexpr int kSteps = sizeof(kMigrations) / sizeof(kMigrations[0]);

  while (version < kSteps) {
    const char* step_sql = kMigrations[version];
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_exec(db_, step_sql, nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_exec(
            db_,
            ("PRAGMA user_version = " + std::to_string(version + 1)).c_str(),
            nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
      sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      if (IsCorruptCode(sqlite3_errcode(db_))) corrupted_ = true;
      if (error) *error = "migration failed: " + ErrOf(db_);
      return false;
    }
    ++version;
  }
  if (version > kSteps) {
    // Database written by a NEWER browser build: keep working read-only-ish
    // rather than destroying data, but surface it.
    if (error) *error = "database schema is newer than this build";
    return false;
  }
  return true;
}

bool Database::Recreate(std::string* error) {
  Close();
  ::DeleteFileW(Utf8ToWide(path_utf8_).c_str());
  corrupted_ = false;
  if (!Open(path_utf8_, error)) return false;
  return Migrate(error);
}

int SchemaVersion(Database* db) {
  if (!db || !db->opened()) return -1;
  auto stmt = db->Prepare("PRAGMA user_version");
  if (!stmt || !stmt->Step()) return -1;
  return static_cast<int>(stmt->ColInt(0));
}

// ---- Stmt ------------------------------------------------------------------

Database::Stmt& Database::Stmt::Bind(int index, int64_t value) {
  if (raw_) sqlite3_bind_int64(raw_, index, value);
  return *this;
}

Database::Stmt& Database::Stmt::Bind(int index, const std::string& text) {
  if (raw_) {
    sqlite3_bind_text(raw_, index, text.c_str(),
                      static_cast<int>(text.size()), SQLITE_TRANSIENT);
  }
  return *this;
}

Database::Stmt& Database::Stmt::BindNull(int index) {
  if (raw_) sqlite3_bind_null(raw_, index);
  return *this;
}

bool Database::Stmt::Step() {
  if (!raw_) {
    error_ = db_ ? "prepare failed: " + ErrOf(db_->db_) : "database not open";
    return false;
  }
  const int rc = sqlite3_step(raw_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  error_ = ErrOf(db_->db_);
  if (IsCorruptCode(rc) || IsCorruptCode(sqlite3_errcode(db_->db_))) {
    db_->corrupted_ = true;
  }
  sqlite3_reset(raw_);
  return false;
}

bool Database::Stmt::Run() {
  while (Step()) {
  }
  return error_.empty();
}

int64_t Database::Stmt::ColInt(int col) const {
  return raw_ ? sqlite3_column_int64(raw_, col) : 0;
}

std::string Database::Stmt::ColText(int col) const {
  if (!raw_) return std::string();
  const unsigned char* text = sqlite3_column_text(raw_, col);
  return text ? reinterpret_cast<const char*>(text) : std::string();
}

bool Database::Stmt::IsCorrupt() const {
  return db_ && db_->corrupted_;
}

}  // namespace dask::storage
