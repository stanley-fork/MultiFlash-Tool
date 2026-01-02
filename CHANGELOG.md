# 更新日志

所有重要的项目变更都将记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [2.0.0] - 2026-01-02

### 🎉 重大更新

本版本带来了大量新功能和优化，包括 OFP/OZIP/OPS 固件解密、智能刷机服务、设备信息读取等。

### ✨ 新增功能

#### OFP/OZIP/OPS 固件解密 (`OFPDecryptor.cs`)
- 🔐 支持 OPPO OFP 固件包自动解密
- 🔐 支持 OZIP 加密 ZIP 包解密
- 🔐 支持 OnePlus OPS 固件包解密
- 🔐 智能检测固件类型 (Qualcomm/MTK)
- 🔐 内置 50+ 组已知解密密钥
- 🔑 **智能密钥爆破** (`OFPKeyBruteForce.cs`)
  - 基于已知模板的变体爆破
  - 简单密钥变体爆破
  - 增量字节爆破
  - 随机密钥爆破
  - 支持 MTK Shuffle 算法

#### 智能刷机服务 (`SmartFlashService.cs`)
- 🚀 一键智能刷机流程
- 🔄 自动设备检测和连接
- 📡 自动 Sahara 握手和 Loader 匹配
- ⚙️ 自动 Firehose 配置 (UFS/eMMC)
- 📊 自动读取 GPT 分区表
- ✅ 分区验证和写入
- 📝 详细的用户指引和错误提示

#### 设备信息读取 (`DeviceInfoReader.cs`)
- 📱 从 Firehose 模式读取 `build.prop`
- 📁 支持多分区读取 (super, system, vendor)
- 📂 支持多文件系统 (EXT4, EROFS, F2FS, SquashFS)
- 📦 支持 Sparse 镜像自动转换
- 🗄️ 支持 LP (Logical Partition) 容器解析

#### Firehose 协议增强 (`FirehoseEnhanced.cs`, `FirehoseProtocol.cs`)
- ⚡ 智能存储类型检测 (eMMC/UFS/NAND)
- 🔄 自动扇区大小适配
- 📈 高速并行分区读写
- 🔁 失败自动重试机制
- 📊 详细的响应解析和日志

#### Sahara 协议改进 (`SaharaProtocol.cs`)
- 🤝 智能握手流程 (`SmartHandshakeAsync`)
- 📋 自动读取设备信息 (Serial, HWID, PK Hash)
- 🔍 V2/V3 协议版本自动检测
- 📂 Loader 自动匹配或用户指引
- ⚠️ 安全退出 Sahara 模式

#### 认证策略系统 (`Authentication/`)
- 🔐 标准认证策略 (`StandardAuthStrategy.cs`)
- 🔐 OPPO VIP 认证策略 (`OppoVipDeviceStrategy.cs`)
- 🔐 小米 MiAuth 认证策略 (`XiaomiAuthStrategy.cs`)
  - 内置 5 组签名盲测
  - Blob 获取和手动签名支持
  - Demacia/SwProject/Project 协议

#### 分区管理增强 (`GptParser.cs`, `XmlPartitionParser.cs`)
- 📊 分区表管理器 (`PartitionTableManager`)
- 📂 支持从 XML/GPT/设备 加载分区表
- 🎨 分区风险等级颜色标识
  - 🔴 关键分区 (xbl, abl, aop)
  - 🟠 危险分区 (boot, recovery)
  - 🟡 系统分区 (system, vendor)
  - 🟢 用户数据分区
- 📝 文件系统类型检测显示

#### 日志系统 (`PrettyLogger.cs`)
- 🎨 格式化彩色日志输出
- 📊 设备信息表格化显示
- 📈 进度条和状态指示
- ⚠️ 分级警告和错误提示

#### 其他新增
- 🖼️ Sparse/Raw 镜像处理 (`SparseImageHandler.cs`)
- 🗄️ Qualcomm 设备数据库 (`QualcommDatabase.cs`)
- 🔧 设备识别服务 (`DeviceIdentifier.cs`)
- ⚙️ 配置管理器 (`ConfigManager.cs`)
- 🔄 自动更新检查 (`AutoUpdater.cs`)

### 🔧 优化改进

#### UI 优化
- 📊 分区列表增加 FS (文件系统) 和 Fmt (镜像格式) 列
- 🎨 分区按风险等级显示不同颜色
- 📏 列宽和顺序优化
- ✅ 复选框列宽增加

#### 性能优化
- ⚡ 异步操作全面优化
- 🔄 重试机制减少失败率
- 📊 大文件分块处理
- 💾 内存使用优化

### 🐛 修复

- 修复从 XML 加载分区后无法读写的问题
- 修复文件系统检测不完整的问题
- 修复 Sparse 镜像检测失败的问题
- 修复 super.img LP 容器解析问题
- 修复 UI 列顺序显示错误
- 修复 OFP MTK 类型检测问题

### 📝 文档

- 📖 更新 README.md 新功能说明
- 📖 更新开发文档
- 📖 添加功能详细说明

---

## [1.0.0] - 2024-12-13

### 新增
- ✨ EDL 模式刷写功能
  - Sahara 协议支持
  - Firehose 协议刷写
  - GPT 分区表备份/恢复
  - 内存读写操作
  
- ✨ Fastboot 增强功能
  - 分区读写操作
  - OEM 解锁/重锁
  - 设备信息查询
  - 自定义命令执行

- ✨ 固件工具
  - Payload.bin 提取
  - Super 分区合并
  - 稀疏镜像处理
  - 分区镜像提取

- ✨ 设备管理
  - 自动设备检测
  - 实时状态监控
  - 多设备支持

- ✨ 用户界面
  - 基于 AntdUI 的现代化界面
  - 详细日志输出
  - 操作进度显示

### 安全
- 🔐 云端授权验证系统
- 🔐 配置文件加密存储

### 文档
- 📝 完整的 README.md
- 📝 开发指南 DEVELOPMENT.md
- 📝 贡献指南 CONTRIBUTING.md
- 📝 非商业许可证

---

## 版本说明

### 版本号格式
- **主版本号**: 不兼容的 API 修改
- **次版本号**: 向下兼容的功能性新增
- **修订号**: 向下兼容的问题修正

### 变更类型
- `新增` - 新功能
- `变更` - 现有功能的变更
- `弃用` - 即将移除的功能
- `移除` - 已移除的功能
- `修复` - Bug 修复
- `安全` - 安全相关的修复
- `文档` - 文档更新
- `性能` - 性能优化

---

<div align="center">
  
  [2.0.0]: https://github.com/xiriovo/MultiFlash-Tool/compare/v1.0.0...v2.0.0
  [1.0.0]: https://github.com/xiriovo/MultiFlash-Tool/releases/tag/v1.0.0
  
</div>
