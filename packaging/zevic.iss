; Inno Setup script for the Windows installer (zevic-<version>-<platform>-setup.exe, platform windows-x64 or windows-arm64).
; Build:  iscc /DAppVersion=0.4.1 /DPlatform=windows-x64 /DArch=x64compatible /DSourceDir=dist\zevic-0.4.1-windows-x64 /DOutputDir=dist\extra packaging\zevic.iss
;         (/DPlatform=windows-arm64 /DArch=arm64 for Windows on Arm)
; Silent install (winget uses this):  zevic-setup.exe /VERYSILENT /NORESTART
; The install is per-user (no administrator rights) and adds {app}\bin to the user PATH.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "."
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1D2C54-6B7E-4C1A-9E3B-5A0D7B1E4F20}
AppName=Zevic
AppVersion={#AppVersion}
AppPublisher=zevic-lang
AppPublisherURL=https://github.com/zevic-lang/zevic
DefaultDirName={localappdata}\Zevic
DefaultGroupName=Zevic
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ChangesEnvironment=yes
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=zevic-{#AppVersion}-{#Platform}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed={#Arch}
ArchitecturesInstallIn64BitMode={#Arch}
UninstallDisplayName=Zevic {#AppVersion}
WizardStyle=modern

[Files]
Source: "{#SourceDir}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\packages\*"; DestDir: "{app}\packages"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\runtime\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\VERSION"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Check: NeedsPath(ExpandConstant('{app}\bin'))

[Code]
function NeedsPath(Dir: string): Boolean;
var
  Paths: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Paths) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Lowercase(Dir) + ';', ';' + Lowercase(Paths) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Paths, Dir: string;
  P: Integer;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Paths) then exit;
  Dir := ExpandConstant('{app}\bin');
  P := Pos(';' + Lowercase(Dir), ';' + Lowercase(Paths));
  if P > 0 then
  begin
    Delete(Paths, P, Length(Dir) + 1);
    RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Paths);
  end;
end;
