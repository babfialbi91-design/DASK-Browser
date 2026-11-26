// DASK Browser - download filename/path validation implementation.
#include "download_filename.h"

#include <windows.h>

namespace dask::storage {
namespace {

std::string ToLowerAscii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  }
  return out;
}

bool IsReservedName(const std::string& stem) {
  static const char* kReserved[] = {
      "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
      "LPT6", "LPT7", "LPT8", "LPT9",
  };
  const std::string lower = ToLowerAscii(stem);
  for (const char* name : kReserved) {
    if (lower == ToLowerAscii(name)) return true;
  }
  return false;
}

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

std::string SanitizeDownloadFilename(const std::string& suggested_utf8) {
  // Keep the basename only: the suggested name cannot pick a location.
  std::string name = suggested_utf8;
  size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name = name.substr(slash + 1);

  // Replace forbidden characters and control characters.
  std::string clean;
  clean.reserve(name.size());
  for (unsigned char c : name) {
    const bool forbidden =
        c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
        c == '|' || c == '?' || c == '*' || c < 0x20 || c == 0x7F;
    clean.push_back(forbidden ? '_' : static_cast<char>(c));
  }
  name = std::move(clean);

  // Strip trailing dots and spaces.
  while (!name.empty() &&
         (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }

  // Cap the length, keeping the extension visible.
  constexpr size_t kMaxName = 200;
  if (name.size() > kMaxName) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot <= 16) {
      name = name.substr(0, kMaxName - (name.size() - dot)) +
             name.substr(dot);
    } else {
      name.resize(kMaxName);
    }
  }

  if (name.empty()) return "download";

  // Reserved device names must not be used as the whole stem.
  const size_t dot = name.find('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  if (IsReservedName(stem)) {
    name = "_" + name;
  }
  return name;
}

std::wstring UniqueDownloadPath(const std::wstring& download_dir_wide,
                                const std::string& filename_utf8) {
  std::wstring filename = Utf8ToWide(filename_utf8);
  std::wstring stem = filename;
  std::wstring ext;
  const size_t dot = filename.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) {
    stem = filename.substr(0, dot);
    ext = filename.substr(dot);
  }
  std::wstring candidate = download_dir_wide + L"\\" + filename;
  int counter = 1;
  while (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
    candidate = download_dir_wide + L"\\" + stem + L" (" +
                std::to_wstring(counter++) + L")" + ext;
  }
  return candidate;
}

}  // namespace dask::storage
