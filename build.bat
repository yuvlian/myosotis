@echo off

REM Usage: build.bat
REM build.bat             build everything
REM build.bat dll         build myosotis.dll only
REM build.bat loader      build myoink.exe loader only
REM build.bat dump        build myodump.dll only
REM build.bat test        build test_scan.exe only

setlocal enabledelayedexpansion
cd /d %~dp0

set ZIG=zig
if not exist build mkdir build

set INC=-Iinclude -Igenerated
set DEF=-DUNICODE -D_UNICODE
set WARN=-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wdouble-promotion -Wformat=2 -Wcast-align -Wnull-dereference -Wswitch-enum
set OPT=-target x86_64-windows-gnu -O3 -std=c++23 %WARN% %INC% %DEF%

set WHAT=%1
if "%WHAT%"=="" set WHAT=all

if /i "%WHAT%"=="dll"    (call :build_dll    & goto :done)
if /i "%WHAT%"=="loader" (call :build_loader & goto :done)
if /i "%WHAT%"=="dump"   (call :build_dump   & goto :done)
if /i "%WHAT%"=="test"   (call :build_test   & goto :done)
if /i "%WHAT%"=="all" (
    call :build_dll
    if errorlevel 1 goto :done

    call :build_loader
    if errorlevel 1 goto :done

    call :build_dump
    if errorlevel 1 goto :done

    call :build_test
    goto :done
)
exit /b 2

:build_dll
set SRC=^
src\dllmain.cpp ^
src\init.cpp ^
src\log.cpp ^
src\config.cpp ^
src\hook.cpp ^
src\http.cpp ^
src\il2cpp\pe.cpp ^
src\il2cpp\scan.cpp ^
src\il2cpp\il2cpp_names.cpp ^
src\il2cpp\il2cpp.cpp ^
src\patches\guard.cpp ^
src\patches\login.cpp ^
src\patches\http.cpp ^
src\patches\request.cpp
%ZIG% c++ %OPT% -shared %SRC% -lwinhttp -lkernel32 -luser32 -lole32 -ladvapi32 -o build\myosotis.dll
if errorlevel 1 ( echo DLL BUILD FAILED & exit /b 1 )
echo built build\myosotis.dll
exit /b 0

:build_loader
%ZIG% c++ %OPT% -municode tools\loader.cpp -lkernel32 -o build\myoink.exe
if errorlevel 1 ( echo LOADER BUILD FAILED & exit /b 1 )
echo built build\myoink.exe
exit /b 0

:build_dump
set DSRC=^
src\dump.cpp ^
src\log.cpp ^
src\config.cpp ^
src\il2cpp\pe.cpp ^
src\il2cpp\scan.cpp ^
src\il2cpp\il2cpp_names.cpp ^
src\il2cpp\il2cpp.cpp
%ZIG% c++ %OPT% -shared %DSRC% -lkernel32 -luser32 -o build\myodump.dll
if errorlevel 1 ( echo DUMP BUILD FAILED & exit /b 1 )
echo built build\myodump.dll
exit /b 0

:build_test
set TSRC=^
tools\test_scan.cpp ^
src\log.cpp ^
src\il2cpp\pe.cpp ^
src\il2cpp\scan.cpp ^
src\il2cpp\il2cpp_names.cpp
%ZIG% c++ %OPT% %TSRC% -lkernel32 -luser32 -o build\test_scan.exe
if errorlevel 1 ( echo TEST BUILD FAILED & exit /b 1 )
echo built build\test_scan.exe
exit /b 0

:done
endlocal
