# Sahara（9008）握手说明

## 命令模式与「未同步」

默认会先发 **COMMAND** 模式的 `HelloResponse`，尝试读取序列号 / PK / HWID。若设备在约 **20 秒内**未返回 `COMMAND_READY`、或返回其它非预期包（且非 `ReadData` 回灌路径）：

**不能再在同一轮 Hello 里立刻再发 `HelloResponse(传输模式)`** —— 许多机型会随后报 **`EndImageTransfer` 状态 0x01（INVALID_CMD）**。

正确做法是：**`RESET_STATE`（软复位）→ 收一轮新 Hello → 再试一次命令模式**（读出 MSM HWID 等，供 Realme 等认证）；若第二次仍失败，再 **仅发 `HelloResponse(传输模式)`** 上传 Loader。RESET 后会 **排空 RX**，并 **等待约 800ms** 再读 Hello。

## 若仍出现「设备无响应」

1. **核对 Loader**：与芯片/机型匹配；错 Loader 易导致后续握手失败。  
2. **CMake** `-DEDL_SAHARA_COMMAND_MODE=OFF`：跳过命令模式，仅上传 Loader。  
3. **物理层**：换 USB 口/线；多次失败后 **拔插设备** 或 **重新进入 EDL**。

