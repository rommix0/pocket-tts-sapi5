@echo off
setlocal

echo Pocket TTS SAPI5 Build
echo.

set BUILD_DIR_X86=build_x86
set BUILD_DIR_X64=build_x64
set OUTPUT_DIR=output

if not exist %OUTPUT_DIR% mkdir %OUTPUT_DIR%
if not exist %OUTPUT_DIR%\x64 mkdir %OUTPUT_DIR%\x64

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Please install Visual Studio 2022 or later.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
    set "VSINSTALLDIR=%%i\"
)

if not defined VSINSTALLDIR (
    echo ERROR: Visual Studio installation not found.
    exit /b 1
)

echo Building x86 SAPI DLL...
cmake -A Win32 -S . -B %BUILD_DIR_X86%
if errorlevel 1 exit /b 1
cmake --build %BUILD_DIR_X86% --config Release
if errorlevel 1 exit /b 1

echo Building x64 SAPI DLL and Voice Manager...
cmake -A x64 -S . -B %BUILD_DIR_X64%
if errorlevel 1 exit /b 1
cmake --build %BUILD_DIR_X64% --config Release
if errorlevel 1 exit /b 1

echo Copying results to output directory...
copy /Y "%BUILD_DIR_X86%\bin\Release\PocketTTSSAPI.dll" "%OUTPUT_DIR%\"
copy /Y "%BUILD_DIR_X64%\bin\Release\PocketTTSSAPI.dll" "%OUTPUT_DIR%\x64\"
copy /Y "%BUILD_DIR_X64%\bin\Release\PocketTTSVoiceManager.exe" "%OUTPUT_DIR%\"

echo.
echo Build completed. To build the installer, run build_installer.bat
endlocal
