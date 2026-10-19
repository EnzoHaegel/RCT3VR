@echo off
rem Builds the 32-bit d3d9.dll proxy. Pass "install" to copy it into the game folder.
setlocal
set ROOT=%~dp0
set GAME=C:\Program Files\Epic Games\RollerCoasterTycoon3

set VS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)

call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >nul || exit /b 1
cmake --build "%ROOT%build" || exit /b 1

if /i "%1"=="install" (
  copy /y "%ROOT%build\d3d9.dll" "%GAME%\d3d9.dll" >nul || exit /b 1
  if not exist "%GAME%\rct3vr.ini" copy /y "%ROOT%rct3vr.ini" "%GAME%\rct3vr.ini" >nul
  echo Installed to %GAME%
)
