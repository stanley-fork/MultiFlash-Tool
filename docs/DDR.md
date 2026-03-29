# DRAM（DDR）类型说明与实现方式

## 事实：EDL / Firehose 无标准「读 DDR 颗粒型号」

Qualcomm **Firehose** 的 `configure`、`getstorageinfo` 等应答里，**通常不包含** LPDDR4/LPDDR5 或颗粒料号。工具里「存储类型」指的是 **UFS / eMMC**，不是 DRAM。

## 本工具当前实现

### 1）MSM 推断：`edl_guess_ddr_generation(msm_id)`

在 **Sahara 已上报 MSM ID** 的前提下，用 `chip_db` 里的芯片名做 **代际粗推断**，在「读取信息」日志中输出 **「DRAM 代际（推断）」**。

- **不是** SPD、不是丝印、**不能**代表换过内存的维修机。
- 与 `edl_guess_memory_type()` **无关**：后者根据 SoC **猜 UFS 还是 eMMC**，仍不是 DDR。

### 2）Firehose 扩展：`getddrtype`（仅认证设备）

实现为 `edl_firehose_try_get_ddr_type`。**读取分区表成功后自动调用** 默认 **关闭**（`edl_service.c` 中 `EDL_ENABLE_GETDDRTYPE_AFTER_GPT` 为 `0`）：多数旧 Programmer 对 `<getddrtype />` 无应答，仅产生超时日志。需要时在编译前改为 `1` 或 `-DEDL_ENABLE_GETDDRTYPE_AFTER_GPT=1`。

XML 示例：

```xml
<?xml version="1.0" ?><data><getddrtype /></data>
```

启用后仅对 **OPLUS VIP / Realme / OnePlus** 认证会话执行；是否支持取决于 **Loader**。

### 3）其它「真实」板级信息（需自行扩展）

1. **抓包 / 厂商 Loader**  
   若某款程序员在 XML 里多带了 `MemoryType` 等字段：可在 `firehose.c` 里扩展对 `<response>` / `<log>` 的解析。

2. **读 CDT / OEM 分区再解析**  
   部分机型在闪存上有 **CDT** 等分区含内存配置，可用 **read partition** 读出二进制再按机型解析。

3. **开机后系统侧**  
   `dmesg` / `sysfs` 等（已脱离 EDL）。

---

结论：**MSM 推断** 与 **`getddrtype`** 可并存；后者仅在 **VIP/Realme/OnePlus** 连接路径下自动尝试。
