@echo off
REM Static Qt build: clone vcpkg under .\vcpkg\, then cmake. In PowerShell run: .\build-static-vcpkg.bat
REM Requires: Git, CMake, Ninja, MSVC (x64 Native Tools)
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\complete-vcpkg-static-build.ps1" %*
exit /b %ERRORLEVEL%
