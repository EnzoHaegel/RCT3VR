@echo off
rem RCT3 VR - double-click to remove the mod and restore the game.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\install.ps1" -Uninstall %*
