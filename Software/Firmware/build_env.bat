@echo off
REM Build wrapper. west and the NCS toolchain live Windows-side, so the build has
REM to run through cmd.exe rather than from a WSL shell with exported vars.
REM
REM Only the two NCS paths below should need editing; the repo location is taken
REM from where this file sits.
set NCS=C:\ncs\toolchains\936afb6332
set ZEPHYR_BASE=C:\ncs\v3.3.1\zephyr

set PATH=%NCS%\opt\bin;%NCS%\opt\bin\Scripts;%NCS%\mingw64\bin;%NCS%\bin;%NCS%\usr\bin;%PATH%

REM %~dp0 is this script's directory, with a trailing backslash. Strip it, then
REM swap to forward slashes: CMake wants those in BOARD_ROOT.
set APP_DIR=%~dp0
set APP_DIR=%APP_DIR:~0,-1%
set BOARD_ROOT=%APP_DIR:\=/%

cd /d "%APP_DIR%"
west build -b sensering_v29 -- -DBOARD_ROOT=%BOARD_ROOT%
