@echo off
REM Wrapper: see docs\BUILD_STATIC.md. PowerShell: .\scripts\complete-vcpkg-static-build.bat
REM Requires: Git, CMake, Ninja, MSVC
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0complete-vcpkg-static-build.ps1" %*
exit /b %ERRORLEVEL%
