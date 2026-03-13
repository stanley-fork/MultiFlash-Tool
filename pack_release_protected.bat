@echo off
chcp 65001 >nul
setlocal

:: 多文件发布；若本机有 ConfuserEx 则尝试混淆（注：ConfuserEx 官方版不支持 .NET 8，可能失败，此时仅发布不混淆）
set PUBLISH_DIR=bin\Release\net8.0-windows\publish
set "CONFUSER_CLI=C:\Users\Administrator\Desktop\rpg\ConfuserEx_bin\Confuser.CLI.exe"
if not exist "%CONFUSER_CLI%" set "CONFUSER_CLI=%~dp0tools\ConfuserEx\Confuser.CLI.exe"
if not exist "%CONFUSER_CLI%" set "CONFUSER_CLI=C:\ConfuserEx\Confuser.CLI.exe"
if not exist "%CONFUSER_CLI%" set "CONFUSER_CLI="

echo [0/3] 清理旧输出...
if exist "%PUBLISH_DIR%" rd /s /q "%PUBLISH_DIR%"

echo [1/3] 发布 Release 多文件（不包含 .NET 运行时）...
dotnet publish SakuraEDL.csproj -c Release -r win-x64 --self-contained false -p:PublishSingleFile=false -o "%PUBLISH_DIR%"
if errorlevel 1 ( echo 发布失败. & exit /b 1 )

if not defined CONFUSER_CLI (
    echo [2/3] 未找到 ConfuserEx，跳过混淆。请将 ConfuserEx 放到 tools\ConfuserEx\ 或 C:\ConfuserEx\
    echo        下载: https://github.com/yck1509/ConfuserEx/releases
    goto :done
)

echo [2/3] 使用 ConfuserEx 混淆主程序 SakuraEDL.dll...
set "RUNTIME_WIN="
set "RUNTIME_CORE="
for /d %%d in ("%ProgramFiles%\dotnet\shared\Microsoft.WindowsDesktop.App\8.0*") do if not defined RUNTIME_WIN set "RUNTIME_WIN=%%d"
for /d %%d in ("%ProgramFiles%\dotnet\shared\Microsoft.NETCore.App\8.0*") do if not defined RUNTIME_CORE set "RUNTIME_CORE=%%d"
set "CRPROJ=%PUBLISH_DIR%\SakuraEDL.crproj"
copy /y "%~dp0SakuraEDL.crproj" "%CRPROJ%" >nul
if defined RUNTIME_WIN if defined RUNTIME_CORE (
  powershell -NoProfile -Command "$c=[System.IO.File]::ReadAllText('%CRPROJ%'); $extra='  <probePath>' + $env:RUNTIME_CORE + '</probePath>' + [char]10 + '  <probePath>' + $env:RUNTIME_WIN + '</probePath>'; $c=$c -replace '<probePath>\.</probePath>', ('<probePath>.</probePath>' + [char]10 + $extra); [System.IO.File]::WriteAllText('%CRPROJ%',$c)"
)
pushd "%PUBLISH_DIR%"
"%CONFUSER_CLI%" SakuraEDL.crproj
popd
if errorlevel 1 ( echo ConfuserEx 运行失败，已保留未混淆的发布文件. )
del "%PUBLISH_DIR%\SakuraEDL.crproj" 2>nul

:done
echo [3/3] 完成.
echo.
echo 输出目录: %CD%\%PUBLISH_DIR%
echo 请勿对 exe 使用 VMProtect 等原生加壳，会无法启动。
echo 若需 .NET 8 混淆，请用支持 .NET 8 的工具：.NET Reactor、Dotfuscator、Eazfuscator.NET。
explorer "%PUBLISH_DIR%" 2>nul
endlocal
