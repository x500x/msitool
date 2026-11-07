@echo off
rem ---------------------------------------------------------------------------
rem  msitool build script for MSVC (Visual Studio 2017 / 2019 / 2022)
rem
rem    build.bat            build x64
rem    build.bat x86        build x86
rem    build.bat clean      remove build outputs
rem
rem  Works from a plain cmd.exe: it locates vcvars64.bat / vcvars32.bat, or
rem  uses an already-initialised "Native Tools Command Prompt".
rem  Override the toolchain with:  set MSIT_VCVARS=D:\path\to\vcvars64.bat
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

set "ARCH=x64"
if /I "%~1"=="x86"   set "ARCH=x86"
if /I "%~1"=="x64"   set "ARCH=x64"
if /I "%~1"=="clean" (
    if exist "%~dp0build" rmdir /s /q "%~dp0build"
    echo Cleaned build directory.
    exit /b 0
)

set "VCBAT=vcvars64.bat"
if /I "%ARCH%"=="x86" set "VCBAT=vcvars32.bat"

set "ROOT=%~dp0"
cd /d "%ROOT%"
if not exist "build\obj\%ARCH%" mkdir "build\obj\%ARCH%"

rem --- already in a developer prompt? ---------------------------------------
where cl >nul 2>nul
if %ERRORLEVEL%==0 goto :compile

rem --- explicit override ----------------------------------------------------
set "VCVARS="
if defined MSIT_VCVARS if exist "%MSIT_VCVARS%" set "VCVARS=%MSIT_VCVARS%"

rem --- vswhere --------------------------------------------------------------
if not defined VCVARS (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\!VCBAT!" set "VCVARS=%%i\VC\Auxiliary\Build\!VCBAT!"
        )
    )
)

rem --- common non-default install locations ---------------------------------
if not defined VCVARS (
    for %%D in (C D E F) do (
        for %%E in (Community Professional Enterprise BuildTools Preview) do (
            if exist "%%D:\vs\%%E\VC\Auxiliary\Build\!VCBAT!" (
                set "VCVARS=%%D:\vs\%%E\VC\Auxiliary\Build\!VCBAT!"
            )
        )
    )
)
if not defined VCVARS (
    for %%Y in (2022 2019 2017) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if exist "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\!VCBAT!" (
                set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\!VCBAT!"
            )
        )
    )
)

if not defined VCVARS (
    echo [!] Could not find !VCBAT!.
    echo     Run from a "Native Tools Command Prompt for VS", or set:
    echo       set MSIT_VCVARS=D:\full\path\to\!VCBAT!
    exit /b 1
)
echo Using: !VCVARS!
call "!VCVARS!" >nul
if errorlevel 1 (
    echo [!] Failed to initialise the MSVC environment.
    exit /b 1
)

:compile
set "MACHINE=X64"
if /I "%ARCH%"=="x86" set "MACHINE=X86"

echo Building msitool ^(%ARCH%^) ...
cl /nologo /std:c++17 /EHsc /W4 /O2 /utf-8 ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /Fobuild\obj\%ARCH%\ /Febuild\msitool_%ARCH%.exe ^
   src\util.cpp src\msidb.cpp src\cab.cpp src\extract.cpp src\main.cpp ^
   Msi.Lib Cabinet.Lib Shell32.Lib ^
   /link /MACHINE:%MACHINE% /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [!] Build FAILED.
    exit /b 1
)
echo Done: build\msitool_%ARCH%.exe
exit /b 0
