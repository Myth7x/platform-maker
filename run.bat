@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if "%~1"=="" goto :usage

set "TARGET=%~1"
shift

set "ARGS="
:collect
if "%~1"=="" goto :dispatch
set "ARGS=!ARGS! %1"
shift
goto :collect

:dispatch
set "BIN_DIR=%ROOT_DIR%\output\bin"

if /I "%TARGET%"=="server" set "EXE_NAME=opm_server.exe" & goto :run
if /I "%TARGET%"=="client" set "EXE_NAME=opm_client.exe" & goto :run

echo Unknown target: %TARGET%
goto :usage

:run
rem Multi-config generators (e.g. Visual Studio) place binaries under a config subdir
set "EXE=%BIN_DIR%\%EXE_NAME%"
if not exist "%EXE%" (
    for %%C in (Debug Release RelWithDebInfo MinSizeRel) do (
        if exist "%BIN_DIR%\%%C\%EXE_NAME%" set "EXE=%BIN_DIR%\%%C\%EXE_NAME%"
    )
)
if not exist "%EXE%" (
    echo Executable not found: %EXE_NAME%
    echo Looked under: %BIN_DIR%
    exit /b 1
)
"%EXE%"%ARGS%
exit /b %errorlevel%

:usage
echo Usage: %~nx0 ^<server^|client^> [args...]
exit /b 1
