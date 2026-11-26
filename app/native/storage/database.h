// DASK Browser - local database (SQLite via the system winsqlite3.dll).
//
// Owns the connection, schema migrations (versioned) and corruption
// recovery. All queries run on a single dedicated DB thread - this class is
// NOT thread-safe by design; callers must confine usage to one thread (a
// FULLMUTEX connection only serializes access, it does not make our logic
// thread-safe).
#pragma once

#include <memory>

#include "sqlite3_min.h"

#include <string>
#include <vector>

namespace dask::storage {

// Every schema change appends a migration step; kLatestVersion must match
// the number of steps applied.
constexpr int kLatestVersion = 2;

class Database {
 public:
  ~Database();

  // Opens (creating the file if needed). Returns false with |error| set.
  bool Open(const std::string& path_utf8, std::string* error);
  void Close();

  // Applies migrations up to kLatestVersion. Returns false with |error| set.
  bool Migrate(std::string* error);

  // True when the last operation hit a corrupted / non-database file.
  bool corrupted() const { return corrupted_; }

  // Recovery: close, delete the file, reopen and migrate. Only useful after
  // corrupted() - the old data is lost, the browser keeps working.
  bool Recreate(std::string* error);

  bool opened() const { return db_ != nullptr; }

  // A single prepared statement, parameterized by design.
  class Stmt {
   public:
    Stmt(Database* db, sqlite3_stmt* raw) : db_(db), raw_(raw) {}
    ~Stmt() { if (raw_) sqlite3_finalize(raw_); }

    Stmt& Bind(int index, int64_t value);
    Stmt& Bind(int index, const std::string& text);
    Stmt& BindNull(int index);

    // Steps once: true = a row is available, false = done (or error, which
    // also flips Database::corrupted()).
    bool Step();
    int64_t ColInt(int col) const;
    std::string ColText(int col) const;

    // Runs until done (INSERT/UPDATE/DELETE). Returns false on error.
    bool Run();

    const std::string& error() const { return error_; }
    bool IsCorrupt() const;

   private:
    Database* db_;
    sqlite3_stmt* raw_ = nullptr;
    std::string error_;
  };

  // Prepares |sql|; check opened()/error afterwards via the returned Stmt.
  std::unique_ptr<Stmt> Prepare(const char* sql);

  // Executes plain SQL (schema changes). Returns false on error.
  bool Exec(const char* sql, std::string* error);

  int64_t LastInsertId();
  int Changes();

 private:
  sqlite3* db_ = nullptr;
  bool corrupted_ = false;
  std::string path_utf8_;
};

// Reads PRAGMA user_version (-1 on failure).
int SchemaVersion(Database* db);

}  // namespace dask::storage
