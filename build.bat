@echo off
setlocal

if not exist build mkdir build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Could not find Visual Studio Installer's vswhere.exe.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo Could not find a Visual Studio C++ installation.
    exit /b 1
)

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%

if /i "%~1"=="check" (
    cl /nologo /std:c++20 /W4 /EHsc /DUNICODE /D_UNICODE tests\editor_checks.cpp /Fo:build\editor-checks.obj /Fe:build\editor-checks.exe /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib comdlg32.lib comctl32.lib
    if errorlevel 1 exit /b 1
    build\editor-checks.exe
    if errorlevel 1 exit /b 1
    exit /b 0
)

cl /nologo /std:c++20 /W4 /EHsc /DUNICODE /D_UNICODE src\main.cpp /Fe:build\octavo.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib comctl32.lib
if errorlevel 1 exit /b %errorlevel%

echo Built build\octavo.exe

