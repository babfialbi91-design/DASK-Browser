// DASK Browser - user profiles (Phase 5).
//
// Each profile owns an isolated directory under
// %LOCALAPPDATA%\DaskBrowser\profiles\<id>\ holding its dask.db,
// session.json, privacy.json and CEF cache. Profiles are deliberately
// filesystem-separated (requirement: separate paths).
#pragma once

#include <string>
#include <vector>

namespace dask {

struct ProfileInfo {
  std::string id;    // directory-safe id ("default", "work", ...)
  std::string name;  // display name
};

class ProfileStore {
 public:
  explicit ProfileStore(const std::wstring& base_dir);

  // Loads profiles.json; migrates legacy root-level data into "default"
  // on first run (old dask.db/session.json/privacy.json move into
  // profiles/default/). Creates "default" when missing.
  void Load();

  const std::vector<ProfileInfo>& profiles() const { return profiles_; }
  const std::string& active_id() const { return active_id_; }

  bool CreateProfile(const std::string& name, std::string* out_id,
                     std::string* error);
  // Returns false when the profile does not exist.
  bool SwitchProfile(const std::string& id, std::string* error);
  // Never deletes the active profile or the last remaining one.
  bool DeleteProfile(const std::string& id, std::string* error);

  // Absolute paths of the ACTIVE profile's files/directories.
  std::wstring profile_dir() const { return active_dir_; }
  std::wstring db_path() const { return active_dir_ + L"\\dask.db"; }
  std::wstring session_path() const { return active_dir_ + L"\\session.json"; }
  std::wstring privacy_path() const { return active_dir_ + L"\\privacy.json"; }
  std::wstring cef_cache_dir() const { return active_dir_ + L"\\cef_cache"; }

 private:
  void Save();
  std::wstring DirFor(const std::string& id) const;

  std::wstring base_dir_;
  std::wstring active_dir_;  // DirFor(active_id_), kept in sync
  std::vector<ProfileInfo> profiles_;
  std::string active_id_;
};

}  // namespace dask
