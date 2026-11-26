// DASK Browser - UI scheme handler (http://dask.ui interception).
#include "dask_scheme.h"

#include <shlwapi.h>

#include <cstdio>
#include <string>

#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/cef_response.h"
#include "include/wrapper/cef_stream_resource_handler.h"

namespace dask {
namespace {

std::string MimeFromExtension(const std::wstring& ext) {
  if (ext == L".html" || ext == L".htm") return "text/html";
  if (ext == L".js" || ext == L".mjs") return "text/javascript";
  if (ext == L".css") return "text/css";
  if (ext == L".svg") return "image/svg+xml";
  if (ext == L".png") return "image/png";
  if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
  if (ext == L".webp") return "image/webp";
  if (ext == L".ico") return "image/x-icon";
  if (ext == L".woff2") return "font/woff2";
  if (ext == L".woff") return "font/woff";
  if (ext == L".json") return "application/json";
  return "application/octet-stream";
}

// Handler that answers 404 with a small text body.
CefRefPtr<CefResourceHandler> NotFoundHandler() {
  static const std::string body = "not found";
  CefResponse::HeaderMap headers;
  headers.insert(std::make_pair(CefString("Content-Type"),
                                CefString("text/plain; charset=utf-8")));
  auto stream = CefStreamReader::CreateForData(
      const_cast<unsigned char*>(
          reinterpret_cast<const unsigned char*>(body.data())),
      body.size());
  return new CefStreamResourceHandler(404, "Not Found",
                                      "text/plain; charset=utf-8", headers,
                                      stream);
}

class UiSchemeFactory : public CefSchemeHandlerFactory {
 public:
  explicit UiSchemeFactory(std::wstring base_dir)
      : base_dir_(std::move(base_dir)) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;

    const std::wstring full_url = request->GetURL().ToWString();
    if (full_url.rfind(L"http://dask.ui/", 0) != 0) return NotFoundHandler();

    // Keep only the path part, strip query/fragment.
    std::wstring rest = full_url.substr(std::size(L"http://dask.ui/") - 1);
    const size_t qmark = rest.find_first_of(L"?#");
    if (qmark != std::wstring::npos) rest.resize(qmark);

    // URL-decode the path.
    const std::string decoded = CefURIDecode(
        CefString(rest).ToString(), true,
        cef_uri_unescape_rule_t(UU_NORMAL | UU_SPACES));
    const std::wstring path = CefString(decoded).ToWString();

    if (path.empty()) return NotFoundHandler();

    // Reject traversal tricks and normalize into path segments.
    std::vector<std::wstring> parts;
    {
      std::wstring cur;
      for (const wchar_t ch : path) {
        if (ch == L'/') {
          if (cur == L".." || cur == L".") return NotFoundHandler();
          if (!cur.empty()) parts.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(ch);
        }
      }
      if (cur == L".." || cur == L".") return NotFoundHandler();
      if (!cur.empty()) parts.push_back(cur);
    }
    if (parts.empty()) parts.push_back(L"index.html");  // http://dask.ui/ root.

    std::wstring rel;
    for (const auto& part : parts) {
      rel += L"\\";
      rel += part;
    }
    std::wstring file_path = base_dir_ + rel;

    // Belt and braces: verify the resolved file stays inside base_dir_.
    wchar_t resolved[MAX_PATH] = {};
    if (!::PathCanonicalizeW(resolved, file_path.c_str()))
      return NotFoundHandler();
    file_path = resolved;
    if (file_path.rfind(base_dir_ + L"\\", 0) != 0) return NotFoundHandler();

    DWORD attrs = ::GetFileAttributesW(file_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
      return NotFoundHandler();

    std::wstring ext = ::PathFindExtensionW(file_path.c_str());
    ::CharLowerW(ext.data());

    auto stream = CefStreamReader::CreateForFile(file_path);
    if (!stream) {
      return NotFoundHandler();
    }
    const std::string mime = MimeFromExtension(ext);
    CefResponse::HeaderMap headers;
    headers.insert(std::make_pair(CefString("Content-Type"), CefString(mime)));
    return new CefStreamResourceHandler(200, "OK", mime, headers, stream);
  }

 private:
  IMPLEMENT_REFCOUNTING(UiSchemeFactory);
  DISALLOW_COPY_AND_ASSIGN(UiSchemeFactory);

  std::wstring base_dir_;
};

}  // namespace

void InstallUiSchemeHandler(const std::wstring& ui_dir) {
  // Intercept the reserved host on the standard http scheme. Requests to any
  // other host fall through to the default network handling.
  CefRegisterSchemeHandlerFactory("http", kUiHost,
                                  new UiSchemeFactory(ui_dir));
}

}  // namespace dask
