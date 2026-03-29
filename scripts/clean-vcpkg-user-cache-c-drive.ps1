#Requires -Version 5.0
<#
  Deletes vcpkg's per-user data under %LOCALAPPDATA%\vcpkg (usually on C:).
  This is the default binary cache location before you set VCPKG_DEFAULT_BINARY_CACHE to D:.

  Does NOT remove: your repo's .\vcpkg clone, .\build-vcpkg-static, or D:\...\vcpkg-binary-cache.

  Usage:
    .\scripts\clean-vcpkg-user-cache-c-drive.ps1 -Force
#>
param(
    [switch] $Force
)

$path = Join-Path $env:LOCALAPPDATA "vcpkg"
if (-not (Test-Path -LiteralPath $path)) {
    Write-Host "INFO: nothing to remove (not found): $path"
    exit 0
}

if (-not $Force) {
    Write-Host "Will DELETE recursively: $path"
    Write-Host "Re-run with -Force to proceed."
    exit 2
}

Write-Host "INFO: removing $path ..."
Remove-Item -LiteralPath $path -Recurse -Force
Write-Host "INFO: done."
exit 0
