@echo off
rem Builds the mod and writes dist\RCT3VR-<version>.zip, ready to share.
setlocal
set ROOT=%~dp0
call "%ROOT%build.cmd" || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$root = '%ROOT%'.TrimEnd('\');" ^
  "$version = (Get-Item \"$root\build\d3d9.dll\").VersionInfo.FileVersion;" ^
  "$name = \"RCT3VR-$version\";" ^
  "$stage = \"$root\dist\$name\";" ^
  "if (Test-Path $stage) { Remove-Item $stage -Recurse -Force };" ^
  "New-Item -ItemType Directory \"$stage\files\" | Out-Null;" ^
  "Copy-Item \"$root\build\d3d9.dll\", \"$root\rct3vr.ini\", \"$root\installer\install.ps1\" \"$stage\files\";" ^
  "Copy-Item \"$root\installer\Installer.bat\", \"$root\installer\Uninstall.bat\", \"$root\installer\README_FR.txt\", \"$root\installer\README.txt\" $stage;" ^
  "$zip = \"$root\dist\$name.zip\";" ^
  "if (Test-Path $zip) { Remove-Item $zip -Force };" ^
  "Compress-Archive -Path \"$stage\*\" -DestinationPath $zip;" ^
  "Write-Host \"Package: $zip\""
