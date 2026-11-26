// DASK Browser - filter list storage and updates implementation.
#include "filter_lists.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/cef_values.h"

#pragma comment(lib, "winhttp.lib")

namespace dask {
namespace {

constexpr wchar_t kFiltersDir[] = L"filters";
constexpr wchar_t kMetaFile[] = L"meta.json";
constexpr size_t kMaxListBytes = 32 * 1024 * 1024;

// CefTask adapter that runs a plain callable on the target thread.
class FnTask : public CefTask {
 public:
  explicit FnTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    auto run = std::move(fn_);
    run();
  }

 private:
  IMPLEMENT_REFCOUNTING(FnTask);

  std::function<void()> fn_;
};

std::wstring NowTimestamp() {
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t buf[64];
  swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute);
  return buf;
}

// Minimal blocking HTTPS GET via WinHTTP. Returns false with |error| set.
bool HttpsDownload(const std::wstring& url, std::string* out,
                   std::wstring* error) {
  out->clear();
  // Parse scheme://host/path
  const size_t scheme_end = url.find(L"://");
  if (scheme_end == std::wstring::npos) {
    *error = L"bad url";
    return false;
  }
  const std::wstring rest = url.substr(scheme_end + 3);
  const size_t slash = rest.find(L'/');
  const std::wstring host = rest.substr(0, slash == std::wstring::npos ? rest.size() : slash);
  const std::wstring path =
      slash == std::wstring::npos ? L"/" : rest.substr(slash);

  HINTERNET session = ::WinHttpOpen(L"DASK Browser/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = L"WinHttpOpen failed";
    return false;
  }
  bool ok = false;
  HINTERNET connect = nullptr, request = nullptr;
  do {
    ::WinHttpSetTimeouts(session, 15000, 15000, 15000, 60000);
    connect = ::WinHttpConnect(session, host.c_str(),
                               INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
      *error = L"WinHttpConnect failed";
      break;
    }
    request = ::WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!request) {
      *error = L"WinHttpOpenRequest failed";
      break;
    }
    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      *error = L"WinHttpSendRequest failed";
      break;
    }
    if (!::WinHttpReceiveResponse(request, nullptr)) {
      *error = L"WinHttpReceiveResponse failed";
      break;
    }
    DWORD status = 0, size = sizeof(status);
    if (!::WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_STATUS_CODE |
                                   WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                               WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
      *error = L"HTTP status not 200";
      break;
    }
    size_t total = 0;
    char chunk[16384];
    DWORD read = 0;
    while (::WinHttpReadData(request, chunk, sizeof(chunk), &read) && read > 0) {
      total += read;
      if (total > kMaxListBytes) {
        *error = L"list too large";
        break;
      }
      out->append(chunk, read);
    }
    ok = !out->empty();
    if (!ok) *error = L"empty response";
  } while (false);
  if (request) ::WinHttpCloseHandle(request);
  if (connect) ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return ok;
}

// Atomic file write: temp file in the same directory + MoveFileEx replace.
bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
  const std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f.good()) {
      f.close();
      ::DeleteFileW(tmp.c_str());
      return false;
    }
  }
  if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

std::string ReadFileString(const std::wstring& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return std::string();
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

const FilterLists::Source FilterLists::kSources[] = {
    {L"easylist", L"https://easylist.to/easylist/easylist.txt"},
    {L"easyprivacy", L"https://easylist.to/easylist/easyprivacy.txt"},
};

void FilterLists::Initialize(const std::wstring& appdata_dir) {
  dir_ = appdata_dir + L"\\" + kFiltersDir;
  ::CreateDirectoryW(dir_.c_str(), nullptr);

  // Load persisted metadata.
  const std::string meta = ReadFileString(dir_ + L"\\" + kMetaFile);
  if (!meta.empty()) {
    CefRefPtr<CefValue> parsed =
        CefParseJSON(meta, cef_json_parser_options_t(0));
    if (parsed && parsed->GetType() == VTYPE_DICTIONARY) {
      CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
      std::lock_guard<std::mutex> lock(status_mutex_);
      statuses_.clear();
      for (const Source& src : kSources) {
        FilterListStatus st;
        st.name = src.name;
        st.url = src.url;
        CefRefPtr<CefDictionaryValue> item = dict->GetDictionary(src.name);
        if (item) {
          st.rules = static_cast<size_t>(item->GetInt("rules"));
          st.updated = item->GetString("updated").ToWString();
          st.status = item->GetString("status").ToString();
        } else {
          st.status = "not loaded";
        }
        statuses_.push_back(st);
      }
    }
  }
  if (statuses_.empty()) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    statuses_.clear();
    for (const Source& src : kSources) {
      FilterListStatus st;
      st.name = src.name;
      st.url = src.url;
      st.status = "not loaded";
      statuses_.push_back(st);
    }
  }

  RebuildEngine();
}

void FilterLists::RebuildEngine() {
  // Parse every stored list into one engine; an empty engine (all files
  // missing) still allows the app to run with blocking inactive.
  std::string combined;
  for (const Source& src : kSources) {
    const std::string text =
        ReadFileString(dir_ + L"\\" + std::wstring(src.name) + L".txt");
    if (!text.empty()) {
      combined += text;
      combined += '\n';
      // Update the per-list accepted-rule count.
      adblock::ParseReport rep;
      adblock::FilterEngine::Parse(text, &rep);
      std::lock_guard<std::mutex> lock(status_mutex_);
      for (FilterListStatus& st : statuses_) {
        if (st.name == src.name) {
          st.rules = rep.accepted;
          if (st.status != "loading" && st.updated.empty()) {
            st.updated = L"";  // unknown timestamp when loaded from disk
          }
        }
      }
    } else {
      std::lock_guard<std::mutex> lock(status_mutex_);
      for (FilterListStatus& st : statuses_) {
        if (st.name == src.name) {
          st.rules = 0;
          if (st.status == "ok") st.status = "error: stored file missing";
        }
      }
    }
  }
  auto parsed = adblock::FilterEngine::Parse(combined);
  engine_.store(std::shared_ptr<const adblock::FilterEngine>(parsed.release()));
  PersistMeta();
}

void FilterLists::PersistMeta() {
  CefRefPtr<CefDictionaryValue> root_dict = CefDictionaryValue::Create();
  std::vector<FilterListStatus> snapshot;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    snapshot = statuses_;
  }
  for (const FilterListStatus& st : snapshot) {
    CefRefPtr<CefDictionaryValue> item = CefDictionaryValue::Create();
    item->SetInt("rules", static_cast<int>(st.rules));
    item->SetString("updated", st.updated);
    item->SetString("status", st.status);
    root_dict->SetDictionary(st.name, item);
  }
  CefRefPtr<CefValue> root = CefValue::Create();
  root->SetDictionary(root_dict);
  const std::string json =
      CefWriteJSON(root, cef_json_writer_options_t(0)).ToString();
  WriteFileAtomic(dir_ + L"\\" + kMetaFile, json);
}

std::vector<FilterListStatus> FilterLists::Statuses() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return statuses_;
}

void FilterLists::UpdateAll(std::function<void()> on_done) {
  bool expected = false;
  if (!updating_.compare_exchange_strong(expected, true)) {
    if (on_done) {
      CefPostTask(TID_UI, new FnTask(std::move(on_done)));
    }
    return;  // an update is already running
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    for (FilterListStatus& st : statuses_) st.status = "loading";
  }
  PersistMeta();

  std::thread([this, on_done = std::move(on_done)]() {
    for (const Source& src : kSources) {
      std::string data;
      std::wstring error;
      bool downloaded = HttpsDownload(src.url, &data, &error);
      if (!downloaded) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        for (FilterListStatus& st : statuses_) {
          if (st.name == src.name) {
            st.status = "error: " + CefString(error).ToString();
          }
        }
        continue;  // keep the previously stored version
      }
      if (!adblock::FilterEngine::LooksLikeFilterList(data, nullptr)) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        for (FilterListStatus& st : statuses_) {
          if (st.name == src.name) {
            st.status = "error: content failed validation";
          }
        }
        continue;  // do not accept corrupted or wrong content
      }
      const std::wstring path = dir_ + L"\\" + std::wstring(src.name) + L".txt";
      if (!WriteFileAtomic(path, data)) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        for (FilterListStatus& st : statuses_) {
          if (st.name == src.name) {
            st.status = "error: failed to store file";
          }
        }
        continue;
      }
      std::lock_guard<std::mutex> lock(status_mutex_);
      for (FilterListStatus& st : statuses_) {
        if (st.name == src.name) {
          st.status = "ok";
          st.updated = NowTimestamp();
        }
      }
    }
    RebuildEngine();  // parse + atomic engine swap
    updating_.store(false);
    if (on_done) {
      CefPostTask(TID_UI, new FnTask(std::move(on_done)));
    }
  }).detach();
}

}  // namespace dask
