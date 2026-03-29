# 重启菜单说明

## 重启到系统 / 关机

Firehose：`<power value="reset"/>` / `<power value="off"/>`。

## 重启到 Fastboot / Recovery（写 MISC 后重启）

1. 准备厂商要求的 **完整 MISC 分区镜像**（与线刷包或文档一致）。
2. 在 **设置 → MISC 重启镜像** 中填写路径；或把文件放到程序目录并命名为：
   - `misc_fastboot.bin`（Fastboot）
   - `misc_recovery.bin`（Recovery）
3. 连接 EDL 后，在 **重启设备** 菜单选择对应项。工具会：
   - 自动读取 GPT，定位 `misc` / `MISC` 分区；
   - 将镜像 **整分区写入**；
   - 再发送 `power reset`。

若你后续提供专用镜像或偏移规则，可再扩展为「仅写 boot message 区域」等模式。

## 重启到 EDL（9008）

发送 `<power value="download"/>`。**是否进入下载模式取决于设备 Programmer**，部分机型会普通开机；可配合音量键组合或 `adb reboot edl`（需系统侧）。
