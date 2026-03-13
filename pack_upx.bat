@echo off
chcp 65001 >nul
setlocal

:: 单文件发布。注意：对生成的 exe 用 VMProtect 等加壳会导致无法打开；需保护请改用 pack_release_protected.bat（多文件+ConfuserEx）
:: UPX 不支持 .NET 托管 exe，仅能压缩原生/NativeAOT 或输出目录中的原生 DLL
set PUBLISH_DIR=bin\Release\net8.0-windows\publish
set "UPX_EXE=C:\Users\Administrator\Desktop\rpg\upx-5.1.0-win64\upx-5.1.0-win64\upx.exe"
if not exist "%UPX_EXE%" (
    set "UPX_EXE=%~dp0tools\upx.exe"
    if not exist "%UPX_EXE%" (
        set "UPX_EXE=upx"
        where upx >nul 2>&1 || set "SKIP_UPX=1"
    )
)
if defined SKIP_UPX echo [INFO] UPX 未找到，将只执行发布，跳过压缩。

echo [0/2] 清理旧输出...
if exist "%PUBLISH_DIR%" rd /s /q "%PUBLISH_DIR%"

echo [1/2] 发布 Release 单文件 exe（不包含运行时，内嵌 Brotli 压缩；目标机需已安装 .NET 8 桌面运行时）...
dotnet publish SakuraEDL.csproj -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "%PUBLISH_DIR%"
if errorlevel 1 ( echo 发布失败. & exit /b 1 )

if defined SKIP_UPX ( echo 跳过 UPX. & goto :done )

echo [2/2] 对发布目录中的可执行文件尝试 UPX 压缩...
for %%f in ("%PUBLISH_DIR%\*.exe") do "%UPX_EXE%" --best --lzma "%%f" 2>nul || echo [SKIP] %%f ^(.NET exe 不被 UPX 支持^)
for %%f in ("%PUBLISH_DIR%\*.dll") do "%UPX_EXE%" --best --lzma "%%f" 2>nul || echo [SKIP] %%f

:done
echo.
echo 输出目录: %CD%\%PUBLISH_DIR%
explorer "%PUBLISH_DIR%" 2>nul
endlocal
