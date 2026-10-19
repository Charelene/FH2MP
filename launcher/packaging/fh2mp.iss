; Inno Setup script. Build: iscc /DAppVersion=1.0.0 packaging\fh2mp.iss   (after PyInstaller made dist\fh2mp-launcher)
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
[Setup]
AppId={{6F0A2C1E-5B61-4E0B-9A55-0F42D0001000}
AppName=FH2MP Launcher
AppVersion={#AppVersion}
AppPublisher=FH2MP
AppPublisherURL=https://forza.scuffed.af
DefaultDirName={autopf}\FH2MP Launcher
DefaultGroupName=FH2MP
UninstallDisplayIcon={app}\fh2mp-launcher.exe
OutputDir=..\dist
OutputBaseFilename=FH2MP-Launcher-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
SetupIconFile=icon.ico
WizardStyle=modern
CloseApplications=yes
RestartApplications=yes
[Files]
Source: "..\dist\fh2mp-launcher\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
[Icons]
Name: "{group}\FH2MP Launcher"; Filename: "{app}\fh2mp-launcher.exe"
Name: "{autodesktop}\FH2MP Launcher"; Filename: "{app}\fh2mp-launcher.exe"; Tasks: desktopicon
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"
[Run]
Filename: "{app}\fh2mp-launcher.exe"; Description: "Start FH2MP Launcher"; Flags: nowait postinstall skipifsilent
