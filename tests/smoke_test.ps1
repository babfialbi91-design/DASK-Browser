# DASK Browser smoke test (Phase 1 foundation).
# Verifies toolchain, CEF presence, configure, build and runtime artifacts.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tests/smoke_test.ps1
#   powershell -ExecutionPolicy Bypass -File tests/smoke_test.ps1 -SkipBuild   # artifacts check only
#
# Exits 0 on success, 1 on failure.

param([switch]$SkipBuild)

$ErrorActionPreference = "Continue"
$rootDir = Split-Path -Parent $PSScriptRoot
$failures = @()

function Fail($msg) {
    Write-Host "FAIL  $msg" -ForegroundColor Red
    $script:failures += $msg
}
function Pass($msg) { Write-Host "PASS  $msg" -ForegroundColor Green }

Write-Host "=== DASK Browser smoke test ===" -ForegroundColor Cyan

# 1. Toolchain
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { Pass "cmake found: $((cmake --version | Select-Object -First 1))" }
else { Fail "cmake not on PATH" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if ($vs) { Pass "MSVC C++ toolset found: $vs" }
    else { Fail "Visual Studio with C++ workload (VCTools) not found" }
} else { Fail "vswhere.exe not found (VS Build Tools not installed?)" }

# 2. CEF distribution
$cefRoot = $null
if ($env:CEF_ROOT) { $cefRoot = $env:CEF_ROOT }
elseif (Test-Path (Join-Path $rootDir "scripts\cef_root.txt")) {
    $cefRoot = (Get-Content (Join-Path $rootDir "scripts\cef_root.txt") -Raw).Trim()
}
if ($cefRoot -and (Test-Path (Join-Path $cefRoot "include\cef_app.h"))) {
    Pass "CEF distribution found at $cefRoot"
} else {
    Fail "CEF distribution not found - run scripts/download_cef.ps1"
}

# 3. Build
$buildDir = Join-Path $rootDir "build"
if (-not $SkipBuild -and $cmake -and $cefRoot) {
    Write-Host "--- cmake configure ---"
    & cmake -S $rootDir -B $buildDir -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed" }

    Write-Host "--- cmake build (Release) ---"
    & cmake --build $buildDir --config Release --parallel
    $firstExit = $LASTEXITCODE
    if ($firstExit -ne 0) {
        # Transient file-lock failures (antivirus / previous instance holding
        # chrome_elf.dll) are common on Windows: retry once before failing.
        Write-Host "build returned $firstExit - retrying once (possible transient file lock)"
        Start-Sleep -Seconds 5
        & cmake --build $buildDir --config Release --parallel
    }
    if ($LASTEXITCODE -ne 0) { Fail "cmake build failed" }
}

# 4. Runtime artifacts
$binDir = Join-Path $buildDir "bin"
$required = @(
    "dask_browser.exe",
    "dask_browser_subprocess.exe",
    "libcef.dll",
    "icudtl.dat",
    "resources.pak",
    "v8_context_snapshot.bin",
    "test_page.html"
)
if (Test-Path $binDir) {
    foreach ($f in $required) {
        if (Test-Path (Join-Path $binDir $f)) { Pass "artifact: $f" }
        else { Fail "missing artifact: $f (expected in $binDir)" }
    }
} else {
    Fail "build output directory not found: $binDir"
}

# 5. UI bundle (React dist copied next to the exe)
$uiDir = Join-Path $binDir "ui"
if (Test-Path (Join-Path $uiDir "index.html")) { Pass "ui bundle: index.html" }
else { Fail "missing ui bundle file: ui/index.html (run npm run build in app/ui)" }
if (Test-Path (Join-Path $uiDir "newtab.html")) { Pass "ui bundle: newtab.html" }
else { Fail "missing ui bundle file: ui/newtab.html" }
if (Test-Path (Join-Path $uiDir "assets")) {
    Pass "ui bundle: assets/ directory"
} else { Fail "missing ui bundle: assets/ directory" }
if (Test-Path (Join-Path $uiDir "settings.html")) { Pass "ui bundle: settings.html" }
else { Fail "missing ui bundle file: ui/settings.html" }

# 6. Filter engine unit tests (Phase 3)
$testExe = Join-Path $binDir "dask_filter_tests.exe"
if (Test-Path $testExe) {
    & $testExe
    if ($LASTEXITCODE -eq 0) { Pass "filter engine tests" }
    else { Fail "filter engine tests failed" }
} else { Fail "missing dask_filter_tests.exe (build target dask_filter_tests)" }

# 7. Storage layer unit tests (Phase 4)
$storageExe = Join-Path $binDir "dask_storage_tests.exe"
if (Test-Path $storageExe) {
    & $storageExe
    if ($LASTEXITCODE -eq 0) { Pass "storage layer tests" }
    else { Fail "storage layer tests failed" }
} else { Fail "missing dask_storage_tests.exe (build target dask_storage_tests)" }

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "SMOKE TEST PASSED" -ForegroundColor Green
    exit 0
}
Write-Host "SMOKE TEST FAILED ($($failures.Count) failures)" -ForegroundColor Red
exit 1
