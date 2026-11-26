// DASK Browser - storage repositories implementation.
#include "stores.h"

#include <cstdlib>

namespace dask::storage {
namespace {

// Escapes % and _ for a LIKE pattern and lowercases (ASCII) for
// case-insensitive matching.
std::string LikePattern(const std::string& query) {
  std::string out = "%";
  for (char c : query) {
    if (c == '%' || c == '_') out.push_back('\\');
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    out.push_back(c);
  }
  out.push_back('%');
  return out;
}

}  // namespace

// ---- Bookmarks -------------------------------------------------------------

bool Bookmarks::Add(const std::string& title, const std::string& url,
                    int64_t* id, std::string* error) {
  *id = 0;
  // Upsert by URL: one entry per address; a repeat add refreshes the title
  // and keeps the original created_at.
  auto stmt = db_->Prepare(
      "INSERT INTO bookmarks (title, url, created_at) VALUES (?, ?, strftime("
      "'%s','now')) ON CONFLICT(url) DO UPDATE SET title = excluded.title");
  if (!stmt) return false;
  stmt->Bind(1, title).Bind(2, url);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  Bookmark found;
  if (!FindByUrl(url, &found, error)) return false;
  *id = found.id;
  return true;
}

bool Bookmarks::Update(int64_t id, const std::string& title,
                       const std::string& url, std::string* error) {
  auto stmt =
      db_->Prepare("UPDATE bookmarks SET title = ?, url = ? WHERE id = ?");
  if (!stmt) return false;
  stmt->Bind(1, title).Bind(2, url).Bind(3, id);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return db_->Changes() > 0;
}

bool Bookmarks::Remove(int64_t id, std::string* error) {
  auto stmt = db_->Prepare("DELETE FROM bookmarks WHERE id = ?");
  if (!stmt) return false;
  stmt->Bind(1, id);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return db_->Changes() > 0;
}

bool Bookmarks::FindByUrl(const std::string& url, Bookmark* out,
                          std::string* error) {
  auto stmt = db_->Prepare(
      "SELECT id, title, url, created_at FROM bookmarks WHERE url = ?");
  if (!stmt) return false;
  stmt->Bind(1, url);
  if (stmt->Step()) {
    out->id = stmt->ColInt(0);
    out->title = stmt->ColText(1);
    out->url = stmt->ColText(2);
    out->created_at = stmt->ColInt(3);
    return true;
  }
  if (error && !stmt->error().empty()) *error = stmt->error();
  return false;
}

bool Bookmarks::Search(const std::string& query, std::vector<Bookmark>* out,
                       std::string* error) {
  out->clear();
  auto stmt = db_->Prepare(
      "SELECT id, title, url, created_at FROM bookmarks "
      "WHERE lower(title) LIKE ? ESCAPE '\\' OR lower(url) LIKE ? ESCAPE "
      "'\\' ORDER BY created_at DESC, id DESC LIMIT 200");
  if (!stmt) return false;
  const std::string pattern = LikePattern(query);
  stmt->Bind(1, pattern).Bind(2, pattern);
  while (stmt->Step()) {
    Bookmark item;
    item.id = stmt->ColInt(0);
    item.title = stmt->ColText(1);
    item.url = stmt->ColText(2);
    item.created_at = stmt->ColInt(3);
    out->push_back(std::move(item));
  }
  if (!stmt->error().empty()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

// ---- History ---------------------------------------------------------------

bool History::Add(const std::string& url, const std::string& title,
                  int64_t now, int64_t* id, std::string* error) {
  *id = 0;
  // Dedupe: same URL visited within the last 60 seconds only refreshes the
  // timestamp instead of adding a row.
  {
    auto stmt = db_->Prepare(
        "SELECT id FROM history WHERE url = ? AND visited_at > ? "
        "ORDER BY visited_at DESC LIMIT 1");
    if (!stmt) return false;
    stmt->Bind(1, url).Bind(2, now - 60);
    if (stmt->Step()) {
      const int64_t existing = stmt->ColInt(0);
      auto upd = db_->Prepare(
          "UPDATE history SET visited_at = ?, title = ? WHERE id = ?");
      if (!upd) return false;
      upd->Bind(1, now).Bind(2, title).Bind(3, existing);
      if (!upd->Run()) {
        if (error) *error = upd->error();
        return false;
      }
      *id = existing;
      return true;
    }
    if (!stmt->error().empty()) {
      if (error) *error = stmt->error();
      return false;
    }
  }
  auto stmt = db_->Prepare(
      "INSERT INTO history (url, title, visited_at) VALUES (?, ?, ?)");
  if (!stmt) return false;
  stmt->Bind(1, url).Bind(2, title).Bind(3, now);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  *id = db_->LastInsertId();
  return true;
}

bool History::Search(const std::string& query, int limit,
                     std::vector<HistoryItem>* out, std::string* error) {
  out->clear();
  if (limit <= 0) limit = 50;
  if (limit > 500) limit = 500;
  auto stmt = db_->Prepare(
      "SELECT id, url, title, visited_at FROM history "
      "WHERE lower(title) LIKE ? ESCAPE '\\' OR lower(url) LIKE ? ESCAPE "
      "'\\' ORDER BY visited_at DESC, id DESC LIMIT ?");
  if (!stmt) return false;
  const std::string pattern = LikePattern(query);
  stmt->Bind(1, pattern).Bind(2, pattern).Bind(3, static_cast<int64_t>(limit));
  while (stmt->Step()) {
    HistoryItem item;
    item.id = stmt->ColInt(0);
    item.url = stmt->ColText(1);
    item.title = stmt->ColText(2);
    item.visited_at = stmt->ColInt(3);
    out->push_back(std::move(item));
  }
  if (!stmt->error().empty()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool History::Clear(int days, std::string* error) {
  auto stmt = days > 0
                  ? db_->Prepare(
                        "DELETE FROM history WHERE visited_at < strftime("
                        "'%s','now') - ? * 86400")
                  : db_->Prepare("DELETE FROM history");
  if (!stmt) return false;
  if (days > 0) stmt->Bind(1, static_cast<int64_t>(days));
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool History::Prune(int64_t older_than, std::string* error) {
  auto stmt = db_->Prepare("DELETE FROM history WHERE visited_at < ?");
  if (!stmt) return false;
  stmt->Bind(1, older_than);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

// ---- Downloads -------------------------------------------------------------

bool Downloads::Insert(const std::string& url, const std::string& path,
                       const std::string& filename, int64_t started_at,
                       int64_t* id, std::string* error) {
  *id = 0;
  auto stmt = db_->Prepare(
      "INSERT INTO downloads (url, path, filename, state, started_at) "
      "VALUES (?, ?, ?, ?, ?)");
  if (!stmt) return false;
  stmt->Bind(1, url).Bind(2, path).Bind(3, filename)
      .Bind(4, static_cast<int64_t>(kDownloadActive)).Bind(5, started_at);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  *id = db_->LastInsertId();
  return true;
}

bool Downloads::UpdateProgress(int64_t id, int state, int64_t received,
                               int64_t total, std::string* error) {
  auto stmt = db_->Prepare(
      "UPDATE downloads SET state = ?, received = ?, total = ? WHERE id = ?");
  if (!stmt) return false;
  stmt->Bind(1, static_cast<int64_t>(state)).Bind(2, received)
      .Bind(3, total).Bind(4, id);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool Downloads::List(int limit, std::vector<DownloadItemRow>* out,
                     std::string* error) {
  out->clear();
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  auto stmt = db_->Prepare(
      "SELECT id, url, path, filename, state, received, total, started_at "
      "FROM downloads ORDER BY started_at DESC, id DESC LIMIT ?");
  if (!stmt) return false;
  stmt->Bind(1, static_cast<int64_t>(limit));
  while (stmt->Step()) {
    DownloadItemRow row;
    row.id = stmt->ColInt(0);
    row.url = stmt->ColText(1);
    row.path = stmt->ColText(2);
    row.filename = stmt->ColText(3);
    row.state = static_cast<int>(stmt->ColInt(4));
    row.received = stmt->ColInt(5);
    row.total = stmt->ColInt(6);
    row.started_at = stmt->ColInt(7);
    out->push_back(std::move(row));
  }
  if (!stmt->error().empty()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool Downloads::Delete(int64_t id, std::string* error) {
  auto stmt = db_->Prepare("DELETE FROM downloads WHERE id = ?");
  if (!stmt) return false;
  stmt->Bind(1, id);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return db_->Changes() > 0;
}

// ---- Settings --------------------------------------------------------------

bool Settings::GetString(const std::string& key, const std::string& def,
                         std::string* out, std::string* error) {
  auto stmt = db_->Prepare("SELECT value FROM settings WHERE key = ?");
  if (!stmt) return false;
  stmt->Bind(1, key);
  if (stmt->Step()) {
    *out = stmt->ColText(0);
    return true;
  }
  if (!stmt->error().empty()) {
    if (error) *error = stmt->error();
    return false;
  }
  *out = def;
  return true;
}

bool Settings::SetString(const std::string& key, const std::string& value,
                         std::string* error) {
  auto stmt = db_->Prepare(
      "INSERT INTO settings (key, value) VALUES (?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  if (!stmt) return false;
  stmt->Bind(1, key).Bind(2, value);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool Settings::GetBool(const std::string& key, bool def, bool* out,
                       std::string* error) {
  std::string value;
  if (!GetString(key, def ? "1" : "0", &value, error)) return false;
  *out = value == "1";
  return true;
}

bool Settings::SetBool(const std::string& key, bool value, std::string* error) {
  return SetString(key, value ? "1" : "0", error);
}

bool Settings::GetInt(const std::string& key, int def, int* out,
                      std::string* error) {
  std::string value;
  if (!GetString(key, std::to_string(def), &value, error)) return false;
  *out = std::atoi(value.c_str());
  return true;
}

bool Settings::SetInt(const std::string& key, int value, std::string* error) {
  return SetString(key, std::to_string(value), error);
}

// ---- Permissions -----------------------------------------------------------

int Permissions::Get(const std::string& host, const std::string& type,
                     std::string* error) {
  auto stmt = db_->Prepare(
      "SELECT decision FROM permissions WHERE host = ? AND type = ?");
  if (!stmt) {
    if (error) *error = "prepare failed";
    return kPermissionAsk;
  }
  stmt->Bind(1, host).Bind(2, type);
  if (stmt->Step()) return static_cast<int>(stmt->ColInt(0));
  if (error && !stmt->error().empty()) *error = stmt->error();
  return kPermissionAsk;
}

bool Permissions::Set(const std::string& host, const std::string& type,
                      int decision, std::string* error) {
  if (decision != kPermissionAsk && decision != kPermissionAllow &&
      decision != kPermissionDeny) {
    if (error) *error = "invalid decision";
    return false;
  }
  auto stmt = db_->Prepare(
      "INSERT INTO permissions (host, type, decision, updated_at) VALUES (?, ?,"
      " ?, strftime('%s','now')) ON CONFLICT(host, type) DO UPDATE SET decision"
      " = excluded.decision, updated_at = excluded.updated_at");
  if (!stmt) return false;
  stmt->Bind(1, host).Bind(2, type).Bind(3, decision);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool Permissions::List(std::vector<PermissionRow>* out, std::string* error) {
  out->clear();
  auto stmt = db_->Prepare(
      "SELECT host, type, decision, updated_at FROM permissions ORDER BY host,"
      " type");
  if (!stmt) {
    if (error) *error = "prepare failed";
    return false;
  }
  while (stmt->Step()) {
    PermissionRow row;
    row.host = stmt->ColText(0);
    row.type = stmt->ColText(1);
    row.decision = static_cast<int>(stmt->ColInt(2));
    row.updated_at = stmt->ColInt(3);
    out->push_back(std::move(row));
  }
  return true;
}

bool Permissions::Remove(const std::string& host, const std::string& type,
                         std::string* error) {
  auto stmt =
      db_->Prepare("DELETE FROM permissions WHERE host = ? AND type = ?");
  if (!stmt) return false;
  stmt->Bind(1, host).Bind(2, type);
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

bool Permissions::ClearAll(std::string* error) {
  auto stmt = db_->Prepare("DELETE FROM permissions");
  if (!stmt) return false;
  if (!stmt->Run()) {
    if (error) *error = stmt->error();
    return false;
  }
  return true;
}

}  // namespace dask::storage
