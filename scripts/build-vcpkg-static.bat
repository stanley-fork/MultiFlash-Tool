@echo off
REM Re-configure/build only. Prefers repo\vcpkg when present (ignores broken global VCPKG_ROOT).
REM Delegates to build-vcpkg-static.ps1 (vcvars + CMake/Ninja discovery).
REM Usage: scripts\build-vcpkg-static.bat [triplet]
setlocal
set "ROOT=%~dp0.."
set "LOCAL_VCPKG=%ROOT%\vcpkg"
if exist "%LOCAL_VCPKG%\scripts\buildsystems\vcpkg.cmake" (
  set "VCPKG_ROOT=%LOCAL_VCPKG%"
) else if "%VCPKG_ROOT%"=="" (
  set "VCPKG_ROOT=%LOCAL_VCPKG%"
)
set "TRIPLET=%~1"
if "%TRIPLET%"=="" set "TRIPLET=x64-windows-static-md"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-vcpkg-static.ps1" -VcpkgRoot "%VCPKG_ROOT%" -Triplet "%TRIPLET%"
exit /b %ERRORLEVEL%
