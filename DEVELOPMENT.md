<div align="center">
  <img src="assets/logo.jpg" alt="MultiFlash Tool Logo" width="200"/>
  
  # MultiFlash Tool 开发者指南
  
  **项目架构、开发规范与最佳实践**
  
</div>

---

> ⚠️ **注意**: 本项目采用非商业许可，禁止任何形式的商业用途。

## 📖 目录

- [项目结构](#项目结构)
- [Form1.cs 模块划分](#form1cs-模块划分)
- [编码规范](#编码规范)
- [依赖包](#依赖包)
- [开发环境设置](#开发环境设置)
- [新功能开发流程](#新功能开发流程)
- [测试指南](#测试指南)
- [注意事项](#注意事项)

## 项目结构

```
MultiFlash Tool/
├── Form1.cs              # 主窗体 (7个功能模块)
├── Form1.Designer.cs     # 窗体设计器代码
├── DeviceManager.cs      # 设备管理器
├── GptParser.cs          # GPT 分区解析和管理
├── XmlPartitionParser.cs # XML 分区配置解析
│
├── Services/             # 服务层
│   ├── AppSettings.cs    # 应用配置
│   ├── ConfigManager.cs  # 配置管理器
│   ├── SmartFlashService.cs # 🆕 智能刷机服务
│   ├── FlashTaskExecutor.cs # 刷写任务执行器
│   ├── PrettyLogger.cs   # 🆕 格式化日志系统
│   ├── DependencyManager.cs # 依赖管理
│   ├── AutoUpdater.cs    # 自动更新检查
│   └── SecurityService.cs # 安全服务
│
├── Qualcomm/             # 高通协议实现
│   ├── AutoFlasher.cs    # 自动刷机流程
│   ├── SaharaProtocol.cs # Sahara 协议 (V2/V3)
│   ├── FirehoseProtocol.cs # Firehose 协议
│   ├── FirehoseEnhanced.cs # 🆕 Firehose 增强
│   ├── OFPDecryptor.cs   # 🆕 OFP/OZIP/OPS 解密器
│   ├── OFPKeyBruteForce.cs # 🆕 密钥爆破器
│   ├── DeviceInfoReader.cs # 🆕 设备信息读取
│   ├── SparseImageHandler.cs # 🆕 Sparse/Raw 镜像处理
│   ├── QualcommDatabase.cs # 🆕 设备数据库
│   ├── DeviceIdentifier.cs # 🆕 设备识别
│   ├── XiaomiAuth.cs     # 🆕 小米认证
│   └── PartitionInfo.cs  # 分区信息
│
├── Authentication/       # 认证策略
│   ├── IAuthStrategy.cs  # 认证策略接口
│   ├── DefaultVipStrategy.cs # 默认 VIP 策略
│   ├── StandardAuthStrategy.cs # 🆕 标准认证
│   └── XiaomiAuthStrategy.cs # 🆕 小米认证策略
│
├── Strategies/           # 设备策略
│   ├── IDeviceStrategy.cs # 设备策略接口
│   ├── StandardDeviceStrategy.cs # 标准设备
│   ├── OppoVipDeviceStrategy.cs # OPPO VIP 设备
│   └── XiaomiDeviceStrategy.cs # 小米设备
│
├── FastbootEnhance/      # Fastboot 增强
│   ├── Payload.cs        # Payload 解析
│   └── UpdateMetadata.cs # OTA 元数据
│
├── Localization/         # 多语言支持
│   ├── LanguageManager.cs
│   └── LocalizationManager.cs
│
└── Properties/           # 项目属性
    ├── Resources.resx    # 资源文件
    ├── Resources.zh-CN.resx # 中文资源
    └── Resources.en.resx # 英文资源
```

## Form1.cs 模块划分

| #region | 功能描述 |
|---------|---------|
| 日志功能 | 日志输出、文件记录 |
| 菜单初始化 | EDL 菜单事件绑定 |
| EDL 高级功能 | GPT 备份/恢复、内存操作 |
| 设备重启事件 | 重启到系统/Recovery/Fastboot |
| 文件选择事件 | 文件对话框处理 |
| 刷写操作 | 分区读写、刷机流程 |
| Payload 操作 | Payload 提取、Super 合并 |

## 编码规范

### 异常处理
```csharp
// 使用 SafeExecuteAsync 包装 async void 事件处理程序
SafeExecuteAsync(async () => {
    await SomeAsyncOperation();
}, "操作名称");

// 避免空 catch，添加日志
catch (Exception ex)
{
    System.Diagnostics.Debug.WriteLine($"[模块名] 操作失败: {ex.Message}");
}
```

### 配置管理
```csharp
// 使用 AppSettings 读取配置
var apiUrl = AppSettings.Instance.UpdateApiUrl;
var rsaKey = AppSettings.Instance.RsaPublicKey;
```

### 日志输出
```csharp
AppendLog("消息", Color.Black);   // 一般日志
AppendLog("成功", Color.Green);   // 成功
AppendLog("警告", Color.Orange);  // 警告
AppendLog("错误", Color.Red);     // 错误
```

## 🆕 核心模块说明

### OFPDecryptor - OFP/OZIP/OPS 解密器

支持解密 OPPO、OnePlus、Realme 的加密固件包。

```csharp
// 智能解密（自动检测类型）
await OFPDecryptor.SmartExtractAsync(firmwarePath, outputDir, ct);

// 单独解密 OFP
await OFPDecryptor.ExtractOFPAsync(ofpPath, outputDir, ct);

// 解密 OZIP
await OFPDecryptor.ExtractOZIPAsync(ozipPath, outputDir, ct);

// 解密 OPS
await OFPDecryptor.ExtractOPSAsync(opsPath, outputDir, ct);
```

支持的加密类型：
- Qualcomm OFP (AES-128-CFB)
- MTK OFP (AES-128-CFB + Shuffle)
- OZIP (AES-128-ECB)
- OPS (AES-128-ECB)

### SmartFlashService - 智能刷机服务

一键完成从连接到刷写的完整流程。

```csharp
var config = new SmartFlashConfig
{
    PortName = "COM3",
    LoaderPath = "prog_firehose.mbn",
    FirmwareDirectory = "C:\\Firmware",
    AuthType = AuthType.Standard
};

var result = await SmartFlashService.ExecuteSmartFlashAsync(config, progress, ct);
```

### DeviceInfoReader - 设备信息读取

从 EDL 模式读取 build.prop 信息。

```csharp
var reader = new DeviceInfoReader(firehoseClient, logger);
var props = await reader.ReadBuildPropsAsync(ct);

// 结果包含：
// - ro.product.model
// - ro.build.version.release
// - ro.build.version.security_patch
// 等
```

### OFPKeyBruteForce - 密钥爆破器

当标准密钥无法解密时，启动智能爆破。

```csharp
var bruteForce = new OFPKeyBruteForce(ofpPath, pageSize, logger);
var result = await bruteForce.BruteForceAsync(ct, progress);

if (result.Success)
{
    Console.WriteLine($"找到密钥: {result.Key}");
    Console.WriteLine($"IV: {result.Iv}");
}
```

爆破模式：
1. 基于已知模板的变体
2. 简单密钥变体
3. 增量字节爆破
4. 随机密钥爆破

## 依赖包

| 包名 | 版本 | 用途 |
|------|------|------|
| AntdUI | 2.2.1 | UI 框架 |
| SharpZipLib | 1.4.2 | Zip/BZip2 压缩 |
| System.Text.Json | 8.0.5 | JSON 序列化 |
| Newtonsoft.Json | 13.0.4 | JSON 兼容 |
| Google.Protobuf | 3.17.3 | Protobuf 支持 |
| System.IO.Ports | 8.0.0 | 串口通信 |

## 🚀 开发环境设置

### 必需工具
1. **Visual Studio 2022** (推荐) 或 Visual Studio 2019
   - 工作负载: .NET 桌面开发
   - 组件: .NET Framework 4.8 SDK

2. **Git** - 版本控制
3. **NuGet** - 包管理器（VS 内置）

### 环境配置

1. **克隆仓库**
   ```bash
   git clone https://github.com/yourusername/MultiFlash-Tool.git
   cd MultiFlash-Tool
   ```

2. **还原 NuGet 包**
   ```bash
   nuget restore "MultiFlash Tool.sln"
   ```
   或在 Visual Studio 中右键解决方案 → "还原 NuGet 包"

3. **配置文件**
   - 复制 `config.json.example` 为 `config.json`
   - 填写必要的配置项（API 密钥等）

4. **编译项目**
   ```bash
   msbuild "MultiFlash Tool.sln" /p:Configuration=Release
   ```
   或在 Visual Studio 中按 `Ctrl+Shift+B`

## 新功能开发流程

### 1. 创建功能分支
```bash
git checkout -b feature/your-feature-name
```

### 2. 定位代码位置
- 在对应的 `#region` 中添加方法
- 如需新模块，在 `Services/` 或 `Strategies/` 下创建

### 3. 实现功能
```csharp
// 事件处理程序使用 SafeExecuteAsync 包装
private void Button_Click(object sender, EventArgs e)
{
    SafeExecuteAsync(async () =>
    {
        AppendLog("开始操作...", Color.Black);
        
        // 你的异步代码
        await YourAsyncMethod();
        
        AppendLog("操作完成", Color.Green);
    }, "操作名称");
}
```

### 4. 添加日志
```csharp
AppendLog("信息", Color.Black);   // 一般信息
AppendLog("成功", Color.Green);   // 成功操作
AppendLog("警告", Color.Orange);  // 警告信息
AppendLog("错误", Color.Red);     // 错误信息
```

### 5. 测试功能
- 单元测试（如适用）
- 手动测试各种场景
- 检查日志输出

### 6. 提交代码
```bash
git add .
git commit -m "Add: 添加某功能"
git push origin feature/your-feature-name
```

### 7. 创建 Pull Request
- 填写 PR 描述
- 关联相关 Issue
- 等待代码审查

## 🧪 测试指南

### 手动测试清单

#### EDL 模式测试
- [ ] 设备检测
- [ ] Programmer 加载
- [ ] 分区读取
- [ ] 分区写入
- [ ] GPT 备份/恢复

#### Fastboot 模式测试
- [ ] 设备检测
- [ ] 分区列表获取
- [ ] 分区读写
- [ ] OEM 解锁/重锁
- [ ] 设备重启

#### 固件工具测试
- [ ] Payload 提取
- [ ] Super 合并
- [ ] 稀疏镜像处理

### 调试技巧

1. **启用详细日志**
   ```csharp
   System.Diagnostics.Debug.WriteLine($"[模块] 调试信息: {value}");
   ```

2. **使用断点**
   - 在关键位置设置断点
   - 检查变量值和调用堆栈

3. **查看日志文件**
   - 日志位置: `logs/` 目录
   - 实时查看: `tail -f logs/latest.log`

## ⚠️ 注意事项

### 代码质量

- ✅ **Form1.cs 很大** - 修改前先定位到对应的 #region
- ✅ **async void** - 仅用于事件处理程序，使用 SafeExecuteAsync
- ✅ **Thread.Sleep** - 仅在串口通信等后台线程中使用
- ✅ **配置** - 敏感信息存放在 config.json，不要硬编码
- ✅ **异常处理** - 总是捕获并记录异常
- ✅ **资源释放** - 使用 `using` 语句或手动释放

### 性能优化

- 避免在 UI 线程执行耗时操作
- 使用 `async/await` 进行异步操作
- 大文件操作使用流式处理
- 及时释放不再使用的资源

### 安全考虑

- 不要在代码中硬编码密钥
- 验证用户输入
- 使用安全的加密算法
- 定期更新依赖包

## 📚 参考资源

### 官方文档
- [.NET Framework 文档](https://docs.microsoft.com/dotnet/framework/)
- [C# 编程指南](https://docs.microsoft.com/dotnet/csharp/)
- [AntdUI 文档](https://gitee.com/antdui/AntdUI)

### 高通协议
- Sahara 协议规范
- Firehose 协议规范
- EDL 模式文档

### 工具
- [dnSpy](https://github.com/dnSpy/dnSpy) - .NET 反编译器
- [ILSpy](https://github.com/icsharpcode/ILSpy) - .NET 反编译器
- [Fiddler](https://www.telerik.com/fiddler) - HTTP 调试工具

## 🤝 获取帮助

遇到问题？

1. 查看 [常见问题](README.md#常见问题)
2. 搜索 [已有 Issues](../../issues)
3. 创建 [新 Issue](../../issues/new)
4. 加入 [讨论区](../../discussions)

---

<div align="center">
  Happy Coding! 🎉
</div>
