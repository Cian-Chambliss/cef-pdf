@echo off
setlocal

set "SCRIPT=%~dp0smoke-streamed.ps1"
if "%~1"=="" (
  powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
) else (
  powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -ExePath "%~f1"
)

rem Do not pipe cef-pdf in cmd.exe: ERRORLEVEL would report the last pipeline command.
set "RESULT=%ERRORLEVEL%"
endlocal & exit /b %RESULT%
