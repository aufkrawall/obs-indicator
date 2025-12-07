@echo off
setlocal enabledelayedexpansion

:: --- CONFIGURATION ---
set "SOURCE_FILE=OBSIndicator.cpp"
set "EXE_NAME=OBSIndicator.exe"
set "ICON_FILE=icon.ico"
set "RC_FILE=resource.rc"
set "RES_FILE=resource.res"
set "ENV_DIR=%~dp0env\msys64"
set "COMPILER=%ENV_DIR%\ucrt64\bin\clang++.exe"

echo [1/5] Checking for MSYS2 Environment...
if not exist "%COMPILER%" (
    echo [INFO] Compiler not found. Running setup script...
    python setup_build_env.py
    if !errorlevel! neq 0 (
        echo [ERROR] Setup script failed.
        pause
        exit /b
    )
)

:: Set PATH to include MSYS2 UCRT64 bin
set "PATH=%ENV_DIR%\ucrt64\bin;%ENV_DIR%\usr\bin;%PATH%"

echo [2/5] Checking for Icon...
if not exist "%ICON_FILE%" (
    echo [ERROR] "%ICON_FILE%" not found!
    echo         Please run the python script 'generate_icon.py' first.
    pause
    exit /b
)

echo [3/5] Generating Resources...
echo 101 ICON "%ICON_FILE%" > "%RC_FILE%"
:: Use windres from MSYS2
windres "%RC_FILE%" -O coff -o "%RES_FILE%"
if !errorlevel! neq 0 (
    echo [ERROR] Resource compilation failed.
    pause
    exit /b
)

echo [4/5] Compiling with Clang...
if exist "%EXE_NAME%" del "%EXE_NAME%"

:: Compile with Clang (static linking for portability, optimized for size)
:: Flags:
:: -Os: Optimize for size
:: -s: Strip symbols
:: -ffunction-sections -fdata-sections: Place each function/data in own section
:: -Wl,--gc-sections: Linker removes unused sections
:: -fno-expires-entry -fno-rtti: Disable features we likely don't need (optional, but good for size)
clang++ -std=c++17 -Os -DNDEBUG -mwindows -static -s -ffunction-sections -fdata-sections "%SOURCE_FILE%" "%RES_FILE%" -o "%EXE_NAME%" -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcrypt32 -luxtheme -Wl,--gc-sections

if !errorlevel! neq 0 (
    echo [ERROR] Compilation Failed.
    pause
    exit /b
)

echo [5/5] Success! 
echo.
echo You can run %EXE_NAME% manually.
endlocal
