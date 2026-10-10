; SANAD.iss — Windows-only (x64) installer for the SANAD Engine.
;
; Built by packaging/windows/build_installer.ps1 locally and by
; .github/workflows/release.yml on a v* tag. Both pass:
;   /DAppVersion=0.2.0  (full string, shown in Add/Remove Programs)
;   /DVersionInfo=0.2.0.0      (numeric x.y.z.b for the Setup.exe properties)
;   /DStageDir=<absolute staged folder produced by build_installer.ps1>
;   /DOutputDir=<absolute dir for the Setup.exe>
;   /DRepoDir=<absolute repo root, for the .ico>
; Defaults below let `iscc SANAD.iss` still work from this folder.
;
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef VersionInfo
  #define VersionInfo "0.2.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\\..\\dist\\stage\\SANAD-0.2.0"
#endif
#ifndef OutputDir
  #define OutputDir "..\\..\\dist"
#endif
#ifndef RepoDir
  #define RepoDir "..\\.."
#endif

#define AppName "SANAD Engine"
#define AppExe "SANADEditor.exe"
#define AppId "{{B5A7F3E2-8C41-4D9E-9F2A-6E1C4D8B7A03}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
VersionInfoVersion={#VersionInfo}
VersionInfoDescription=SANAD Engine installer (Windows x64)
AppPublisher=SANAD
AppPublisherURL=https://github.com/abdallah2183/SANAD-Engine
AppSupportURL=https://github.com/abdallah2183/SANAD-Engine/issues
AppUpdatesURL=https://github.com/abdallah2183/SANAD-Engine/releases
DefaultDirName={autopf}\SANAD Engine
DefaultGroupName=SANAD Engine
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir={#OutputDir}
OutputBaseFilename=SANAD-{#AppVersion}-Windows-x64-Setup
SetupIconFile={#RepoDir}\Editor\resources\SANAD.ico
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVersion}
DisableDirPage=no
DisableProgramGroupPage=yes
ShowLanguageDialog=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "starticon"; Description: "Start Menu entry"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SANAD Editor"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExe}"; Tasks: starticon
Name: "{autodesktop}\SANAD Editor"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,SANAD Editor}"; Flags: nowait postinstall skipifsilent

[Code]
// The Vulkan loader (vulkan-1.dll) ships with the GPU driver, never with the
// app — bundling a stale loader is how you get a black screen on a newer
// driver. build_installer.ps1 documents the same decision. The editor already
// reports a missing loader plainly at startup; this only warns earlier, at
// install time, and never blocks the install.
function VulkanLoaderPresent(): Boolean;
var
  Sys64, Sys32: String;
begin
  Sys64 := ExpandConstant('{sys}');
  // {syswow64} exists only on 64-bit Windows; ExpandConstant leaves it
  // literal elsewhere, so guard with IsWin64 before touching it.
  Result := FileExists(Sys64 + '\vulkan-1.dll');
  if (not Result) and IsWin64 then
  begin
    Sys32 := ExpandConstant('{syswow64}');
    if Pos('{', Sys32) = 0 then
      Result := FileExists(Sys32 + '\vulkan-1.dll');
  end;
end;

function InitializeSetup(): Boolean;
var
  Msg: String;
begin
  Result := True;
  if not VulkanLoaderPresent() then
  begin
    if ActiveLanguage() = 'ar' then
      Msg := 'لم يتم العثور على Vulkan loader (vulkan-1.dll) على هذا الجهاز.' + #13#10 +
             'المحرر يحتاج تعريف GPU يدعم Vulkan. التثبيت سيكمل، لكن المحرر سيخبرك عند التشغيل.'
    else
      Msg := 'No Vulkan loader (vulkan-1.dll) was found on this machine.' + #13#10 +
             'The editor needs a Vulkan-capable GPU driver. Setup will continue, ' +
             'and the editor will tell you at startup if the loader is still missing.';
    MsgBox(Msg, mbInformation, MB_OK);
  end;
end;
