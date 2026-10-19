@echo off
rem Counter-Strike (Xbox), native Windows build: runs from this folder (logs\ and hdd\ are created here) with the
rem Vulkan renderer. The argument is the folder holding the game's own files (default.xbe, Media\, the .sxwad maps).
setlocal
cd /d "%~dp0"
if "%~1"=="" (
    echo usage: run_game.bat "C:\path\to\Counter-Strike"
    exit /b 2
)
if not exist "%~1\default.xbe" (
    echo no default.xbe in "%~1"
    exit /b 2
)
if not defined CSX_RENDERER set CSX_RENDERER=vulkan
CounterStrike.exe "%~1" --renderer %CSX_RENDERER%
