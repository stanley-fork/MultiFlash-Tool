#Requires -Version 5.0
<#
  Re-run cmake + build only. Needs existing vcpkg (run complete-vcpkg-static-build.ps1 first).

  Resolves vcpkg path like this:
  1) If -VcpkgRoot is passed and toolchain exists, use it.
  2) Else if repo\..\vcpkg\scripts\buildsystems\vcpkg.cmake exists, use repo vcpkg (fixes wrong global VCPKG_ROOT).
  3) Else if %VCPKG_ROOT% points to a valid toolchain, use it.
  4) Else default to repo\vcpkg.

  Loads MSVC (vcvars64) when cl.exe is missing; finds CMake / Ninja if not in PATH.

  Usage:
    .\scripts\build-vcpkg-static.ps1
    .\scripts\build-vcpkg-static.ps1 -VcpkgRoot D:\vcpkg -Triplet x64-windows-static-md
#>
param(
    [string] $VcpkgRoot = "",
    [string] $Triplet = "x64-windows-static-md",
    [string] $BuildType = "Release",
    [string] $BuildDir = "build-vcpkg-static",
    [string] $CMake = "",
    [string] $Ninja = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Find-VcVars64Bat() {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $inst = & $vswhere -latest -products * -property installationPath 2>$null
    if (-not $inst) { return $null }
    $p = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $p) { return $p }
    return $null
}

function Import-VcVars64Environment([string] $vcVarsPath) {
    if (-not (Test-Path $vcVarsPath)) { return $false }
    $dump = [IO.Path]::GetTempFileName()
    try {
        cmd /c "`"$vcVarsPath`" && set > `"$dump`""
        if ($LASTEXITCODE -ne 0) { return $false }
        Get-Content $dump -Encoding Default | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
            }
        }
        return $true
    } finally {
        Remove-Item -LiteralPath $dump -Force -ErrorAction SilentlyContinue
    }
}

function Find-InPath([string] $name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Find-CMakeExe() {
    if ($CMake -and (Test-Path $CMake)) { return $CMake }
    $p = Find-InPath "cmake"
    if ($p) { return $p }
    foreach ($d in @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
        "C:\Qt\Tools\CMake_64\bin\cmake.exe",
        "D:\Qt\Tools\CMake_64\bin\cmake.exe"
    )) {
        if (Test-Path $d) { return $d }
    }
    return $null
}

function Find-NinjaExe() {
    if ($Ninja -and (Test-Path $Ninja)) { return $Ninja }
    $p = Find-InPath "ninja"
    if ($p) { return $p }
    foreach ($d in @(
        "${env:ProgramFiles}\Ninja\ninja.exe",
        "${env:ChocolateyInstall}\bin\ninja.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    )) {
        if (Test-Path $d) { return $d }
    }
    $vsRoot = "${env:ProgramFiles}\Microsoft Visual Studio"
    if (Test-Path $vsRoot) {
        foreach ($edition in @("Community", "BuildTools", "Professional", "Enterprise", "Preview")) {
            Get-ChildItem $vsRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
                $candidate = Join-Path $_.FullName "$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
                if (Test-Path $candidate) { return $candidate }
            }
        }
    }
    return $null
}

function Test-VcpkgToolchain([string] $root) {
    if (-not $root) { return $false }
    return (Test-Path (Join-Path $root "scripts\buildsystems\vcpkg.cmake"))
}

$localVcpkg = Join-Path $Root "vcpkg"
$resolved = ""

if ($VcpkgRoot -and $VcpkgRoot.Trim() -ne "") {
    $resolved = $VcpkgRoot.Trim()
    if (-not (Test-VcpkgToolchain $resolved)) {
        Write-Error "Invalid -VcpkgRoot (no vcpkg.cmake): $resolved"
    }
}
elseif (Test-VcpkgToolchain $localVcpkg) {
    $resolved = $localVcpkg
    Write-Host "INFO: using repo vcpkg (ignores broken VCPKG_ROOT if any): $resolved"
}
elseif ($env:VCPKG_ROOT -and (Test-VcpkgToolchain $env:VCPKG_ROOT.Trim())) {
    $resolved = $env:VCPKG_ROOT.Trim()
    Write-Host "INFO: using VCPKG_ROOT from environment: $resolved"
}
else {
    $resolved = $localVcpkg
}

$VcpkgRoot = $resolved
$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    Write-Error "vcpkg toolchain not found: $Toolchain. Run .\build-static-vcpkg.bat from repo root first."
}

$vcvarsBat = Find-VcVars64Bat
if ($vcvarsBat -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "INFO: loading MSVC + Windows SDK (vcvars64) ..."
    [void](Import-VcVars64Environment $vcvarsBat)
}

$cmakeExe = Find-CMakeExe
if (-not $cmakeExe) {
    Write-Error "cmake not found. Install CMake or add to PATH."
}

$ninjaExe = Find-NinjaExe
$bundledNinja = Join-Path $Root "tools\ninja\ninja.exe"
if (-not $ninjaExe -and (Test-Path $bundledNinja)) {
    $ninjaExe = $bundledNinja
}

$useVsGenerator = $false
if (-not $ninjaExe) {
    Write-Host "WARN: ninja.exe not found; using Visual Studio 2022 x64 generator (install Ninja for faster builds: winget install Ninja-build.Ninja)."
    $useVsGenerator = $true
} else {
    $ninjaDir = Split-Path -Parent $ninjaExe
    if ($env:PATH -notlike "*$ninjaDir*") {
        $env:PATH = "$ninjaDir;$env:PATH"
    }
}

Write-Host "INFO: CMake: $cmakeExe"
if (-not $useVsGenerator) {
    Write-Host "INFO: Ninja: $ninjaExe"
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "MSVC cl.exe not on PATH. Install VS C++ workload or run from x64 Native Tools after fixing vcvars."
}

Push-Location $Root
try {
    if ($useVsGenerator) {
        $cmakeArguments = @(
            "-B", $BuildDir,
            "-G", "Visual Studio 17 2022", "-A", "x64",
            "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
            "-DVCPKG_TARGET_TRIPLET=$Triplet",
            "-DEDL_STATIC_QT=ON"
        )
    } else {
        $cmakeArguments = @(
            "-B", $BuildDir,
            "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
            "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
            "-DVCPKG_TARGET_TRIPLET=$Triplet",
            "-DCMAKE_BUILD_TYPE=$BuildType",
            "-DEDL_STATIC_QT=ON"
        )
    }
    Write-Host "INFO: $($cmakeExe) $($cmakeArguments -join ' ')"
    & $cmakeExe @cmakeArguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $cmakeExe --build $BuildDir --config $BuildType
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
