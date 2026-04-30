@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

set "BUILD_DIR=%ROOT_DIR%\output\build\%BUILD_TYPE%"

cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% -j
if errorlevel 1 exit /b %errorlevel%

echo Build complete: %BUILD_TYPE%
endlocal
