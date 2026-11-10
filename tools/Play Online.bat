@echo off
rem Double-click to open the MK9 Rollback netplay window (tools\netplay_gui.ps1).
if not exist "%~dp0netplay_gui.ps1" (
    echo Extract the whole zip first: right-click it, 'Extract All', then open Play Online.bat in the extracted folder.
    pause
    exit /b 1
)
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0netplay_gui.ps1" %*
