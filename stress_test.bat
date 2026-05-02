@echo off
setlocal enabledelayedexpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

set "CLIENT=%ROOT_DIR%\output\bin\Debug\opm_client.exe"
set "ADDRESS=%~1"
if "%ADDRESS%"=="" set "ADDRESS=127.0.0.1:34900"

set "LEVEL=%~2"
if "%LEVEL%"=="" set "LEVEL=test_room"

set "INSTANCES=%~3"
if "%INSTANCES%"=="" set "INSTANCES=10"

echo Starting %INSTANCES% test clients connecting to %ADDRESS% on level %LEVEL%

for /L %%i in (1,1,%INSTANCES%) do (
    start "" "%CLIENT%" --test-mode --test-address "%ADDRESS%" --test-level "%LEVEL%"
)

echo Done. %INSTANCES% instances launched.
endlocal
