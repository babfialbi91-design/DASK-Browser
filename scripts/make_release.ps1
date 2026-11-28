# DASK Browser - Release packaging (Phase 6).
# Builds a portable ZIP from the Release output. CEF files and the UI bundle
# are included as unmodified upstream/our-built artifacts (see
# THIRD_PARTY_NOTICES.md for their licenses).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts/make_release.ps1
# Output: dist\DASK-Browser-<version>-portable.zip
param()
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin"
$stage = Join-Path $root "dist\stage"
$version = "1.2.0"

if (-not (Test-Path (Join-Path $bin "dask_browser.exe"))) {
    Write-Error "build\bin\dask_browser.exe missing - build Release first"
}

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Application files
Copy-Item (Join-Path $bin "dask_browser.exe") $stage
Copy-Item (Join-Path $bin "dask_browser_subprocess.exe") $stage

# CEF runtime (unmodified upstream)
foreach ($f in @("chrome_elf.dll","libcef.dll","d3dcompiler_47.dll","dxcompiler.dll","vk_swiftshader.dll","vulkan-1.dll","icudtl.dat","resources.pak","chrome_100_percent.pak","chrome_200_percent.pak")) {
    Copy-Item (Join-Path $bin $f) $stage -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $bin "locales") $stage -Recurse
Copy-Item (Join-Path $bin "v8_context_snapshot.bin") $stage

# UI bundle + local resources
Copy-Item (Join-Path $bin "ui") $stage -Recurse
Copy-Item (Join-Path $bin "test_page.html") $stage -ErrorAction SilentlyContinue

# Docs
foreach ($d in @("README.md","CHANGELOG.md","SECURITY.md","THIRD_PARTY_NOTICES.md","INSTALL.md")) {
    Copy-Item (Join-Path $root $d) $stage -ErrorAction SilentlyContinue
}

$zip = Join-Path $root ("dist\DASK-Browser-" + $version + "-portable.zip")
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host ("release zip: " + $zip)
