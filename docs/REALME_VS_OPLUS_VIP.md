# Realme 云端签名 vs OPLUS VIP

| 项目 | Realme（勾选「Realme」） | OPLUS VIP（勾选「OPLUS VIP」） |
|------|-------------------------|--------------------------------|
| 凭据 | **RCSMAUTH**：`realme/rcsmAuthAccount` + `realme/rcsmAuthKey`（或 `token` + `account`） | 本地 **Digest** + **Signature** 两个文件 |
| 流程 | Firehose `getsigndata` → 应用请求云端 → `verify`（EnableVip=0）+ 签名数据 | Digest 二进制 → TransferCfg → Verify(EnableVip=1) → 发 Signature → … |
| 与谁对应 | 桌面 C# `RealmeSignService` / RCSM API | Sakura 类 VIP 文件串口流程 |

**勿混用**：Realme 机选 Realme 并配 RCSM；需要 VIP 文件认证时只选 OPLUS VIP，不要把 VIP 的 Digest/Signature 当成 Realme 云端账号。

## 主界面

- **项目号** 仅在勾选 **Realme** 时显示，位置在 **Digest 行下方、Signature 行上方**，**必填**；连接前写入 `realme/projectNumber`。
- **RCSMAUTH 账号与 Auth Key** 已在程序内作为默认值（仍可用 QSettings 覆盖）。

## QSettings（Windows 常为注册表）

组织名与应用名均为：`SAKURAEDL`（由 `QCoreApplication::setOrganizationName` / `setApplicationName` 设置，`QSettings` 默认作用域）。

- `realme/projectNumber` — 与主界面「项目号」同步（必填）  
- `realme/rcsmAuthAccount` / `realme/rcsmAuthKey` — 可选覆盖内置 RCSMAUTH  
- `realme/signApiUrl` — 签名 API（可选，有默认）
