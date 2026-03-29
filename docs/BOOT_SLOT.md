# 启动分区 / setbootablestoragedrive 逻辑说明

## 调用链

1. 主界面勾选 **「刷写完成后激活启动分区」**（仅作用于 **「写 GPT」** rawprogram XML 流程；**不**在「写入分区」单/批量路径自动执行，与 SAKURAEDL 一致）。
2. 程序化：`edl_service_flash_xml_ex` 带 **`EDL_FLASH_XML_SET_BOOTABLE_AT_END`** 时，在 rawprogram+patch+可选 fixgpt 末尾回读 GPT 后调用 `edl_service_activate_boot_lun_sakura()`（见 `edl_service.c`）。
3. `edl_boot_lun_pick_sakura()`（`slot_detect.c`）根据 **存储类型**、**GPT 分区属性**（Android A/B 标志位，GPT 条目 offset 48）与 **本次任务写入的分区名**（`_a` / `_b` 计数）决定目标 LUN。
4. `edl_firehose_try_set_bootable_storage_drive(lun)` 发送  
   `<setbootablestoragedrive value="LUN" />`（见 `xml_helper.c`）。

## fixgpt 与 setbootable

- **`edl_firehose_fix_gpt(..., lun=-1)`**（`lun=all`）**不会**在发送 `<fixgpt />` 之前抢先执行 `setbootablestoragedrive(0)`。历史上若抢先发 LUN0，在部分 UFS 机型上会把可启动路径指错，导致刷机后不开机；fixgpt 本身与「是否可启动」无必然先后关系。

## UFS（常见映射）

- GPT 中 **无任何** `*_a` / `*_b` 后缀分区名时视为 **单槽 / nonexistent**（与 C# `CurrentSlot == nonexistent`），**跳过** setbootable，不按写入任务名回退。
- GPT 多 LUN 合并后槽位 **A** → **LUN1**，槽位 **B** → **LUN2**（与 SAKURAEDL 启发式一致）。
- **不同 OEM / 平台可能不同**；若设备文档另有说明，应以厂商为准。

## eMMC

- 通常为 **LUN0**；无 `_a`/`_b` 分区名时 **跳过** setbootable。

## 槽位推断（`detect_slot_one_lun`）

- 分区名需以 **`_a` / `_b`** 结尾（大小写不敏感）。
- 优先统计 **关键基名**：`boot`、`system`、`vendor`、`abl`、`xbl`、`recovery`、`vbmeta` 等（见 `key_base_match`）。
- 若无关键分区命中，退化为 **所有非排除** 的 A/B 分区（排除如 `vendor_boot`）。
- 使用 GPT **attributes** 中的 Android A/B 位：active、priority、successful、unbootable 等。

## 回退（与写入任务结合）

- **UFS** 且 GPT 中 **已有** `_a`/`_b` 后缀，但多 LUN 合并结果为 **`u` / `?`**（槽位不明）时：按 **本次 XML/任务** 写入统计 **`_a` 多** → LUN1、**`_b` 多** → LUN2；双槽均写则默认 **LUN1**（与 C# undefined/unknown 按写入推断一致）。
- 无 A/B 后缀（上文「单槽」）时 **不会** 进入此回退。

## 日志

- 主日志会输出 **setbootablestoragedrive 是否 ACK**（不再仅 detail）。
- 跳过时会输出原因字符串（如「无 A/B 或无法推断」）。
