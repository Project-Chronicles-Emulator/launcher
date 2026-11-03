Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "Project Chronicles"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Chronicles"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
BrandingText "Project Chronicles · Your next chapter"
VIProductVersion "0.2.6.0"
VIAddVersionKey "ProductName" "Project Chronicles Launcher"
VIAddVersionKey "FileDescription" "Project Chronicles Launcher Setup"
VIAddVersionKey "FileVersion" "0.2.6"
VIAddVersionKey "LegalCopyright" "Project Chronicles"

!define MUI_ICON "${PROJECT_ROOT}\resources\chronicles-launcher.ico"
!define MUI_UNICON "${PROJECT_ROOT}\resources\chronicles-launcher.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${PROJECT_ROOT}\resources\installer-welcome.bmp"
!define MUI_WELCOMEPAGE_TITLE "Welcome to Project Chronicles"
!define MUI_WELCOMEPAGE_TEXT "Your next adventure starts here.$\r$\n$\r$\nSetup installs the launcher and its required components. Project Chronicles finds existing game files and prepares your game automatically.$\r$\n$\r$\nSign in with Discord or your Project Chronicles account. Steam is not required.$\r$\n$\r$\nVersion 0.2.6 · Windows x64"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_TITLE "Your next chapter awaits"
!define MUI_FINISHPAGE_TEXT "Project Chronicles is installed. The launcher will verify your game and download any files it needs.$\r$\n$\r$\nYou can change the game folder later in Settings. An internet connection is required."
!define MUI_FINISHPAGE_RUN "$INSTDIR\chronicles-launcher.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Project Chronicles"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Project Chronicles requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext current
FunctionEnd

Function .onVerifyInstDir
  IfFileExists "$INSTDIR\Chronicles.installed" allowed
  IfFileExists "$INSTDIR\*.*" 0 allowed
  MessageBox MB_ICONEXCLAMATION "Choose an empty folder, or the existing Project Chronicles launcher folder. Your game folder is selected automatically after setup."
  Abort
  allowed:
FunctionEnd

Section "Project Chronicles Launcher"
  SetOutPath "$INSTDIR"
  File /r /x runtime "${PACKAGE_ROOT}\*.*"
  FileOpen $0 "$INSTDIR\Chronicles.installed" w
  FileWrite $0 "Project Chronicles Launcher 0.2.6"
  FileClose $0
  WriteUninstaller "$INSTDIR\Uninstall Chronicles.exe"
  CreateDirectory "$SMPROGRAMS\Chronicles"
  CreateShortcut "$SMPROGRAMS\Chronicles\Chronicles.lnk" "$INSTDIR\chronicles-launcher.exe"
  CreateShortcut "$DESKTOP\Chronicles.lnk" "$INSTDIR\chronicles-launcher.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "DisplayName" "Project Chronicles Launcher"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "DisplayVersion" "0.2.6"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "Publisher" "Project Chronicles"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "DisplayIcon" "$INSTDIR\chronicles-launcher.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "UninstallString" '$\"$INSTDIR\Uninstall Chronicles.exe$\"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  !include "${UNINSTALL_LIST}"
  Delete "$INSTDIR\Chronicles.installed"
  Delete "$INSTDIR\Uninstall Chronicles.exe"
  Delete "$DESKTOP\Chronicles.lnk"
  Delete "$SMPROGRAMS\Chronicles\Chronicles.lnk"
  RMDir "$SMPROGRAMS\Chronicles"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Chronicles"
  ; The generated list removes only shipped files. Keep the game, settings and saves.
  RMDir "$INSTDIR"
SectionEnd
