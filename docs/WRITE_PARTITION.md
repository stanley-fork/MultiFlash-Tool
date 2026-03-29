# 写入分区行为（与 SAKURAEDL 对齐说明）

## Rawprogram XML 解析（`edl_rawprogram_parse`）

- 支持 **多行** `<program … />` / `<erase … />` / `<zeroout … />`：属性可换行书写，`physical_partition_number="2"` 等与 LUN 相关的属性若单独占一行，也会被正确合并后再解析（旧版按单行读会漏属性，导致 LUN 恒为 0）。
- 标签须为 **自闭合** `/>`；若使用非自闭合的 `<program>…</program>`，需自行改为一行或自闭合格式。
- **Patch XML**（`edl_patch_parse`）同样支持多行 `<patch … />`。

## 写入前检查（主界面）

- 若某行 **已双击分配镜像**（第 5 列有路径）但 **未勾选**，点击「写入分区」时会在日志中输出 **【检查】…已分配镜像文件但未勾选，本次不会写入**，避免误以为已写入。

## 分区表 LUN 列

- 若 `RoleLun` 未随 GPT 写入（例如手工改表），会回退解析 **LUN 列文本**，支持十进制与 **`0x2` 形式**（即 LUN2）。

## 会话选项 API

- `edl_firehose_set_write_options(ctx, pad_short_image_to_gpt, program_read_back_verify)`
- `edl_service_set_write_options(svc, ...)`（转发到 Firehose）

默认：`pad_short_image_to_gpt = true`（镜像短于 GPT 声明时用 **0 补满** 整段分区）、`program_read_back_verify = false`（主界面写入亦传 **false**）。部分 Firehose/loader 对 `read_back_verify="true"` 行为不一致，易导致写后异常或误判；需要时可自行在代码中改为 `true` 抓包对比。

## program XML（普通分区）

与 SAKURAEDL `FlashPartitionFromFileAsync` 一致：

- **无** `file_sector_offset`（旧版曾带 `file_sector_offset="0"`，部分设备解析路径不同）。
- 属性顺序：`SECTOR_SIZE_IN_BYTES` → `num_partition_sectors` → `physical_partition_number` → `start_sector` → `filename` → `label`（可选 `read_back_verify`）。
- `filename` 与 `label` 同为分区名（转义后）。

仅 `label`、无 `filename` 的旧式格式仍保留在 `include_filename=false` 分支。

## 写入后 fixgpt（主界面勾选）

- **多分区**或**单分区但分区名为 `PrimaryGPT` / `BackupGPT`**：若勾选「写入后执行 fixgpt」，刷写结束后会调用 Firehose fixgpt。
- **仅单分区且为普通分区名**（如 `recovery`、`boot`）：**不执行** fixgpt（避免无谓主备同步），仍会**回读分区表**刷新列表；与常见单镜像写入习惯一致。

## 主界面「仅按镜像长度写入（SAKURAEDL）」

- **勾选**（默认）：`pad_short_image_to_gpt = false`。普通分区在镜像短于 GPT 时，只编程 `ceil(镜像字节/扇区)` 个扇区，**不**向后填 0；与 C# `FlashPartitionFromFileAsync` 按文件长度决定 `num_partition_sectors` 的行为一致，可降低误伤分区尾部导致无法开机。
- **取消勾选**：恢复旧版/rawprogram 习惯：按 GPT 声明扇区数写满，不足处填 0。

**不受「仅镜像长度」影响**：分区名为 `PrimaryGPT` / `BackupGPT` 的写入（仍走完整 GPT 校验、CRC、短镜像拒绝或合并逻辑）。

## 仍无法开机时的排查（非工具单点）

- 镜像与机型/槽位（A/B）/AVB 是否匹配；`recovery`/`boot` 常与 `vbmeta` 等一起刷。
- 扇区大小与存储类型（UFS/eMMC）是否与「读取分区表」时一致。

## 刷写后「能走完流程」但重启又进 EDL（9008）

这通常表示 **BootROM 无法完成可信链加载**（与「仅日志里 setbootable 是否 ACK」无必然关系），常见原因包括：

1. **关键分区未写入**：主界面若某行镜像为 **0 字节**，会 **跳过写入**（不填 0）。若跳过的是 **cdt、sec、xbl、tz、PrimaryGPT** 等，设备可能直接无法启动或断电后再次进 EDL。日志里若出现 **【严重】…0 字节**，必须先补齐对应镜像或从完整线刷包取文件。
2. **镜像与机型/槽位不匹配**：错误 `boot`/`vbmeta` 或错误 GPT 可导致下一级校验失败。
3. **线材/供电**：排除纯硬件导致的异常复位（较少表现为「稳定复现进 EDL」）。

工具侧已把「关键分区 + 0 字节跳过」标为 **红色严重警告**，并在末尾输出汇总行，避免误以为「全部绿色即安全」。

## setbootablestoragedrive（激活可启动存储 / 与抓包对照）

Qualcomm Firehose 在 **configure 成功之后**可发送（与 Bus Hound 等抓包一致）：

```xml
<?xml version="1.0" ?><data><setbootablestoragedrive value="1" /></data>
```

- `value` 为 **物理分区号（LUN）**，常见 UFS 上为 **0/1/2** 等，需按机型与线刷包/官方工具行为选择；**不等同于**「重启到系统」或 `power` 指令。
- 本工具：`edl_xml_build_setbootablestoragedrive` / `edl_service_try_set_bootable_storage_drive` 生成与上式相同的 XML。**分区表「写入分区」**流程末尾**不再**自动调用 setbootable；**完整 rawprogram XML 刷写**请使用 `edl_service_flash_xml_ex` 并传入 `EDL_FLASH_XML_SET_BOOTABLE_AT_END`，且仅当 XML 中 **PROGRAM 任务数 ≥ 20** 时才会在末尾激活，槽位不明时默认倾向 **Boot A**（`edl_service_activate_boot_lun_sakura(..., 1, 0)`）。另提供 **「激活启动分区」** 菜单可手动选 A/B。

## 读取分区表与多 LUN（UFS）

- 每个 **LUN** 单独读 LBA0 起 GPT 区并解析；若某 LUN 的 GPT **条目表为空或极少**（常见于 **LUN2** 等仅元数据槽），仍会在列表中 **自动补充** `PrimaryGPT`、`BackupGPT` 两行，对应盘首/盘尾元数据扇区（与 rawprogram 中 `gpt_main?.bin` / `gpt_backup?.bin` 区域一致），便于刷写与对照官方包。
- 若条目里已存在同名分区，则不会重复插入。
- **分区名**：GPT 条目内为 **UTF-16LE**，界面显示为 **UTF-8**（中文等不再被替换成 `?`）。若条目区很大，会按 GPT 头自动 **扩大读取**（最多约 4096 扇区），避免「只读到头、条目截断→分区缺失」。
- **Configure**：与常见抓包一致使用 **`ZLPAwareHost="1"`**（与旧版 `ZlpAwareHost` 区分）。
- **PROGRAM XML**：`filename`/`label`/`num_partition_sectors` 等顺序与 **Qualcomm rawprogram** 线刷包一致，降低部分 Loader 对属性顺序敏感导致的异常。

## 读取分区保存为 IMG

- **单选分区**：弹出「另存为」，默认扩展名 **`.img`**，可选 `.bin`。
- **多选分区**：选择目录，每个分区保存为 **`分区名.img`**。
