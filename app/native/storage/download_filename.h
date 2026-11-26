// DASK Browser - download filename/path validation (Phase 4).
// Pure logic, unit-tested: builds a safe download target path from a
// suggested name and the user's Downloads folder.
#pragma once

#include <string>

namespace dask::storage {

// Sanitizes a suggested download filename:
//  * keeps only the basename (path separators cannot smuggle a location),
//  * replaces characters Windows forbids in names (< > : " / \ | ? *) and
//    control characters with '_',
//  * blocks reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9),
//  * strips trailing dots/spaces,
//  * caps the length at 200 chars while keeping the extension,
//  * falls back to "download" when the result is empty.
std::string SanitizeDownloadFilename(const std::string& suggested_utf8);

// Returns a path in |download_dir_wide| that does not collide with an
// existing file: appends " (n)" before the extension when needed.
// |filename_utf8| is assumed already sanitized.
std::wstring UniqueDownloadPath(const std::wstring& download_dir_wide,
                                const std::string& filename_utf8);

}  // namespace dask::storage
