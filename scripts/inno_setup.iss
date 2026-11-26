; DASK Browser - optional classic installer (Inno Setup script).
; Build: install Inno Setup 6, then: ISCC scripts\inno_setup.iss
; Output: dist\setup\DASK-Browser-Setup-1.0.0.exe

[Setup]
AppId={{7A1D2C4B-9E33-4F2A-8C6B-DASK0BROWSER}
AppName=DASK Browser
AppVersion=1.0.0
AppPublisher=DASK
DefaultDirName={autopf}\DASK Browser
DefaultGroupName=DASK Browser
UninstallDisplayIcon={app}\dask_browser.exe
OutputDir=..\dist\setup
OutputBaseFilename=DASK-Browser-Setup-1.0.0
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin

[Files]
Source: "..\dist\stage\dask_browser.exe"; DestDir: "{app}"
Source: "..\dist\stage\dask_browser_subprocess.exe"; DestDir: "{app}"
Source: "..\dist\stage\*.dll"; DestDir: "{app}"
Source: "..\dist\stage\*.pak"; DestDir: "{app}"
Source: "..\dist\stage\*.dat"; DestDir: "{app}"
Source: "..\dist\stage\*.bin"; DestDir: "{app}"
Source: "..\dist\stage\*.html"; DestDir: "{app}"
Source: "..\dist\stage\ui\*"; DestDir: "{app}\ui"; Flags: recursesubdirs
Source: "..\dist\stage\locales\*"; DestDir: "{app}\locales"; Flags: recursesubdirs
Source: "..\dist\stage\README.md"; DestDir: "{app}"
Source: "..\dist\stage\CHANGELOG.md"; DestDir: "{app}"
Source: "..\dist\stage\SECURITY.md"; DestDir: "{app}"
Source: "..\dist\stage\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"
Source: "..\dist\stage\INSTALL.md"; DestDir: "{app}"

[Icons]
Name: "{group}\DASK Browser"; Filename: "{app}\dask_browser.exe"
Name: "{autodesktop}\DASK Browser"; Filename: "{app}\dask_browser.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "إنشاء أيقونة على سطح المكتب"; GroupDescription: "إضافات:"

[Run]
Filename: "{app}\dask_browser.exe"; Description: "تشغيل DASK Browser"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; بيانات المستخدم في %LOCALAPPDATA%\DaskBrowser تبقى عمداً (اختيار المستخدم).
Type: filesandordirs; Name: "{app}"
