@echo off
rem Double-click to open the MK9 Rollback netplay window (tools\netplay_gui.ps1).
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0netplay_gui.ps1"
