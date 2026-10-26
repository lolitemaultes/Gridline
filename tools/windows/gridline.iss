; Gridline installer for Windows (Inno Setup 6). Built by .github/workflows/release.yml:
;   iscc /DAppVersion=0.7.0-beta.1 /DArch=x64 /DStage=<folder> /DOutDir=<folder> tools\windows\gridline.iss
; AppVersion is the full version (names, titles); Windows' version fields take numbers only, so
; AppNumVersion is its numeric part (0.7.0), taken from AppVersion when it is not given.
; <Stage> holds gridline-app.exe, gridline.exe, assets\ and plugins\{VST3,CLAP}.
; The installer is not code signed: Windows SmartScreen asks for confirmation ("More info",
; "Run anyway").

#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef AppNumVersion
  #if Pos("-", AppVersion) > 0
    #define AppNumVersion Copy(AppVersion, 1, Pos("-", AppVersion) - 1)
  #else
    #define AppNumVersion AppVersion
  #endif
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef Stage
  #define Stage "stage"
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6E7B2B4C-6F3E-4C8D-9A1B-3D2C5E8F4A10}
AppName=Gridline
AppVersion={#AppVersion}
AppVerName=Gridline {#AppVersion}
AppPublisher=Gridline
AppPublisherURL=https://github.com/lolitemaultes/Gridline
AppSupportURL=https://github.com/lolitemaultes/Gridline/issues
VersionInfoVersion={#AppNumVersion}.0
VersionInfoProductVersion={#AppNumVersion}.0
VersionInfoTextVersion={#AppVersion}
VersionInfoProductTextVersion={#AppVersion}
VersionInfoDescription=Gridline {#AppVersion} setup
DefaultDirName={autopf}\Gridline
DefaultGroupName=Gridline
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\gridline-app.exe
OutputDir={#OutDir}
OutputBaseFilename=Gridline-{#AppVersion}-windows-{#Arch}-setup
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
ChangesAssociations=yes
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

;---- Look (J-134): Gridline's installer design, made by tools/make_installer_art.py ----
; Dark wizard (Inno Setup 6.6 or later), the art at every DPI scale (Setup picks the best match),
; the welcome page, the license as information and the setup / uninstall icon.
SetupIconFile=art\setup.ico
DisableWelcomePage=no
#if Ver >= EncodeVer(6, 6, 0)
WizardStyle=modern dark includetitlebar hidebevels
WizardImageFile=art\wizard-100.png,art\wizard-125.png,art\wizard-150.png,art\wizard-175.png,art\wizard-200.png,art\wizard-225.png,art\wizard-250.png
WizardSmallImageFile=art\wizard-small-100.png,art\wizard-small-125.png,art\wizard-small-150.png,art\wizard-small-175.png,art\wizard-small-200.png,art\wizard-small-225.png,art\wizard-small-250.png
WizardImageBackColor=$0B0B0B
#else
  #pragma message "Inno Setup 6.6 or later is needed for Gridline's installer design; building the plain wizard"
WizardStyle=modern
#endif
#if FileExists(AddBackslash(SourcePath) + "..\..\LICENSE")
InfoBeforeFile=..\..\LICENSE
#endif

[Messages]
WelcomeLabel1=Welcome to Gridline
WelcomeLabel2=Setup installs [name/ver], the free, open source music workstation, on this computer.%n%nClose other programs before you continue.
WizardInfoBefore=License
InfoBeforeLabel=Gridline is free software under the GNU General Public License, version 3.
InfoBeforeClickLabel=You do not need to accept it to use Gridline: it gives you the right to share and change Gridline. Click Next to continue.
FinishedHeadingLabel=Gridline is ready
FinishedLabelNoIcons=Gridline is installed on this computer.
FinishedLabel=Gridline is installed on this computer. Open it any time from the Start menu.
ClickFinish=Click Finish to close Setup.
;---- end of the look ----

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "plugins"; Description: "Install the Gridline VST3 and CLAP plugins for other hosts (Common Files\VST3, Common Files\CLAP)"; GroupDescription: "Plugins:"; Flags: unchecked
Name: "assocproject"; Description: "Offer Gridline in ""Open with"" for Gridline projects (.gridline.json)"; GroupDescription: "File types:"; Flags: unchecked
Name: "assocflp"; Description: "Offer Gridline in ""Open with"" for FL Studio projects (.flp)"; GroupDescription: "File types:"; Flags: unchecked

[Files]
Source: "{#Stage}\gridline-app.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\gridline.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#Stage}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\plugins\VST3\Gridline.vst3\*"; DestDir: "{commoncf64}\VST3\Gridline.vst3"; Tasks: plugins; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\plugins\CLAP\Gridline.clap"; DestDir: "{commoncf64}\CLAP"; Tasks: plugins; Flags: ignoreversion
Source: "{#Stage}\plugins\CLAP\Gridline-assets\*"; DestDir: "{commoncf64}\CLAP\Gridline-assets"; Tasks: plugins; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Gridline"; Filename: "{app}\gridline-app.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Gridline"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Gridline"; Filename: "{app}\gridline-app.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Optional file associations (off by default).
Root: HKA; Subkey: "Software\Classes\.json\OpenWithProgids"; ValueType: string; ValueName: "Gridline.Project"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocproject
Root: HKA; Subkey: "Software\Classes\Gridline.Project"; ValueType: string; ValueName: ""; ValueData: "Gridline project"; Flags: uninsdeletekey; Tasks: assocproject
Root: HKA; Subkey: "Software\Classes\Gridline.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\gridline-app.exe,0"; Tasks: assocproject
Root: HKA; Subkey: "Software\Classes\Gridline.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gridline-app.exe"" ""%1"""; Tasks: assocproject
Root: HKA; Subkey: "Software\Classes\.flp\OpenWithProgids"; ValueType: string; ValueName: "Gridline.FLP"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocflp
Root: HKA; Subkey: "Software\Classes\Gridline.FLP"; ValueType: string; ValueName: ""; ValueData: "FL Studio project (Gridline)"; Flags: uninsdeletekey; Tasks: assocflp
Root: HKA; Subkey: "Software\Classes\Gridline.FLP\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\gridline-app.exe,0"; Tasks: assocflp
Root: HKA; Subkey: "Software\Classes\Gridline.FLP\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\gridline-app.exe"" ""%1"""; Tasks: assocflp

[Run]
Filename: "{app}\gridline-app.exe"; Description: "{cm:LaunchProgram,Gridline}"; Flags: nowait postinstall skipifsilent
