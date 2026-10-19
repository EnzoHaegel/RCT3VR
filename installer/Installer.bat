@echo off
rem RCT3 VR - double-click to install, update or uninstall the mod.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\install.ps1" %*
