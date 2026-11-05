@echo off
rem fnv_recomp: send a build to the PS5 and fetch the log, over the console's FTP server.
rem Needs an FTP server payload running on the PS5 (etaHEN's built-in FTP, or ftpsrv).
rem Set PS5_IP and PORT below to what the PS5 shows when its FTP server starts.
rem Uses curl.exe and tar.exe, which come with Windows 10 and 11.
rem   Drag a build zip (or an eboot.bin) onto this file: it is uploaded to the app folder.
rem   Double-click this file: fnv.log is downloaded from the PS5 next to this file.
rem Close the game on the PS5 before uploading a new build.
setlocal
set PS5_IP=EDIT_ME
set PORT=2121
set URL=ftp://%PS5_IP%:%PORT%/%%2Fdata/homebrew/PPSA97701
if "%PS5_IP%"=="EDIT_ME" (
  echo Open this file in Notepad and set PS5_IP and PORT first.
  pause
  exit /b 1
)
if "%~1"=="" goto getlog

set FILE=%~1
if /i "%~x1"==".zip" (
  rmdir /s /q "%TEMP%\fnv_ps5_upload" 2>nul
  mkdir "%TEMP%\fnv_ps5_upload"
  tar -xf "%~1" -C "%TEMP%\fnv_ps5_upload" eboot.bin
  if errorlevel 1 (
    echo Could not unpack eboot.bin from %~nx1
    pause
    exit /b 1
  )
  set FILE=%TEMP%\fnv_ps5_upload\eboot.bin
)
echo Uploading eboot.bin to the PS5 ...
curl.exe --fail -T "%FILE%" "%URL%/eboot.bin"
if errorlevel 1 (
  echo Upload failed. Is the FTP server running on the PS5, and are PS5_IP and PORT right?
  pause
  exit /b 1
)
echo Done. Start the game from the PS5 home screen.
pause
exit /b 0

:getlog
echo Downloading fnv.log from the PS5 ...
curl.exe --fail -s "%URL%/fnv.log" -o "%~dp0fnv.log"
if errorlevel 1 (
  echo Download failed. Is the FTP server running on the PS5, and are PS5_IP and PORT right?
  pause
  exit /b 1
)
echo Saved %~dp0fnv.log
pause
