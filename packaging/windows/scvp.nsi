; Per-user installer for SCVP: no administrator rights, nothing written outside the user's profile
; and HKCU. Installs the files of the zip package (Qt, FFmpeg and the VC++ runtime included),
; a Start menu entry, an uninstaller (Settings > Apps) and "Open with" for video files. SCVP also
; appears in Settings > Default apps; Windows lets only the user make it the default there.
;
; Built in CI from the unpacked zip plus scvp.ico (scvp.exe --export-ico):
;   makensis /DVERSION=0.1.0 /DSRC=<folder with bin\ and scvp.ico> /DOUT=<setup.exe> scvp.nsi
; Silent install: setup.exe /S        uninstall: "%LOCALAPPDATA%\Programs\SCVP\Uninstall.exe" /S

!ifndef VERSION | SRC | OUT
  !error "define VERSION, SRC and OUT"
!endif

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!define APP "SCVP"
!define PROGID "SCVP.video"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SCVP"
!define CAPS_KEY "Software\SCVP\Capabilities"

Name "${APP}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\SCVP"
InstallDirRegKey HKCU "Software\SCVP" "InstallDir"
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "${APP}"
VIAddVersionKey /LANG=0 "FileDescription" "${APP} installer"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "LegalCopyright" "Copyright (c) 2026 Seungchan Mok (MIT License)"

!include "MUI2.nsh"
!define MUI_ICON "${SRC}\scvp.ico"
!define MUI_UNICON "${SRC}\scvp.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\scvp.exe"
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(DefaultAppsText)"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenDefaultApps

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Korean"

LangString DefaultAppsText ${LANG_ENGLISH} "Choose SCVP as the default video player (Windows Settings)"
LangString DefaultAppsText ${LANG_KOREAN} "SCVP를 기본 비디오 플레이어로 선택 (Windows 설정)"
LangString TypeName ${LANG_ENGLISH} "Video (SCVP)"
LangString TypeName ${LANG_KOREAN} "비디오 (SCVP)"

; Video files SCVP is offered for in "Open with" (containers and raw streams, as scvp-mime.xml).
!macro ForEachVideoType MACRO
  !insertmacro ${MACRO} ".mp4"
  !insertmacro ${MACRO} ".m4v"
  !insertmacro ${MACRO} ".mkv"
  !insertmacro ${MACRO} ".webm"
  !insertmacro ${MACRO} ".mov"
  !insertmacro ${MACRO} ".avi"
  !insertmacro ${MACRO} ".mpg"
  !insertmacro ${MACRO} ".mpeg"
  !insertmacro ${MACRO} ".m1v"
  !insertmacro ${MACRO} ".m2v"
  !insertmacro ${MACRO} ".ts"
  !insertmacro ${MACRO} ".m2ts"
  !insertmacro ${MACRO} ".mts"
  !insertmacro ${MACRO} ".flv"
  !insertmacro ${MACRO} ".wmv"
  !insertmacro ${MACRO} ".asf"
  !insertmacro ${MACRO} ".mxf"
  !insertmacro ${MACRO} ".3gp"
  !insertmacro ${MACRO} ".3g2"
  !insertmacro ${MACRO} ".ogv"
  !insertmacro ${MACRO} ".dv"
  !insertmacro ${MACRO} ".264"
  !insertmacro ${MACRO} ".265"
  !insertmacro ${MACRO} ".h264"
  !insertmacro ${MACRO} ".h265"
  !insertmacro ${MACRO} ".hevc"
  !insertmacro ${MACRO} ".ivf"
  !insertmacro ${MACRO} ".obu"
  !insertmacro ${MACRO} ".y4m"
  !insertmacro ${MACRO} ".yuv"
!macroend

!macro RegisterType EXT
  WriteRegStr HKCU "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKCU "Software\Classes\Applications\scvp.exe\SupportedTypes" "${EXT}" ""
  WriteRegStr HKCU "${CAPS_KEY}\FileAssociations" "${EXT}" "${PROGID}"
!macroend

!macro UnregisterType EXT
  DeleteRegValue HKCU "Software\Classes\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey /ifempty HKCU "Software\Classes\${EXT}\OpenWithProgids"
  DeleteRegKey /ifempty HKCU "Software\Classes\${EXT}"
!macroend

; The parts of $INSTDIR this installer owns: an update replaces them whole (no DLL of an older
; FFmpeg or Qt left behind), and the uninstaller removes only them, never the folder's other
; contents should the user have picked a folder that holds more.
!macro RemoveOwnedFiles
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\plugins"
  RMDir /r "$INSTDIR\translations"
  Delete "$INSTDIR\scvp.ico"
!macroend

Function OpenDefaultApps
  ExecShell "open" "ms-settings:defaultapps?registeredAppUser=${APP}"
FunctionEnd

Section "SCVP"
  SetOutPath "$INSTDIR"
  !insertmacro RemoveOwnedFiles
  File /r "${SRC}\bin"
  File /r "${SRC}\plugins"
  File /nonfatal /r "${SRC}\translations"
  File "${SRC}\scvp.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\SCVP" "InstallDir" "$INSTDIR"

  CreateShortCut "$SMPROGRAMS\SCVP.lnk" "$INSTDIR\bin\scvp.exe" "" "$INSTDIR\scvp.ico" 0

  ; A program id for the video types, the application entry "Open with" lists, and the
  ; capabilities that make SCVP a choice in Settings > Default apps.
  WriteRegStr HKCU "Software\Classes\${PROGID}" "" "$(TypeName)"
  WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\scvp.ico"
  WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\bin\scvp.exe" "%1"'
  WriteRegStr HKCU "Software\Classes\Applications\scvp.exe" "FriendlyAppName" "${APP}"
  WriteRegStr HKCU "Software\Classes\Applications\scvp.exe\DefaultIcon" "" "$INSTDIR\scvp.ico"
  WriteRegStr HKCU "Software\Classes\Applications\scvp.exe\shell\open\command" "" '"$INSTDIR\bin\scvp.exe" "%1"'
  WriteRegStr HKCU "${CAPS_KEY}" "ApplicationName" "${APP}"
  WriteRegStr HKCU "${CAPS_KEY}" "ApplicationIcon" "$INSTDIR\scvp.ico"
  WriteRegStr HKCU "${CAPS_KEY}" "ApplicationDescription" "Frame-accurate comparison of two videos with pixel values"
  !insertmacro ForEachVideoType RegisterType
  WriteRegStr HKCU "Software\RegisteredApplications" "${APP}" "${CAPS_KEY}"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\scvp.ico"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  ; Tell Explorer the associations changed (SHCNE_ASSOCCHANGED).
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\SCVP.lnk"
  !insertmacro ForEachVideoType UnregisterType
  DeleteRegKey HKCU "Software\Classes\${PROGID}"
  DeleteRegKey HKCU "Software\Classes\Applications\scvp.exe"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP}"
  DeleteRegKey HKCU "Software\SCVP"
  DeleteRegKey HKCU "${UNINST_KEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  !insertmacro RemoveOwnedFiles
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
