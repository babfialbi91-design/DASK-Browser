; Inno Setup script for DASK Browser.
; Packages the Release output (build/bin) without the test binaries,
; installs to %LOCALAPPDATA%\Programs\DaskBrowser (per-user, no admin),
; creates a desktop icon that opens the browser right away, and Start
; Menu / uninstall entries.

#define AppName "DASK Browser"
#define AppExe "dask_browser.exe"
#define AppVersion GetVersionNumbersString(AddBackslash(SourcePath) + "..\build\bin\" + AppExe)

[Setup]
AppId={{8E5B6C4E-9D2A-4B41-A3C3-DASK00000001}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=DASK
DefaultDirName={autopf}\DaskBrowser
; per-user install — no admin rights required
PrivilegesRequired=lowest
DefaultGroupName=DASK Browser
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=..\build\installer
OutputBaseFilename=DaskBrowser-Setup-v{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; the browser auto-updates its own filter lists; running app files change
CloseApplications=no
RestartApplications=no

[Files]
Source: "..\build\bin\dask_browser.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\dask_browser_subprocess.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\libcef.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\chrome_elf.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\vk_swiftshader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\vk_swiftshader_icd.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\vulkan-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\d3dcompiler_47.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\dxcompiler.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\icudtl.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\resources.pak"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\chrome_100_percent.pak"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\chrome_200_percent.pak"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\v8_context_snapshot.bin"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\locales\*"; DestDir: "{app}\locales"; Flags: ignoreversion recursesubdirs
Source: "..\build\bin\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs

[Icons]
; Desktop icon opens the browser immediately.
Name: "{autodesktop}\DASK Browser"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon
Name: "{group}\DASK Browser"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall DASK Browser"; Filename: "{uninstallexe}"

[Tasks]
Name: "desktopicon"; Description: "إنشاء أيقونة على سطح المكتب"; GroupDescription: "إضافات:"

[Run]
Filename: "{app}\{#AppExe}"; Description: "تشغيل DASK Browser الآن"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; user data lives in %LOCALAPPDATA%\DaskBrowser and is kept on uninstall
Type: files; Name: "{app}\debug.log"
