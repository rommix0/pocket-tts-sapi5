@echo off
setlocal

REM Builds the full Pocket TTS SAPI5 installer.
REM Prerequisites: build_all.bat has produced output\, and the models exist
REM in C:\ProgramData\PocketTTS\models (they do after development setup).

set ROOT=%~dp0
set ISCC="%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist %ISCC% set ISCC="C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist %ISCC% (
    echo ERROR: Inno Setup 6 ISCC.exe not found.
    exit /b 1
)

if not exist "%ROOT%output\PocketTTSSAPI.dll" (
    echo ERROR: run build_all.bat first.
    exit /b 1
)

if not exist "%ROOT%runtime\PocketTTSHost.exe" (
    echo Assembling the Python runtime...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%installer\prepare_runtime.ps1"
    if errorlevel 1 exit /b 1
)

if not exist "%ROOT%installer\staging\voices\voices.ini" (
    echo Building the default voice payload...
    "%USERPROFILE%\.pockettts\venv\Scripts\python.exe" "%ROOT%installer\prepare_voices.py"
    if errorlevel 1 exit /b 1
)

echo Compiling the installer...
%ISCC% "%ROOT%installer\pockettts.iss"
if errorlevel 1 exit /b 1

echo.
echo Installer written to output\PocketTTS_SAPI5_Setup.exe
endlocal
