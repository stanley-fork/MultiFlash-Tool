#Requires -Version 5.0
<#
  EDL static Qt build: clone vcpkg -> bootstrap -> cmake -> build.
  Needs: CMake, Ninja, MSVC (x64 Native Tools). Git recommended (clone vcpkg); if Git is broken,
  the script falls back to downloading the vcpkg tree as a ZIP from GitHub. Default vcpkg dir: repo\vcpkg

  Usage:
    .\scripts\complete-vcpkg-static-build.ps1
    .\build-static-vcpkg.bat
    .\scripts\complete-vcpkg-static-build.ps1 -VcpkgRoot D:\path\to\vcpkg
    .\scripts\complete-vcpkg-static-build.ps1 -SkipBuild
    .\scripts\complete-vcpkg-static-build.ps1 -BinaryCacheDir D:\vcpkg-binary-cache
#>
param(
    [string] $VcpkgRoot = "",
    [string] $Triplet = "x64-windows-static-md",
    [string] $BuildDir = "build-vcpkg-static",
    [string] $CMake = "",
    [string] $Git = "",
    [string] $Ninja = "",
    [string] $BinaryCacheDir = "",
    [switch] $SkipBuild
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

$vcvarsBat = Find-VcVars64Bat
if ($vcvarsBat) {
    Write-Host "INFO: loading MSVC + Windows SDK environment (vcvars64) ..."
    [void](Import-VcVars64Environment $vcvarsBat)
} else {
    Write-Host "WARN: vcvars64.bat not found via vswhere; ensure you run from x64 Native Tools or have VS C++ installed."
}

function Find-InPath([string] $name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Find-CMake() {
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

function Find-Git() {
    if ($Git -and (Test-Path $Git)) { return $Git }
    # Prefer Git for Windows install over PATH (e.g. Anaconda's git.exe can trigger
    # "BUG (fork bomb): ...\git.exe" when it shadows the real Git).
    foreach ($d in @(
        "${env:ProgramFiles}\Git\cmd\git.exe",
        "${env:ProgramFiles}\Git\mingw64\bin\git.exe",
        "${env:ProgramFiles}\Git\bin\git.exe",
        "${env:ProgramFiles(x86)}\Git\cmd\git.exe",
        "${env:ProgramFiles(x86)}\Git\bin\git.exe",
        "${env:LocalAppData}\Programs\Git\cmd\git.exe",
        "${env:LocalAppData}\Programs\Git\bin\git.exe"
    )) {
        if (Test-Path $d) { return $d }
    }
    $p = Find-InPath "git"
    if ($p) { return $p }
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

function Install-VcpkgFromZip([string] $destRoot) {
    if (Test-Path (Join-Path $destRoot "scripts\buildsystems\vcpkg.cmake")) { return }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $zipUrl = "https://github.com/microsoft/vcpkg/archive/refs/heads/master.zip"
    $tmpZip = Join-Path $env:TEMP ("vcpkg-master-" + [guid]::NewGuid().ToString() + ".zip")
    $extractParent = Join-Path $env:TEMP ("vcpkg-extract-" + [guid]::NewGuid().ToString())
    try {
        Write-Host "INFO: downloading vcpkg ZIP from GitHub (fallback when git clone is unavailable)..."
        Invoke-WebRequest -Uri $zipUrl -OutFile $tmpZip -UseBasicParsing
        New-Item -ItemType Directory -Path $extractParent -Force | Out-Null
        Expand-Archive -LiteralPath $tmpZip -DestinationPath $extractParent -Force
        $extracted = Join-Path $extractParent "vcpkg-master"
        if (-not (Test-Path $extracted)) {
            throw "ZIP extract did not contain vcpkg-master folder"
        }
        if (Test-Path $destRoot) {
            Remove-Item -LiteralPath $destRoot -Recurse -Force
        }
        $parent = Split-Path -Parent $destRoot
        if (-not (Test-Path $parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Move-Item -LiteralPath $extracted -Destination $destRoot
    } finally {
        Remove-Item -LiteralPath $tmpZip -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $extractParent -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$cmakeExe = Find-CMake
$gitExe = Find-Git
if (-not $gitExe) {
    Write-Host "WARN: git not found or unusable; will use ZIP download for vcpkg if needed."
}
if (-not $cmakeExe) {
    Write-Error "cmake not found. Install CMake and add to PATH."
}

$ninjaExe = Find-NinjaExe
if (-not $ninjaExe) {
    Write-Error "ninja.exe not found (required for -G Ninja). Install Ninja (e.g. winget install Ninja-build.Ninja) or pass -Ninja C:\path\to\ninja.exe"
}
$ninjaDir = Split-Path -Parent $ninjaExe
if ($env:PATH -notlike "*$ninjaDir*") {
    $env:PATH = "$ninjaDir;$env:PATH"
}
Write-Host "INFO: using Ninja: $ninjaExe"

if (-not $VcpkgRoot -or $VcpkgRoot.Trim() -eq "") {
    $VcpkgRoot = Join-Path $Root "vcpkg"
    Write-Host "INFO: vcpkg directory (under repo): $VcpkgRoot"
}

$tc = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $tc)) {
    if (-not (Test-Path $VcpkgRoot)) {
        New-Item -ItemType Directory -Path $VcpkgRoot -Force | Out-Null
    }
    $items = @(Get-ChildItem -Path $VcpkgRoot -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -notin @('.', '..') })
    if ($items.Count -eq 0) {
        $cloneOk = $false
        if ($gitExe) {
            Write-Host "INFO: cloning vcpkg into $VcpkgRoot ..."
            # Shallow clone is enough for vcpkg; faster and smaller.
            & $gitExe @("clone", "--depth", "1", "https://github.com/microsoft/vcpkg.git", $VcpkgRoot)
            if ($LASTEXITCODE -eq 0) { $cloneOk = $true }
        }
        if (-not $cloneOk) {
            if ($gitExe -and $LASTEXITCODE -ne 0) {
                Write-Host "WARN: git clone failed (exit $LASTEXITCODE). Trying ZIP download..."
            }
            if (Test-Path $VcpkgRoot) {
                Remove-Item -LiteralPath $VcpkgRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            try {
                Install-VcpkgFromZip $VcpkgRoot
            } catch {
                $msg = (
                    "Could not obtain vcpkg (git clone and ZIP both failed).`n" +
                    "Last error: $($_.Exception.Message)`n`n" +
                    "If you saw 'BUG (fork bomb)': repair Git for Windows or fix PATH (e.g. conda deactivate).`n" +
                    "Or extract https://github.com/microsoft/vcpkg/archive/refs/heads/master.zip into:`n  $VcpkgRoot`n" +
                    "Or use: -VcpkgRoot D:\path\to\existing\vcpkg"
                )
                Write-Error $msg
            }
        }
    } elseif (-not (Test-Path (Join-Path $VcpkgRoot "scripts"))) {
        Write-Error "Folder exists but is not a vcpkg clone: $VcpkgRoot -- remove or use -VcpkgRoot"
    }
}

if (-not (Test-Path $tc)) {
    Write-Error "vcpkg toolchain not found: $tc"
}

$bootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
    if (-not (Test-Path $bootstrap)) {
        Write-Error "bootstrap-vcpkg.bat not found: $bootstrap"
    }
    Write-Host "INFO: running bootstrap-vcpkg.bat ..."
    Push-Location $VcpkgRoot
    try {
        cmd /c "bootstrap-vcpkg.bat"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}

$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "INFO: VCPKG_ROOT=$VcpkgRoot"

# Binary cache: default to <parent of repo>\vcpkg-binary-cache (e.g. D:\EDL\vcpkg-binary-cache) so C: AppData is not used.
if (-not $BinaryCacheDir -or $BinaryCacheDir.Trim() -eq "") {
    $BinaryCacheDir = Join-Path (Split-Path $Root -Parent) "vcpkg-binary-cache"
}
New-Item -ItemType Directory -Path $BinaryCacheDir -Force | Out-Null
$BinaryCacheDir = (Resolve-Path -LiteralPath $BinaryCacheDir).Path
$env:VCPKG_DEFAULT_BINARY_CACHE = $BinaryCacheDir
Write-Host "INFO: VCPKG_DEFAULT_BINARY_CACHE=$BinaryCacheDir (vcpkg binary cache on disk, not C: AppData)"
Write-Host "INFO: first cmake may take hours (qtbase, qtsvg) and much disk space."

Push-Location $Root
try {
    $cmakeArguments = @(
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
        "-DCMAKE_TOOLCHAIN_FILE=$tc",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DEDL_STATIC_QT=ON"
    )
    # Rely on PATH from Import-VcVars64Environment (rc.exe, link.exe, mt.exe, cl.exe). Do not pin cl.exe alone.
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Write-Error "MSVC cl.exe not on PATH after vcvars64. Install VS C++ workload or run from x64 Native Tools."
    }
    Write-Host "INFO: cmake command: $cmakeExe $($cmakeArguments -join ' ')"
    & $cmakeExe @cmakeArguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if (-not $SkipBuild) {
        Write-Host "INFO: building EDL ..."
        & $cmakeExe --build $BuildDir --config Release
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}
