// DASK Browser - user profiles implementation (Phase 5).
#include "profiles.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "storage/database.h"

namespace dask {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string WideToUtf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(),
                             n, nullptr, nullptr);
  return out;
}

// JSON escaping for names (quotes, backslash, control chars).
std::string JsonEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20) continue;
    out.push_back(c);
  }
  return out;
}

// Directory-safe profile id: latin letters/digits/dash only, lowercase,
// max 32 chars; anything else maps to "_".
std::string SanitizeProfileId(const std::string& name) {
  std::string id;
  for (char c : name) {
    if (isalnum(static_cast<unsigned char>(c)))
      id.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    else if (c == '-')
      id.push_back('-');
    else
      id.push_back('_');
    if (id.size() >= 32) break;
  }
  // Non-latin names (e.g. Arabic) collapse to underscores: fall back to a
  // neutral "profile-N" id; the display name stays unchanged.
  const bool all_underscore = !id.empty() &&
      id.find_first_not_of('_') == std::string::npos;
  if (id.empty() || all_underscore) return "profile";
  return id;
}

}  // namespace

ProfileStore::ProfileStore(const std::wstring& base_dir) : base_dir_(base_dir) {}

std::wstring ProfileStore::DirFor(const std::string& id) const {
  return base_dir_ + L"\\profiles\\" + Utf8ToWide(id);
}

void ProfileStore::Load() {
  const std::wstring path = base_dir_ + L"\\profiles.json";
  std::ifstream in(path, std::ios::binary);
  bool loaded = false;
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    // Minimal flat parse: {"active":"x","profiles":[{"id":"a","name":"b"},...]}
    auto find_str = [&json](const std::string& key, size_t from,
                            std::string* out) {
      const std::string needle = "\"" + key + "\":";
      size_t p = json.find(needle, from);
      if (p == std::string::npos) return std::string::npos;
      p += needle.size();
      while (p < json.size() && isspace(static_cast<unsigned char>(json[p])))
        ++p;
      if (p >= json.size() || json[p] != '"') return std::string::npos;
      ++p;
      while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) ++p;
        out->push_back(json[p]);
        ++p;
      }
      return p;
    };
    size_t pos = find_str("active", 0, &active_id_);
    if (!active_id_.empty()) {
      size_t scan = json.find("\"profiles\":");
      while (scan != std::string::npos) {
        size_t obj = json.find('{', scan);
        if (obj == std::string::npos) break;
        size_t end = json.find('}', obj);
        if (end == std::string::npos) break;
        scan = end + 1;
        // Extract id/name within [obj, end]
        std::string id, name;
        for (const char* key : {"id", "name"}) {
          const std::string needle = std::string("\"") + key + "\":";
          size_t p = json.find(needle, obj);
          if (p == std::string::npos || p > end) continue;
          p += needle.size();
          while (p < end && isspace(static_cast<unsigned char>(json[p]))) ++p;
          if (p < end && json[p] == '"') {
            std::string val;
            ++p;
            while (p < end && json[p] != '"') {
              if (json[p] == '\\' && p + 1 < end) ++p;
              val.push_back(json[p]);
              ++p;
            }
            if (std::string(key) == "id") id = val;
            else name = val;
          }
        }
        if (!id.empty()) profiles_.push_back({id, name.empty() ? id : name});
      }
      loaded = !profiles_.empty();
    }
    (void)pos;
  }
  if (!loaded) {
    profiles_.clear();
    active_id_.clear();
  }

  bool has_active = false;
  for (const ProfileInfo& p : profiles_) {
    if (p.id == active_id_) has_active = true;
  }
  if (!has_active) active_id_ = profiles_.empty() ? "default" : profiles_[0].id;

  // Ensure the active profile directory exists.
  CreateDirectoryW(base_dir_.c_str(), nullptr);
  CreateDirectoryW((base_dir_ + L"\\profiles").c_str(), nullptr);
  active_dir_ = DirFor(active_id_);
  CreateDirectoryW(active_dir_.c_str(), nullptr);

  // Legacy migration: on first run with an old root-level data layout, move
  // dask.db/session.json/privacy.json into the default profile so nothing
  // is lost.
  const bool is_default_active = active_id_ == "default";
  bool has_default =
      std::any_of(profiles_.begin(), profiles_.end(),
                  [](const ProfileInfo& p) { return p.id == "default"; });
  if (is_default_active && !has_default) {
    profiles_.insert(profiles_.begin(), {"default", "افتراضي"});
    has_default = true;
  }
  if (is_default_active) {
    const wchar_t* legacy[] = {L"dask.db", L"session.json", L"privacy.json"};
    for (const wchar_t* f : legacy) {
      std::wstring src = base_dir_ + L"\\" + f;
      std::wstring dst = DirFor("default") + L"\\" + f;
      if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
  }
  Save();
}

void ProfileStore::Save() {
  const std::wstring path = base_dir_ + L"\\profiles.json";
  std::string json = "{\"active\":\"" + JsonEscape(active_id_) +
                     "\",\"profiles\":[";
  for (size_t i = 0; i < profiles_.size(); ++i) {
    if (i) json += ",";
    json += "{\"id\":\"" + JsonEscape(profiles_[i].id) + "\",\"name\":\"" +
            JsonEscape(profiles_[i].name) + "\"}";
  }
  json += "]}";
  // Atomic write: temp + MoveFileEx (same pattern as filter lists).
  const std::wstring tmp = path + L".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out.write(json.data(), (std::streamsize)json.size());
  out.close();
  MoveFileExW(tmp.c_str(), path.c_str(),
              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool ProfileStore::CreateProfile(const std::string& name, std::string* out_id,
                                 std::string* error) {
  std::string id = SanitizeProfileId(name);
  // Uniqueness: append -2, -3 ... when taken.
  std::string base = id;
  int n = 2;
  auto taken = [&](const std::string& candidate) {
    for (const ProfileInfo& p : profiles_) {
      if (p.id == candidate) return true;
    }
    return false;
  };
  while (taken(id)) id = base + "-" + std::to_string(n++);
  if (!CreateDirectoryW(DirFor(id).c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    if (error) *error = "cannot create profile directory";
    return false;
  }
  profiles_.push_back({id, name.empty() ? id : name});
  Save();
  if (out_id) *out_id = id;
  return true;
}

bool ProfileStore::SwitchProfile(const std::string& id, std::string* error) {
  for (const ProfileInfo& p : profiles_) {
    if (p.id == id) {
      active_id_ = id;
      active_dir_ = DirFor(id);
      CreateDirectoryW(active_dir_.c_str(), nullptr);
      Save();
      return true;
    }
  }
  if (error) *error = "unknown profile";
  return false;
}

bool ProfileStore::DeleteProfile(const std::string& id, std::string* error) {
  if (id == active_id_ || profiles_.size() <= 1) {
    if (error) *error = "cannot delete active or last profile";
    return false;
  }
  for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
    if (it->id == id) {
      profiles_.erase(it);
      Save();
      // Remove the directory tree.
      const std::wstring dir = DirFor(id);
      std::wstring from = dir + L'\0';
      SHFILEOPSTRUCTW op{};
      op.hwnd = nullptr;
      op.wFunc = FO_DELETE;
      op.pFrom = from.c_str();
      op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
      ::SHFileOperationW(&op);
      return true;
    }
  }
  if (error) *error = "unknown profile";
  return false;
}

}  // namespace dask
