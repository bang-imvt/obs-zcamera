; obs-zcamera NSIS installer.
;
; OBS loads a plugin from the OBS installation directory with this layout:
;
;   <obs-studio>\obs-plugins\64bit\          obs-zcamera.dll, ssp-connector.exe,
;                                            libssp.dll, avcodec-61.dll,
;                                            avutil-59.dll, swscale-8.dll,
;                                            swresample-5.dll
;   <obs-studio>\data\obs-plugins\<name>\    locale\*.ini
;
; (A per-user install instead lives in %APPDATA%\obs-studio\plugins\<name>\
; with a bin\64bit\ + data\ subfolder; that layout is what package.ps1's zip
; produces. This installer targets the OBS directory, so it must use the
; obs-plugins\64bit\ layout above -- putting the DLLs in bin\64bit\ there is
; why OBS did not recognise the plugin.)
;
; Build with:  makensis installer.nsi   (from the repo root)

Unicode true

!define PLUGIN_NAME "obs-zcamera"
!define VERSION "0.14.0"

Name "ZCamera (${PLUGIN_NAME}) for OBS Studio"
OutFile "dist\${PLUGIN_NAME}-${VERSION}-setup.exe"
; OBS's own installer defaults here; the directory page lets the operator point
; at a custom OBS location.
InstallDir "$PROGRAMFILES64\obs-studio"
RequestExecutionLevel admin
BrandingText "${PLUGIN_NAME} ${VERSION}"
ShowInstDetails show

!include "MUI2.nsh"
!include "LogicLib.nsh"

!define MUI_WELCOMEPAGE_TITLE "ZCamera plugin for OBS Studio"
!define MUI_WELCOMEPAGE_TEXT "This wizard installs the ZCamera (${PLUGIN_NAME}) plugin, version ${VERSION}, into OBS Studio.$\r$\n$\r$\nSelect your OBS Studio installation folder (the one that contains obs64.exe). The plugin is copied to:$\r$\n$\r$\n    obs-plugins\64bit\        (plugin and runtime DLLs)$\r$\n    data\obs-plugins\${PLUGIN_NAME}\    (translations)$\r$\n$\r$\nClose OBS Studio before continuing."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "The plugin (version ${VERSION}) was installed into:$\r$\n$INSTDIR$\r$\n$\r$\nRestart OBS Studio to load it."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Plugin"
    SetOutPath "$INSTDIR\obs-plugins\64bit"
    File "build_x64\RelWithDebInfo\obs-zcamera.dll"
    File "build_x64\ssp_connector\RelWithDebInfo\ssp-connector.exe"
    File "build_x64\_deps\libssp-src\lib\win_x64_vs2017\libssp.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\avcodec-61.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\avutil-59.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\swscale-8.dll"
    File ".deps\obs-deps-2024-09-12-x64\bin\swresample-5.dll"

    SetOutPath "$INSTDIR\data\obs-plugins\${PLUGIN_NAME}\locale"
    File "data\locale\en-US.ini"
    File "data\locale\zh-CN.ini"
    File "data\locale\de-DE.ini"
    File "data\locale\ru-RU.ini"

    WriteUninstaller "$INSTDIR\uninstall-${PLUGIN_NAME}.exe"
SectionEnd

Section "Uninstall"
    ; Remove only this plugin's files: $INSTDIR is the OBS directory, so a
    ; recursive delete would take the whole installation with it.
    Delete "$INSTDIR\obs-plugins\64bit\obs-zcamera.dll"
    Delete "$INSTDIR\obs-plugins\64bit\ssp-connector.exe"
    Delete "$INSTDIR\obs-plugins\64bit\libssp.dll"
    RMDir /r "$INSTDIR\data\obs-plugins\${PLUGIN_NAME}"
    Delete "$INSTDIR\uninstall-${PLUGIN_NAME}.exe"
SectionEnd
