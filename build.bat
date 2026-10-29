@echo off
setlocal
cd /d "%~dp0"

echo ==== Fast Travel Time Info - local build ====
echo.

where git >nul 2>nul
if errorlevel 1 goto :nogit
where xmake >nul 2>nul
if errorlevel 1 goto :noxmake

if exist "lib\commonlibsse-ng\xmake.lua" goto :configure
echo Downloading CommonLibSSE-NG v15.0.1 ...
git clone --depth 1 --branch v15.0.1 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG.git lib\commonlibsse-ng
if errorlevel 1 goto :fail

:configure
echo.
echo Configuring ...
call xmake config -m releasedbg -a x64 --yes
if errorlevel 1 goto :fail

echo.
echo Building - the first build can take a while ...
call xmake build --yes FastTravelTimeInfo
if errorlevel 1 goto :fail

echo.
echo Packaging ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1"
if errorlevel 1 goto :fail

echo.
echo Done. Install FastTravelTimeInfo-mod.zip with Vortex.
echo.
pause
exit /b 0

:nogit
echo Git was not found. Install it first, see README.md, then sign out and back in to Windows.
goto :fail

:noxmake
echo xmake was not found. Install it first, see README.md, then sign out and back in to Windows.
goto :fail

:fail
echo.
echo Build stopped. Scroll up to the first line that mentions "error".
echo.
pause
exit /b 1
