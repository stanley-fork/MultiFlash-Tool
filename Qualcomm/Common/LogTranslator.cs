using System;
using System.Collections.Generic;
using System.Linq;
using SakuraEDL.Common;

namespace SakuraEDL.Qualcomm.Common
{
    /// <summary>
    /// Intercepts Chinese log messages and translates them based on LanguageManager.CurrentLanguage.
    /// Two-tier strategy: exact dictionary first, then ordered phrase replacement fallback.
    /// </summary>
    public static class LogTranslator
    {
        private static readonly Dictionary<string, string> ExactMap;
        private static readonly List<KeyValuePair<string, string>> PhraseMap;

        static LogTranslator()
        {
            ExactMap = BuildExactMap();
            PhraseMap = BuildPhraseMap();
        }

        public static string Translate(string msg)
        {
            if (string.IsNullOrEmpty(msg)) return msg;
            if (LanguageManager.CurrentLanguage == "zh") return msg;

            if (ExactMap.TryGetValue(msg, out var exact))
                return exact;

            string result = msg;
            foreach (var kv in PhraseMap)
            {
                if (result.Contains(kv.Key))
                    result = result.Replace(kv.Key, kv.Value);
            }
            return result;
        }

        #region Exact Message Dictionary

        private static Dictionary<string, string> BuildExactMap()
        {
            return new Dictionary<string, string>
            {
                // ==================== qualcomm_service.cs ====================
                // --- Connection / Port ---
                { "等待高通 EDL USB 设备 : 成功", "Waiting for Qualcomm EDL USB device : OK" },
                { "正在连接设备 : 成功", "Connecting to device : OK" },
                { "[高通] 连接成功", "[Qualcomm] Connected" },
                { "[高通] Firehose 直连成功", "[Qualcomm] Firehose direct connection OK" },
                { "[高通] VIP Loader 连接成功", "[Qualcomm] VIP Loader connected" },
                { "[高通] 检测到设备断开", "[Qualcomm] Device disconnected" },
                { "[高通] 端口已从系统中移除", "[Qualcomm] Port removed from system" },
                { "[高通] 端口连接验证失败", "[Qualcomm] Port connection verification failed" },
                { "[高通] 多次超时，尝试重置连接...", "[Qualcomm] Multiple timeouts, attempting to reset connection..." },
                { "[高通] VIP 认证失败", "[Qualcomm] VIP authentication failed" },
                { "[高通] VIP 认证需要 Digest 和 Signature 文件", "[Qualcomm] VIP authentication requires Digest and Signature files" },
                { "[高通] 小米认证失败", "[Qualcomm] Xiaomi authentication failed" },
                { "无法打开端口", "Unable to open port" },
                { "无法重新打开端口", "Unable to reopen port" },
                { "连接成功", "Connection successful" },
                { "连接失败", "Connection failed" },

                // --- Firehose Configure ---
                { "正在配置 Firehose...", "Configuring Firehose..." },
                { "配置 Firehose : 成功", "Configure Firehose : OK" },
                { "配置 Firehose : 失败", "Configure Firehose : Failed" },
                { "正在发送 Firehose 引导文件 : 成功", "Sending Firehose boot file : OK" },

                // --- Realme Auth ---
                { "[Realme] 进入 Firehose 后先发送 nop...", "[Realme] Sending nop before Firehose..." },
                { "[Realme] Firehose nop 成功", "[Realme] Firehose nop OK" },
                { "[Realme] 配置 Firehose...", "[Realme] Configuring Firehose..." },
                { "[Realme] 配置 Firehose (legacy loader)...", "[Realme] Configuring Firehose (legacy loader)..." },
                { "[Realme] 配置 Firehose (simplified)...", "[Realme] Configuring Firehose (simplified)..." },
                { "[Realme] 查询设备存储 diskId...", "[Realme] Querying device storage diskId..." },
                { "[Realme] initdigest 成功", "[Realme] initdigest OK" },
                { "[Realme] initdigest 失败", "[Realme] initdigest failed" },
                { "[Realme] legacy configure 失败", "[Realme] Legacy configure failed" },
                { "[Realme] simplified configure 失败", "[Realme] Simplified configure failed" },
                { "[Realme] configure 成功", "[Realme] Configure OK" },
                { "[Realme] configure 无响应", "[Realme] Configure no response" },
                { "[Realme] configure 被设备拒绝", "[Realme] Configure rejected by device" },
                { "[Realme] Firehose 已配置，但 modern Realme 认证未通过", "[Realme] Firehose configured but modern Realme auth failed" },
                { "[Realme] Firehose 已配置，但 legacy Realme 认证未通过", "[Realme] Firehose configured but legacy Realme auth failed" },
                { "[Realme] Firehose 已配置，但 simplified Realme 认证未通过", "[Realme] Firehose configured but simplified Realme auth failed" },
                { "[Realme] 设备侧 verify 已通过，认证成功", "[Realme] Device-side verify passed, auth OK" },
                { "[Realme] 设备侧 verify 已通过", "[Realme] Device-side verify passed" },
                { "[Realme] 认证后 GPT 回读验证通过", "[Realme] Post-auth GPT readback verification passed" },
                { "[Realme] GPT 回读验证通过", "[Realme] GPT readback verification passed" },
                { "[Realme] GPT 回读验证未通过，但 verify 已成功，继续", "[Realme] GPT readback verification failed, but verify succeeded, continuing" },
                { "[Realme] GPT 回读验证超时，但 verify 已成功，继续", "[Realme] GPT readback verification timeout, but verify succeeded, continuing" },
                { "[Realme] 所有 LUN 读取均失败", "[Realme] All LUN reads failed" },
                { "[Realme] Firehose 未连接", "[Realme] Firehose not connected" },
                { "[Realme] 获取签名材料失败", "[Realme] Failed to obtain signing material" },
                { "[Realme] 签名接口返回空数据", "[Realme] Signing interface returned empty data" },
                { "[Realme] 云端签名认证已取消", "[Realme] Cloud signing auth cancelled" },
                { "[Realme] 缺少 Digest 文件", "[Realme] Missing Digest file" },
                { "[Realme] 缺少 Digest 数据", "[Realme] Missing Digest data" },
                { "[Realme] 缺少 ProjectID", "[Realme] Missing ProjectID" },
                { "[Realme] 保持端口不关闭，避免 USB 端点重置", "[Realme] Keeping port open to avoid USB endpoint reset" },
                { "[Realme] Modern: 端口未就绪，跳过 DigestsToSign 发送", "[Realme] Modern: Port not ready, skipping DigestsToSign transfer" },
                { "[Realme] Modern: 未找到 DigestsToSign 文件，跳过 Sahara 后原始二进制发送", "[Realme] Modern: DigestsToSign file not found, skipping raw binary transfer after Sahara" },
                { "[Realme] Modern: VIP 认证可能因缺少分区信息而失败", "[Realme] Modern: VIP auth may fail due to missing partition info" },
                { "[Realme] Modern: DigestsToSign 原始二进制已发送", "[Realme] Modern: DigestsToSign raw binary sent" },
                { "[Realme] Modern: DigestsToSign 发送失败", "[Realme] Modern: DigestsToSign transfer failed" },
                { "[Realme] Modern: 发送 nop 并排空设备日志...", "[Realme] Modern: Sending nop and draining device logs..." },
                { "[Realme] Modern: 设备日志已排空", "[Realme] Modern: Device logs drained" },
                { "[Realme] nop 响应中检测到 initdigest 支持，回退到 Legacy 流程", "[Realme] initdigest support detected in nop response, falling back to Legacy flow" },
                { "[Realme] Legacy: nop 已在 Modern 检测中发送，跳过", "[Realme] Legacy: nop already sent during Modern detection, skipping" },
                { "[Realme] Simplified: nop 已在 Modern 检测中发送，跳过", "[Realme] Simplified: nop already sent during Modern detection, skipping" },
                { "[Realme] Simplified: 警告 - 未从 programmer ELF 提取到 hash segment，设备可能拒绝后续命令", "[Realme] Simplified: Warning - no hash segment extracted from programmer ELF, device may reject subsequent commands" },
                { "[Realme] Simplified: programmer hash segment 发送失败，尝试继续", "[Realme] Simplified: programmer hash segment send failed, attempting to continue" },
                { "[Realme] configure 响应中检测到 initdigest 支持，升级为完整 Legacy 流程", "[Realme] initdigest support detected in configure response, upgrading to full Legacy flow" },
                { "[Realme] 获取签名材料 (legacy getsigndata)...", "[Realme] Obtaining signing material (legacy getsigndata)..." },
                { "[Realme] 请求云端签名...", "[Realme] Requesting cloud signature..." },
                { "[Realme] 执行云端签名认证...", "[Realme] Performing cloud signing auth..." },
                { "[Realme] 已获取签名", "[Realme] got signature" },
                { "[Sahara] 加载引导", "[Sahara] Loading boot" },

                // --- Realme Cloud (Form1) ---
                { "[Realme云端] 正在下载 Loader...", "[Realme Cloud] Downloading Loader..." },
                { "[Realme云端] 正在下载 Digest...", "[Realme Cloud] Downloading Digest..." },
                { "[Realme云端] Digest 下载完成", "[Realme Cloud] Digest download complete" },
                { "[Realme云端] Loader 下载完成 ({0} KB)", "[Realme Cloud] Loader download complete ({0} KB)" },
                { "[Realme云端] Loader 下载失败", "[Realme Cloud] Loader download failed" },
                { "[Realme云端] Digest 下载失败，无法继续连接。请检查网络或联系管理员。", "[Realme Cloud] Digest download failed, cannot continue. Check network or contact admin." },

                // --- Device info / GPT read (QualcommUIController, etc.) ---
                { "设备信息解析完成", "device info parsing complete" },
                { "设备信息解析完成 ({0:F1}s)", "device info parsing complete ({0:F1}s)" },
                { "detected system partition，开始readdevice info...", "detected system partition, starting read device info..." },
                { "从 odm 解析OK", "odm parsed OK" },
                { "从 odm 解析成功", "odm parsed OK" },
                { "  从 {0} 解析成功", "  {0} parsed OK" },

                // --- Cloud ---
                { "[云端] 获取设备信息...", "[Cloud] Getting device info..." },
                { "[云端] 无法获取设备信息", "[Cloud] Unable to get device info" },
                { "[云端] 设备信息获取成功", "[Cloud] Device info obtained" },
                { "[云端] Loader 上传失败", "[Cloud] Loader upload failed" },
                { "[云端] 执行小米认证...", "[Cloud] Performing Xiaomi auth..." },
                { "[云端] 小米认证成功", "[Cloud] Xiaomi auth OK" },
                { "[云端] 小米认证失败", "[Cloud] Xiaomi auth failed" },
                { "[云端] 执行 OnePlus 认证...", "[Cloud] Performing OnePlus auth..." },
                { "[云端] OnePlus 认证成功", "[Cloud] OnePlus auth OK" },
                { "[云端] OnePlus 认证失败", "[Cloud] OnePlus auth failed" },
                { "[云端] 该 Loader 没有 VIP 验证文件，将以普通模式连接", "[Cloud] This Loader has no VIP verification files, connecting in normal mode" },
                { "[云端] 请先调用 GetSaharaDeviceInfoOnlyAsync", "[Cloud] Please call GetSaharaDeviceInfoOnlyAsync first" },

                // --- Watchdog / Timeout ---
                { "看门狗超时", "Watchdog timeout" },
                { "关闭端口异常", "Port close exception" },
                { "释放端口异常", "Port release exception" },
                { "释放 Firehose 异常", "Firehose release exception" },

                // ==================== firehose_client.cs ====================
                { "[Firehose] 配置设备...", "[Firehose] Configuring device..." },
                { "[Firehose] 配置超时，设备可能不在 Firehose 模式", "[Firehose] Configuration timeout, device may not be in Firehose mode" },
                { "[Firehose] 读取已取消", "[Firehose] Read cancelled" },
                { "[Firehose] 写入已取消", "[Firehose] Write cancelled" },
                { "[Firehose] 数据写入失败", "[Firehose] Data write failed" },
                { "[Firehose] programcust 命令未确认", "[Firehose] programcust command not acknowledged" },
                { "镜像文件不存在", "Image file does not exist" },
                { "分段大小不能为负数", "Segment size cannot be negative" },
                { "[Firehose] GPT 修复成功", "[Firehose] GPT repair OK" },
                { "[Firehose] setactiveslot 命令成功", "[Firehose] setactiveslot command OK" },

                // ==================== sahara_protocol.cs ====================
                { "[Sahara] 获取设备信息 (不上传 Loader)...", "[Sahara] Getting device info (without uploading Loader)..." },
                { "[Sahara] 无法接收 Hello 包", "[Sahara] Unable to receive Hello packet" },
                { "[Sahara] 设备无响应", "[Sahara] Device not responding" },
                { "[Sahara] Loader 数据为空", "[Sahara] Loader data is empty" },
                { "[Sahara] 芯片信息不完整", "[Sahara] Chip info incomplete" },
                { "[Sahara] 设备接受命令模式", "[Sahara] Device accepted command mode" },
                { "[Sahara] 命令模式无响应", "[Sahara] Command mode no response" },
                { "[Sahara] 设备拒绝命令模式", "[Sahara] Device rejected command mode" },
                { "[Sahara] 跳过命令模式", "[Sahara] Skipping command mode" },
                { "[Sahara] 切换回传输模式...", "[Sahara] Switching back to transfer mode..." },
                { "[Sahara] 看门狗检测到卡死", "[Sahara] Watchdog detected hang" },
                { "[Sahara] 看门狗触发自动重置...", "[Sahara] Watchdog triggered auto-reset..." },
                { "[Sahara] 设备状态异常 (收到 EndImageTransfer)，需要重置", "[Sahara] Device state abnormal (received EndImageTransfer), reset needed" },
                { "[Sahara] 发送 ResetStateMachine (与官方工具流程一致)", "[Sahara] Sending ResetStateMachine (matches official tool flow)" },
                { "[Sahara] 发送 HelloResponse (传输模式)", "[Sahara] Sending HelloResponse (transfer mode)" },
                { "引导 文件不存在", "Boot file does not exist" },
                { "引导数据为空", "Boot data is empty" },

                // ==================== device_info_service.cs ====================
                { "解析 build.prop 失败", "Failed to parse build.prop" },
                { "文件不存在", "File does not exist" },
                { "无法读取 LP Geometry", "Unable to read LP Geometry" },
                { "无效的 LP Geometry magic", "Invalid LP Geometry magic" },
                { "无法找到有效的 LP Metadata Header", "Unable to find valid LP Metadata Header" },
                { "数据太短", "Data too short" },
                { "正在从 Super 分区逻辑卷解析 build.prop...", "Parsing build.prop from Super partition logical volume..." },
                { "从 Super 分区读取超时 (30秒)，跳过", "Super partition read timeout (30s), skipping" },
                { "正在扫描物理分区以提取 build.prop...", "Scanning physical partitions to extract build.prop..." },

                // ==================== xiaomi_auth_strategy.cs ====================
                { "[小米认证] 未找到令牌文件，请求用户输入...", "[Xiaomi Auth] Token file not found, requesting user input..." },
                { "[小米认证] 用户未提供令牌", "[Xiaomi Auth] User did not provide token" },
                { "[小米认证] 认证成功", "[Xiaomi Auth] Authentication OK" },
                { "[小米认证] 认证失败", "[Xiaomi Auth] Authentication failed" },
                { "[小米认证] 令牌无效或已过期", "[Xiaomi Auth] Token invalid or expired" },
                { "[小米认证] 发送令牌...", "[Xiaomi Auth] Sending token..." },

                // ==================== oneplus_auth_strategy.cs ====================
                { "[OnePlus] 未找到 VIP Digest 文件", "[OnePlus] VIP Digest file not found" },
                { "[OnePlus] VIP 认证成功", "[OnePlus] VIP authentication OK" },
                { "[OnePlus] VIP 认证失败", "[OnePlus] VIP authentication failed" },
                { "[OnePlus] 执行 OnePlus/Realme VIP 认证...", "[OnePlus] Performing OnePlus/Realme VIP authentication..." },
                { "OnePlus 认证成功", "OnePlus authentication OK" },
                { "OnePlus 认证失败", "OnePlus authentication failed" },

                // ==================== pbl_exploit.cs ====================
                { "[PBL Exploit] 正在执行漏洞利用...", "[PBL Exploit] Executing exploit..." },
                { "[PBL Exploit] 正在发送 Payload...", "[PBL Exploit] Sending payload..." },
                { "[PBL Exploit] 发送 Payload 失败", "[PBL Exploit] Failed to send payload" },
                { "[PBL Exploit] 漏洞利用成功", "[PBL Exploit] Exploit successful" },
                { "[PBL Exploit] 漏洞利用失败", "[PBL Exploit] Exploit failed" },
                { "[PBL Exploit] 未知错误", "[PBL Exploit] Unknown error" },
                { "[PBL Exploit] 设备不在 Sahara 模式", "[PBL Exploit] Device not in Sahara mode" },
                { "[PBL Exploit] 开始 Sahara 漏洞攻击...", "[PBL Exploit] Starting Sahara exploit attack..." },

                // ==================== exploit_service.cs ====================
                { "漏洞利用成功", "Exploit successful" },
                { "漏洞利用失败", "Exploit failed" },

                // ==================== Fastboot ====================
                { "[Fastboot] 多次超时，断开连接", "[Fastboot] Multiple timeouts, disconnecting" },
                { "[Fastboot] 未连接设备", "[Fastboot] Device not connected" },
                { "[Fastboot] 正在读取设备信息...", "[Fastboot] Reading device info..." },
                { "[Fastboot] 提示: Bootloader 模式不支持读取分区列表，如需查看请进入 Fastbootd 模式", "[Fastboot] Note: Bootloader mode does not support partition list; use Fastbootd mode" },
                { "[Fastboot] 正在重启...", "[Fastboot] Rebooting..." },
                { "[Fastboot] 正在重启到 Bootloader...", "[Fastboot] Rebooting to Bootloader..." },
                { "[Fastboot] 正在重启到 Recovery...", "[Fastboot] Rebooting to Recovery..." },
                { "[Fastboot] 正在重启到 Fastbootd...", "[Fastboot] Rebooting to Fastbootd..." },
                { "[Fastboot] 正在解锁 Bootloader...", "[Fastboot] Unlocking Bootloader..." },
                { "[Fastboot] 正在锁定 Bootloader...", "[Fastboot] Locking Bootloader..." },
                { "[Fastboot] 执行 OEM EDL...", "[Fastboot] Executing OEM EDL..." },
                { "[Fastboot] 擦除 FRP 分区...", "[Fastboot] Erasing FRP partition..." },
                { "[Fastboot] 设备正在重启...", "[Fastboot] Device rebooting..." },
                { "[Fastboot] 设备正在重启到 Bootloader...", "[Fastboot] Device rebooting to Bootloader..." },
                { "[Fastboot] 设备正在重启到 Recovery...", "[Fastboot] Device rebooting to Recovery..." },
                { "[Fastboot] 设备正在重启到 Fastbootd...", "[Fastboot] Device rebooting to Fastbootd..." },
                { "[Fastboot] 正在删除 COW 快照分区...", "[Fastboot] Deleting COW snapshot partitions..." },
                { "[Fastboot] 逻辑分区结构重建完成", "[Fastboot] Logical partition structure rebuild complete" },
                { "[Fastboot] 正在清除用户数据...", "[Fastboot] Clearing user data..." },
                { "数据传输超时", "Data transfer timeout" },
                { "重启到系统...", "Rebooting to system..." },
                { "重启到 Bootloader...", "Rebooting to Bootloader..." },
                { "重启到 Fastbootd...", "Rebooting to Fastbootd..." },
                { "重启到 Recovery...", "Rebooting to Recovery..." },
                { "解锁 Bootloader...", "Unlocking Bootloader..." },
                { "锁定 Bootloader...", "Locking Bootloader..." },
                { "继续启动...", "Continuing boot..." },
                { "关机...", "Powering down..." },
                { "重启到 EDL...", "Rebooting to EDL..." },
                { "正在获取真实下载链接...", "Fetching real download URL..." },
                { "✓ 成功获取真实链接", "Got real download link" },
                { "URL 不能为空", "URL cannot be empty" },
                { "无法获取文件大小", "Unable to get file size" },
                { "解析 ZIP 结构...", "Parsing ZIP structure..." },
                { "请先加载 Payload", "Please load Payload first" },
                { "提取已取消", "Extraction cancelled" },
                { "操作已取消", "Operation cancelled" },
                { "[华为/荣耀] 正在读取设备信息...", "[Huawei/Honor] Reading device info..." },
                { "[华为/荣耀] FRP 密钥不能为空", "[Huawei/Honor] FRP key cannot be empty" },
                { "[华为/荣耀] FRP 解锁命令已发送", "[Huawei/Honor] FRP unlock command sent" },
                { "[华为/荣耀] 解锁码不能为空", "[Huawei/Honor] Unlock code cannot be empty" },
                { "[华为/荣耀] Bootloader 解锁成功", "[Huawei/Honor] Bootloader unlock OK" },
                { "[华为/荣耀] 正在锁定 Bootloader...", "[Huawei/Honor] Locking Bootloader..." },
                { "[华为/荣耀] Bootloader 锁定成功", "[Huawei/Honor] Bootloader lock OK" },
                { "[华为/荣耀] 正在重启到 EDL 模式...", "[Huawei/Honor] Rebooting to EDL mode..." },
                { "文件路径不能为空", "File path cannot be empty" },
                { "请先加载 Payload 文件", "Please load Payload file first" },
                { "正在从 ZIP 文件提取 payload.bin...", "Extracting payload.bin from ZIP file..." },
                { "ZIP 文件中未找到 payload.bin", "payload.bin not found in ZIP file" },
                { "Manifest 大小超出限制", "Manifest size exceeds limit" },
                { "未连接设备", "Device not connected" },

                // ==================== qualcomm_ui_controller.cs ====================
                { "连接成功！", "Connected!" },
                { "设备已断开连接，需要重新完整配置", "Device disconnected, full reconfiguration required" },
                { "操作进行中", "Operation in progress" },
                { "请选择端口", "Please select a port" },
                { "读取设备信息 : 成功", "Read device info : OK" },
                { "成功读取 {0} 个分区", "Read {0} partitions OK" },
                { "Realme 认证设备，跳过 build.prop 读取", "Realme authenticated device, skipping build.prop read" },
                { "设备已断开，请重新连接", "Device disconnected, please reconnect" },
                { "已发送重启到 EDL 命令，请重新连接", "Reboot to EDL command sent, please reconnect" },
                { "[高通] 断开连接", "[Qualcomm] Disconnected" },
                { "设备状态：未连接任何设备", "Device status: Not connected" },
                { "设备状态：等待连接", "Device status: Waiting for connection" },

                // ==================== Form1 kick EDL / Realme cloud ====================
                { "执行: 联想/安卓踢EDL (adb reboot edl)...", "Executing: Lenovo/Android kick EDL (adb reboot edl)..." },
                { "ADB: 踢EDL成功，设备将进入 EDL 模式", "ADB: Kick EDL OK, device will enter EDL mode" },
                { "踢EDL失败: {0}", "Kick EDL failed: {0}" },

                // ==================== Misc status patterns ====================
                { "正在读取分区表 (GPT)...", "Reading partition table (GPT)..." },
                { "成功", "OK" },
                { "失败", "Failed" },
            };
        }

        #endregion

        #region Phrase Replacement Dictionary

        private static List<KeyValuePair<string, string>> BuildPhraseMap()
        {
            var phrases = new Dictionary<string, string>
            {
                // --- Info panel labels (QualcommUIController) ---
                { "设备状态：", "Device status: " },
                { "品牌：", "Brand: " },
                { "芯片：", "Chip: " },
                { "版本：", "Version: " },
                { "芯片序列号：", "Chip serial: " },
                { "型号：", "Model: " },
                { "代号：", "Codename: " },
                { "存储：", "Storage: " },
                { "等待连接", "Waiting for connection" },
                { "待深度扫描", "Scanning..." },
                { "未识别", "Unknown" },
                { "未获取", "N/A" },
                { "正在识别...", "Identifying..." },
                { "[Realme云端] 已选择: ", "[Realme Cloud] Selected: " },

                // --- Multi-char phrases first (longer = higher priority) ---
                { "，开始readdevice info...", ", starting read device info..." },
                { "Loader 下载完成", "Loader download complete" },
                { "Digest 下载完成", "Digest download complete" },
                { "执行云端签名认证", "Performing cloud signing auth" },
                { "- 市场名称 : ", "- Market name : " },
                { "- 生产厂家 : ", "- Manufacturer : " },
                { "- 展示 ID : ", "- Display ID : " },
                { "- OTA 版本 : ", "- OTA version : " },
                { "- 完整 OTA : ", "- Full OTA : " },
                { "- OPLUS 项目 : ", "- OPLUS project : " },
                { " 解析OK", " parsed OK" },
                { " 解析成功", " parsed OK" },
                { "Programmer 文件不存在", "Programmer file does not exist" },
                { "Sahara 握手失败", "Sahara handshake failed" },
                { "漏洞利用", "exploit" },
                { "看门狗超时", "watchdog timeout" },
                { "看门狗", "watchdog" },
                { "分区条目偏移", "partition entry offset" },
                { "分区条目", "partition entries" },
                { "分区表", "partition table" },
                { "个分区", " partitions" },
                { "引导加载成功", "boot load OK" },
                { "引导加载", "boot loading" },
                { "引导文件", "boot file" },
                { "数据写入失败", "data write failed" },
                { "数据完整性验证失败", "data integrity check failed" },
                { "镜像签名不正确", "image signature incorrect" },
                { "签名验证失败", "signature verification failed" },
                { "设备信息", "device info" },
                { "连接异常", "connection error" },
                { "连接超时", "connection timeout" },
                { "连接成功", "connected" },
                { "连接失败", "connection failed" },
                { "获取签名材料", "obtain signing material" },
                { "云端签名认证", "cloud signing auth" },
                { "认证成功", "auth OK" },
                { "认证失败", "auth failed" },
                { "写保护", "write-protected" },
                { "写入完成", "write complete" },
                { "写入失败", "write failed" },
                { "读取完成", "read complete" },
                { "读取失败", "read failed" },
                { "读取成功", "read OK" },
                { "读取超时", "read timeout" },
                { "擦除成功", "erase OK" },
                { "擦除失败", "erase failed" },
                { "配置成功", "configured OK" },
                { "配置超时", "configuration timeout" },
                { "配置失败", "configuration failed" },
                { "已取消", "cancelled" },
                { "已通过", "passed" },
                { "未通过", "not passed" },
                { "已接受", "accepted" },
                { "已获取签名", "got signature" },
                { "已命中", "matched" },
                { "扇区大小", "sector size" },
                { "扇区", "sectors" },
                { "设备忙", "device busy" },
                { "设备错误", "device error" },
                { "设备侧", "device-side" },
                { "无效的 LUN", "invalid LUN" },
                { "分区未找到", "partition not found" },
                { "Hash 校验失败", "hash check failed" },
                { "未知错误", "unknown error" },
                { "请重试操作", "please retry" },
                { "操作超时，建议重试", "operation timed out, retry recommended" },
                { "暴力扫描成功", "brute-force scan OK" },
                { "无法读取头部数据", "unable to read header data" },

                // --- Status verbs / modifiers (medium length) ---
                { "正在解析", "Parsing" },
                { "正在读取", "Reading" },
                { "正在写入", "Writing" },
                { "正在发送", "Sending" },
                { "正在擦除", "Erasing" },
                { "正在配置", "Configuring" },
                { "正在连接", "Connecting" },
                { "正在扫描", "Scanning" },
                { "正在重启", "Rebooting" },
                { "正在删除", "Deleting" },
                { "正在清除", "Clearing" },
                { "正在锁定", "Locking" },
                { "正在执行", "Executing" },

                // --- Single-word translations (short, applied last) ---
                { "解析完成", "parsing complete" },
                { "解析失败", "parsing failed" },
                { "上传失败", "upload failed" },
                { "上传成功", "upload OK" },
                { "发送失败", "send failed" },
                { "发送成功", "send OK" },
                { "检测到", "detected" },
                { "初始化", "initialize" },
                { "格式化", "format" },
                { "加载中", "loading" },
                { "加载", "load" },
                { "成功", "OK" },
                { "失败", "failed" },
                { "错误", "error" },
                { "异常", "exception" },
                { "超时", "timeout" },
                { "认证", "auth" },
                { "签名", "signature" },
                { "设备", "device" },
                { "分区", "partition" },
                { "引导", "boot" },
                { "镜像", "image" },
                { "擦除", "erase" },
                { "读取", "read" },
                { "写入", "write" },
                { "配置", "configure" },
                { "连接", "connect" },
                { "字节", "bytes" },
                { "有效", "valid" },
                { "无效", "invalid" },
                { "存储", "storage" },
                { "解锁", "unlock" },
                { "锁定", "lock" },
                { "恢复", "restore" },
                { "备份", "backup" },
                { "重启", "reboot" },
            };

            return phrases
                .OrderByDescending(kv => kv.Key.Length)
                .ToList();
        }

        #endregion
    }
}
