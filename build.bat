@echo off
setlocal enabledelayedexpansion

:: --- CONFIGURATION ---
set "SOURCE_FILE=OBSIndicator.cpp"
set "EXE_NAME=OBSIndicator.exe"
set "ICON_FILE=icon.ico"
set "RC_FILE=resource.rc"
set "RES_FILE=resource.res"

echo [1/5] Checking for Compiler Environment...

:: Check if cl.exe is in PATH
where cl.exe >nul 2>nul
if %errorlevel% equ 0 goto :ENV_READY

:: If not, try to locate VS
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found.
    echo         Please open this script in the "Developer Command Prompt for VS".
    pause
    exit /b
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_INSTALL_DIR=%%i"
)

if "%VS_INSTALL_DIR%"=="" (
    echo [ERROR] No Visual Studio C++ found.
    pause
    exit /b
)

:: Initialize MSVC Environment
call "%VS_INSTALL_DIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul

:ENV_READY

echo [2/5] Checking for Icon...
if not exist "%ICON_FILE%" (
    echo [ERROR] %ICON_FILE% not found!
    echo         Please run the python script 'generate_icon.py' first.
    pause
    exit /b
)

echo [3/5] Generating Resources...
:: Create a resource script that links the icon to ID 101
echo 101 ICON "%ICON_FILE%" > "%RC_FILE%"
:: Compile resource script to .res
rc.exe /nologo "%RC_FILE%"

echo [4/5] Compiling C++...
if exist "%EXE_NAME%" del "%EXE_NAME%"

:: Compile CPP and link with RES
cl.exe /nologo /O2 /EHsc /DNDEBUG "%SOURCE_FILE%" "%RES_FILE%" /Fe:"%EXE_NAME%" /link /SUBSYSTEM:WINDOWS

if %errorlevel% neq 0 (
    echo [ERROR] Compilation Failed.
    pause
    exit /b
)

echo [5/5] Success! 
echo.
echo You can run %EXE_NAME% manually or with --tray
endlocal