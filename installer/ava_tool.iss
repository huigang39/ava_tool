; Inno Setup script for ava_tool.
;
; Build the app first (`make`), then compile this installer with:
;   make package            (runs ISCC for you)   -- or --
;   ISCC.exe installer\ava_tool.iss
;
; Keep MyAppVersion in sync with src/version.hpp (AVA_VERSION) and the GitHub
; release tag (v<version>). Output: dist\ava_tool_setup_<version>.exe.

#define MyAppName    "ava_tool"
#define MyAppVersion "0.2.1"
#define MyAppExeName "ava_tool.exe"
#ifndef MyAppBinDir
#define MyAppBinDir "..\bin\win"
#endif
#define MyAppPublisher "huigang39"
#define MyAppURL     "https://github.com/huigang39/ava_tool"

[Setup]
; A stable AppId ties every version together so installs upgrade in place.
AppId={{A1B2C3D4-E5F6-47A8-9B0C-1D2E3F4A5B6C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases

; Per-user install under %LOCALAPPDATA% so updates apply without UAC prompts
; (lets updater.exe run the installer silently).
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#MyAppName}
DisableProgramGroupPage=yes
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
; Only set the installer icon if it has been generated (see `make icon`).
#if FileExists(AddBackslash(SourcePath) + "..\assets\icon.ico")
SetupIconFile=..\assets\icon.ico
#endif
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ava_tool_setup_{#MyAppVersion}
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#MyAppBinDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppBinDir}\updater.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppBinDir}\JLink_x64.dll";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppBinDir}\glfw3.dll";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppBinDir}\mujoco.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyAppBinDir}\mujoco_single_joint.xml"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyAppBinDir}\mujoco_actuator.xml"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyAppBinDir}\Spice64\*"; DestDir: "{app}\Spice64"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#MyAppBinDir}\SpiceModels\*"; DestDir: "{app}\SpiceModels"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#MyAppBinDir}\MuJoCo_LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyAppBinDir}\MuJoCo_THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#MyAppBinDir}\libfftw3f-3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyAppBinDir}\sdk\*"; DestDir: "{app}\sdk"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MyAppBinDir}\config\STM32H7_LiveAttach.JLinkScript"; DestDir: "{app}\config"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";            Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";      Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Relaunch after install. No 'skipifsilent' so it also runs during the silent
; update performed by updater.exe.
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall
