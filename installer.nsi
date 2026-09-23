; obs-zcamera NSIS installer.
; Installs the plugin into OBS's plugin directory. OBS loads plugins from
; %ProgramData%\obs-studio\plugins\<name>\ (machine-wide, the location the
; integration test uses) and %APPDATA%\obs-studio\plugins\<name>\. The installer
; defaults to the ProgramData location and copies the standard plugin layout:
;
;   plugins\obs-zcamera\bin\64bit\  obs-zcamera.dll, ssp-connector.exe,
;                                   libssp.dll, avcodec-61.dll, avutil-59.dll,
;                                   swscale-8.dll, swresample-5.dll
;   plugins\obs-zcamera\data\locale\ en-US / zh-CN / de-DE / ru-RU.ini
;
; Build with:  makensis installer.nsi   (from the repo root)

Unicode true
Name "ZCamera (obs-zcamera) for OBS Studio"
OutFile "dist\obs-zcamera-0.14.0-setup.exe"
; OBS loads per-user plugins from %APPDATA%\obs-studio\plugins\<name>\.
InstallDir "$APPDATA\obs-studio\plugins\obs-zcamera"
RequestExecutionLevel user
BrandingText "obs-zcamera 0.14.0"
ShowInstDetails show

!include "MUI2.nsh"
!include "LogicLib.nsh"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "The plugin was installed into:$\r$\n$INSTDIR$\r$\n$\r$\nRestart OBS Studio to load it."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Plugin"
    SetOutPath "$INSTDIR\bin\64bit"
    File "build_x64\RelWithDebInfo\obs-zcamera.dll"
    File "build_x64\ssp_connector\RelWithDebInfo\ssp-connector.exe"
    File "build_x64\_deps\libssp-src\lib\win_x64_vs2017\libssp.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\avcodec-61.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\avutil-59.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\swscale-8.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\swresample-5.dll"

    SetOutPath "$INSTDIR\data\locale"
    File "data\locale\en-US.ini"
    File "data\locale\zh-CN.ini"
    File "data\locale\de-DE.ini"
    File "data\locale\ru-RU.ini"

    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\uninstall.exe"
    RMDir /r "$INSTDIR"
SectionEnd