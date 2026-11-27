// DASK Browser - minimal HTTPS downloader (update checker helper).
#ifndef DASK_BROWSER_HTTPDOWNLOAD_H_
#define DASK_BROWSER_HTTPDOWNLOAD_H_

#include <string>

namespace dask {

// HTTPS GET returning the body as UTF-8. Returns false on any failure.
bool DownloadToString(const std::string& url_utf8, std::string* out,
                      int timeout_ms);

}  // namespace dask

#endif  // DASK_BROWSER_HTTPDOWNLOAD_H_
