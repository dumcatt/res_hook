@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"
if not exist "dist" mkdir "dist"

:: 1. Try vswhere.exe to locate the latest Visual Studio / Build Tools vcvars64.bat
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

:: 2. Fallback to common Visual Studio paths if vswhere did not find one
if not defined VCVARS (
    for %%v in (18 2022 2019) do (
        for %%e in (BuildTools Community Professional Enterprise) do (
            if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
            )
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )
)

if defined VCVARS (
    echo [build] Using MSVC: "!VCVARS!"
    call "!VCVARS!" >nul
    cl.exe /nologo /O2 /MT /EHsc /LD /W3 /D_CRT_SECURE_NO_WARNINGS src\res_hook.cpp /Fe:dist\res_hook.dll /Fo:dist\res_hook.obj /link /INCREMENTAL:NO
    if errorlevel 1 (
        echo [build] ERROR: MSVC compilation failed.
        exit /b 1
    )
    del /q dist\res_hook.obj dist\res_hook.lib dist\res_hook.exp >nul 2>&1
    copy /y config\res_hook.ini dist\res_hook.ini >nul
    echo [build] Successfully built dist\res_hook.dll and copied dist\res_hook.ini
    exit /b 0
)

:: 3. Fallback to MinGW-w64 g++ if in PATH
where g++.exe >nul 2>&1
if %errorlevel% equ 0 (
    echo [build] Using MinGW-w64 g++
    g++.exe -shared -O2 -Wall -Wextra -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic src\res_hook.cpp -o dist\res_hook.dll -ld3d9 -lgdi32 -luser32 -lpsapi
    if errorlevel 1 (
        echo [build] ERROR: MinGW-w64 compilation failed.
        exit /b 1
    )
    copy /y config\res_hook.ini dist\res_hook.ini >nul
    echo [build] Successfully built dist\res_hook.dll and copied dist\res_hook.ini
    exit /b 0
)

echo [build] ERROR: Neither Visual Studio C++ Build Tools (cl.exe) nor MinGW-w64 (g++.exe) was found.
exit /b 1
