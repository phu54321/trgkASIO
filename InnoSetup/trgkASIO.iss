; Script generated by the Inno Setup Script Wizard.
; SEE THE DOCUMENTATION FOR DETAILS ON CREATING INNO SETUP SCRIPT FILES!

#define MyAppName "trgkASIO"
#define MyAppVersion "23.7.25.1017"
#define MyAppPublisher "Park Hyunwoo"
#define MyAppURL "https://github.com/phu54321"
#define clsid "{{E3226090-473D-4CC9-8360-E123EB9EF847}}"

[Setup]
; NOTE: The value of AppId uniquely identifies this application. Do not use the same AppId value in installers for other applications.
; (To generate a new GUID, click Tools | Generate GUID inside the IDE.)
AppId={{C57399D2-631B-4AD3-917F-05D80C6389A7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
;AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={commonpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableDirPage=yes
LicenseFile=..\LICENSE.txt
; Uncomment the following line to run in non administrative install mode (install for current user only.)
;PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
OutputDir=.\output
OutputBaseFilename=Setup_trgkASIO
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\\Build-RelWithDebInfo\\x64\trgkASIO64.dll"; DestDir: "{commonpf64}\\{#MyAppName}"; DestName: "trgkASIO.dll"; Flags: ignoreversion regserver; Check: Is64BitInstallMode
Source: "..\\Build-RelWithDebInfo\\trgkASIO.dll"; DestDir: "{commonpf32}\\{#MyAppName}"; Flags: ignoreversion regserver 32bit; Check: not Is64BitInstallMode
Source: "..\\Build-RelWithDebInfo\\trgkASIO.dll"; DestDir: "{commonpf32}\\{#MyAppName}"; Flags: ignoreversion; Check: Is64BitInstallMode
Source: "..\\LICENSE.txt"; DestDir: "{autopf}\{#MyAppName}"; Flags: ignoreversion
Source: "..\\Build-RelWithDebInfo\\Configurator.exe"; DestDir: "{autopf}\{#MyAppName}"; Flags: ignoreversion
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Icons]
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{group}\trgkASIO Configurator"; Filename: "{autopf}\{#MyAppName}\Configurator.exe"; WorkingDir: "{autopf}"
