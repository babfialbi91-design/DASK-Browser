# Downloads a pinned CEF binary distribution for Windows x64 into
# third_party/cef and writes scripts/cef_root.txt for CMake.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/download_cef.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/download_cef.ps1 -Version <other-version>
#
# The distribution is large (~180 MB) and is never committed to Git.

param(
    [string]$Version = "158.0.6+g23f1736+chromium-158.0.8127.89",
    [string]$Platform = "windows64",
    [string]$Flavor = "minimal"
)

$ErrorActionPreference = "Stop"

$rootDir = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $rootDir "third_party"
$cefDir = Join-Path $thirdParty "cef"
$archivePath = Join-Path $env:TEMP "cef_minimal.tar.bz2"

$fileName = "cef_binary_${Version}_${Platform}_${Flavor}.tar.bz2"
$downloadUrl = "https://cef-builds.spotifycdn.com/$([uri]::EscapeDataString($fileName))"

# Known SHA1 from cef-builds.spotifycdn.com (verify before extracting).
$sha1 = "2abb78fcd3b07ea0fb2014e3867a0902f19f1731"

Write-Host "CEF version : $Version"
Write-Host "URL         : $downloadUrl"

if (Test-Path (Join-Path $cefDir "include")) {
    Write-Host "CEF already present at $cefDir - nothing to do." -ForegroundColor Green
    exit 0
}

Write-Host "Downloading (about 180 MB)..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -UseBasicParsing

Write-Host "Verifying SHA1..."
$actual = (Get-FileHash -Path $archivePath -Algorithm SHA1).Hash.ToLower()
if ($actual -ne $sha1) {
    Remove-Item $archivePath -ErrorAction SilentlyContinue
    throw "SHA1 mismatch: expected $sha1, got $actual. Download aborted."
}
Write-Host "SHA1 OK."

Write-Host "Extracting..."
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
# Windows 10/11 bsdtar understands .tar.bz2 directly.
tar -xf $archivePath -C $thirdParty
if ($LASTEXITCODE -ne 0) { throw "tar extraction failed with code $LASTEXITCODE." }

$extracted = Get-ChildItem -Path $thirdParty -Directory |
    Where-Object { $_.Name -like "cef_binary_${Version}_${Platform}*" } |
    Select-Object -First 1
if (-not $extracted) { throw "Extracted CEF directory not found under $thirdParty." }

if (Test-Path $cefDir) { Remove-Item $cefDir -Recurse -Force }
Move-Item $extracted.FullName $cefDir

# Record the path for CMake (CMakeLists.txt reads scripts/cef_root.txt).
$rootLine = Join-Path $rootDir "scripts/cef_root.txt"
[IO.File]::WriteAllText($rootLine, $cefDir)

Write-Host ""
Write-Host "CEF installed at: $cefDir" -ForegroundColor Green
Write-Host "Now run:  cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64"
