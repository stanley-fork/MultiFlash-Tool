# 开发与调试

## 在 Visual Studio 中调试

1. 用 **Visual Studio 2022** 打开 `SakuraEDL.sln`。
2. 在解决方案资源管理器中确认 **SakuraEDL** 为启动项目（名称加粗；若否，右键 SakuraEDL → “设为启动项目”）。
3. 顶部选择 **Debug** 和 **AnyCPU**（或 x64）。
4. 按 **F5** 开始调试，或 **Ctrl+F5** 运行不调试。

解决方案中 SakuraEDL 为第一个项目，首次打开时通常已是默认启动项目，可直接 F5。

## 配置说明

- **SakuraEDL.csproj**：Debug 配置已启用 `Optimize=false`、`DebugType=full`、`DebugSymbols=true`，便于断点与单步调试。
- **Properties/launchSettings.json**：提供启动配置，便于在 VS 中直接运行主程序。
