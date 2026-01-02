using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Linq;
using OPFlashTool.Qualcomm;

namespace OPFlashTool.Strategies
{
    /// <summary>
    /// OPPO/Realme VIP 设备策略
    /// 基于 OP_Flash_Tool_1.3 优化，支持自动检测读写模式
    /// </summary>
    public class OppoVipDeviceStrategy : StandardDeviceStrategy
    {
        public override string Name => "Oppo/Realme VIP";

        #region RW Mode Detection (基于 OP_Flash_Tool test_rw_mode.bat)

        /// <summary>
        /// OPPO 特殊读写模式
        /// </summary>
        public enum OplusRwMode
        {
            Unknown,         // 未检测
            Normal,          // 普通模式 (无伪装)
            GptBackup,       // oplus_gptbackup 模式
            GptMain_Mode1,   // oplus_gptmain 模式 1 (Gap 在 sector 6)
            GptMain_Mode2    // oplus_gptmain 模式 2 (Gap 在 sector 34)
        }

        // 当前检测到的读写模式
        private OplusRwMode _currentRwMode = OplusRwMode.Unknown;
        
        // Gap 扇区位置 (根据模式自动设置)
        private long _gapSector = 6; // UFS 默认

        // 缓存每个 LUN 的第一个分区名称 (用于 gptmain 方案的分段伪装)
        private Dictionary<int, string> _lunFirstPartitions = new Dictionary<int, string>();

        /// <summary>
        /// 自动检测 OPPO 特殊读写模式
        /// 参考 OP_Flash_Tool_1.3 的 test_rw_mode.bat
        /// </summary>
        private async Task<OplusRwMode> DetectRwModeAsync(FirehoseClient client, Action<string> log, CancellationToken ct)
        {
            log("[Oppo] 正在检测特殊读写模式...");

            int sectorSize = client.SectorSize;
            string tempPath = Path.Combine(Path.GetTempPath(), "oppo_rwmode_test.bin");

            try
            {
                // === 测试 1: oplus_gptbackup 模式 (sector 5-35) ===
                log("[Test] 测试 gptbackup 模式 (sector 5-35)...");
                bool gptbackupSuccess = await TestReadSectorRange(client, 5, 31, "gpt_backup0.bin", "BackupGPT", tempPath, ct);
                
                if (gptbackupSuccess)
                {
                    log("[Oppo] 检测结果: oplus_gptbackup 模式");
                    _gapSector = -1; // gptbackup 模式无 Gap
                    return OplusRwMode.GptBackup;
                }

                // === 测试 2: oplus_gptmain 模式 ===
                log("[Test] 测试 gptmain 模式...");

                // 测试 sector 33-35 (快速判断 mode1 vs mode2)
                bool sector33_35Success = await TestReadSectorRange(client, 33, 3, "gpt_main0.bin", "gpt_main0.bin", tempPath, ct);
                
                if (sector33_35Success)
                {
                    // 能读 33-35，说明 Gap 在 sector 6 (mode1)
                    log("[Oppo] 检测结果: oplus_gptmain 模式 1 (Gap @ sector 6)");
                    _gapSector = 6;
                    return OplusRwMode.GptMain_Mode1;
                }
                else
                {
                    // 不能读 33-35，可能 Gap 在 sector 34 (mode2)
                    // 验证: 测试 sector 35+ 是否可读
                    bool sector35Success = await TestReadSectorRange(client, 35, 10, "gpt_main0.bin", "gpt_main0.bin", tempPath, ct);
                    
                    if (sector35Success)
                    {
                        log("[Oppo] 检测结果: oplus_gptmain 模式 2 (Gap @ sector 34)");
                        _gapSector = 34;
                        return OplusRwMode.GptMain_Mode2;
                    }
                }

                // === 测试 3: 普通模式 ===
                log("[Test] 测试普通模式...");
                bool normalSuccess = await TestReadSectorRange(client, 0, 6, "gpt_main0.bin", "PrimaryGPT", tempPath, ct);
                
                if (normalSuccess)
                {
                    log("[Oppo] 检测结果: 普通模式 (无特殊限制)");
                    _gapSector = -1;
                    return OplusRwMode.Normal;
                }

                log("[Oppo] 检测结果: 未知模式，将使用瀑布流策略");
                return OplusRwMode.Unknown;
            }
            catch (Exception ex)
            {
                log($"[Oppo] 模式检测异常: {ex.Message}");
                return OplusRwMode.Unknown;
            }
            finally
            {
                // 清理临时文件
                try { if (File.Exists(tempPath)) File.Delete(tempPath); } catch { }
            }
        }

        /// <summary>
        /// 测试读取指定扇区范围
        /// </summary>
        private async Task<bool> TestReadSectorRange(FirehoseClient client, long startSector, long numSectors, 
            string filename, string label, string savePath, CancellationToken ct)
        {
            try
            {
                // 使用分块读取，suppressError=true 避免错误日志
                return await client.ReadPartitionChunkedAsync(
                    savePath,
                    startSector.ToString(),
                    numSectors,
                    "0", // LUN 0
                    null, // 无进度回调
                    ct,
                    label,
                    filename,
                    append: false,
                    suppressError: true
                );
            }
            catch
            {
                return false;
            }
        }

        #endregion

        #region Authentication

        public override Task<bool> AuthenticateAsync(FirehoseClient client, string programmerPath, Action<string> log, 
            Func<string, string> inputCallback = null, string digestPath = null, string signaturePath = null)
        {
            log("[Oppo] 准备执行 VIP 签名验证...");
            string finalDigest = digestPath;
            string finalSig = signaturePath;

            // 1. 自动查找逻辑 (兼容 .bin 和 .mbn)
            if (string.IsNullOrEmpty(finalDigest))
            {
                string dir = Path.GetDirectoryName(programmerPath);
                string[] digestNames = { "digest.bin", "digest.mbn", "Digest.bin", "Digest.mbn" };
                foreach (var name in digestNames)
                {
                    string t = Path.Combine(dir, name);
                    if (File.Exists(t)) { finalDigest = t; break; }
                }
            }

            if (string.IsNullOrEmpty(finalSig))
            {
                string dir = Path.GetDirectoryName(programmerPath);
                string[] sigNames = { "signature.bin", "signature.mbn", "Signature.bin", "Signature.mbn", "sig.bin", "sig.mbn" };
                foreach (var name in sigNames)
                {
                    string t = Path.Combine(dir, name);
                    if (File.Exists(t)) { finalSig = t; break; }
                }
            }

            // 2. 检查文件
            if (string.IsNullOrEmpty(finalDigest) || !File.Exists(finalDigest) ||
                string.IsNullOrEmpty(finalSig) || !File.Exists(finalSig))
            {
                log($"[Oppo] 警告: 未找到 VIP 验证文件 (Digest/Signature)！");
                log($"[Oppo] 请手动选择文件或将其放入引导目录。");
                return Task.FromResult(true); // 允许继续尝试
            }

            log($"[Oppo] Digest: {Path.GetFileName(finalDigest)}");
            log($"[Oppo] Signature: {Path.GetFileName(finalSig)}");

            // 3. 执行验证
            return Task.FromResult(client.PerformVipAuth(finalDigest, finalSig));
        }

        #endregion

        #region GPT Reading

        public override async Task<List<PartitionInfo>> ReadGptAsync(FirehoseClient client, CancellationToken ct, Action<string> log)
        {
            // 首先检测读写模式
            if (_currentRwMode == OplusRwMode.Unknown)
            {
                _currentRwMode = await DetectRwModeAsync(client, log, ct);
            }

            var allPartitions = new List<PartitionInfo>();
            _lunFirstPartitions.Clear();

            const int maxLun = 5;
            const int sectorsToRead = 6;
            int lunRead = 0;

            // 根据检测到的模式选择策略
            var spoofStrategies = GetGptSpoofStrategies();

            log($"[Info] 开始读取分区表 (模式: {_currentRwMode})...");

            for (int lun = 0; lun <= maxLun; lun++)
            {
                if (ct.IsCancellationRequested) break;

                byte[] data = null;
                bool success = false;

                foreach (var (label, filenameTemplate) in spoofStrategies)
                {
                    if (success) break;

                    try
                    {
                        string filename = filenameTemplate.Replace("{lun}", lun.ToString());

                        data = await client.ReadGptPacketAsync(
                            lun.ToString(),
                            0,
                            sectorsToRead,
                            label,
                            filename,
                            ct
                        );

                        if (data != null && data.Length > 0 && IsValidGptData(data))
                        {
                            success = true;
                            log($"[Debug] LUN{lun}: 策略 [{label}] 成功");
                            break;
                        }
                    }
                    catch { }

                    await Task.Delay(50);
                }

                if (success && data != null)
                {
                    try
                    {
                        var parts = GptParser.ParseGptBytes(data, lun);

                        if (parts != null && parts.Count > 0)
                        {
                            allPartitions.AddRange(parts);
                            lunRead++;
                            log($"[Success] LUN{lun}: 读取到 {parts.Count} 个分区");

                            var firstPart = parts.OrderBy(p => p.StartLba).FirstOrDefault();
                            if (firstPart != null)
                            {
                                _lunFirstPartitions[lun] = firstPart.Name;
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        log($"[Warn] LUN{lun}: 解析失败 - {ex.Message}");
                    }
                }
                else
                {
                    if (lun == 0) log($"[Error] 无法读取 LUN0 分区表");
                    else log($"[Info] LUN{lun} 无响应或不存在");
                }
            }

            log($"[GPT] 共读取到 {lunRead} 个 LUN，解析出 {allPartitions.Count} 个分区");

            if (allPartitions.Count == 0)
            {
                log("[警告] VIP 模式无法读取分区表");
                log("[提示] 您可使用 XML 刷写模式 (rawprogram*.xml) 进行刷机操作");
            }

            return allPartitions;
        }

        /// <summary>
        /// 根据检测到的模式获取 GPT 读取策略
        /// </summary>
        private (string label, string filename)[] GetGptSpoofStrategies()
        {
            switch (_currentRwMode)
            {
                case OplusRwMode.GptBackup:
                    return new[]
                    {
                        ("BackupGPT", "gpt_backup{lun}.bin"),
                        ("gpt_backup0.bin", "gpt_backup0.bin"),
                        ("PrimaryGPT", "gpt_main{lun}.bin"),
                    };

                case OplusRwMode.GptMain_Mode1:
                case OplusRwMode.GptMain_Mode2:
                    return new[]
                    {
                        ("gpt_main0.bin", "gpt_main{lun}.bin"),
                        ("PrimaryGPT", "gpt_main{lun}.bin"),
                        ("gpt_main0.bin", "gpt_main0.bin"),
                        ("BackupGPT", "gpt_backup{lun}.bin"),
                    };

                case OplusRwMode.Normal:
                    return new[]
                    {
                        ("PrimaryGPT", "gpt_main{lun}.bin"),
                        ("BackupGPT", "gpt_backup{lun}.bin"),
                    };

                default: // Unknown - 使用完整瀑布流
                    return new[]
                    {
                        ("PrimaryGPT", "gpt_main{lun}.bin"),
                        ("BackupGPT", "gpt_main{lun}.bin"),
                        ("BackupGPT", "gpt_backup{lun}.bin"),
                        ("gpt_backup0.bin", "gpt_backup0.bin"),
                        ("gpt_main0.bin", "gpt_main0.bin"),
                        ("ssd", "ssd"),
                        ("super", "super"),
                        ("userdata", "userdata"),
                    };
            }
        }

        /// <summary>
        /// 验证 GPT 数据有效性
        /// </summary>
        private bool IsValidGptData(byte[] data)
        {
            if (data == null || data.Length < 512) return false;
            // 检查 GPT 签名 "EFI PART" at offset 512
            if (data.Length >= 520)
            {
                return data[512] == 0x45 && data[513] == 0x46 && 
                       data[514] == 0x49 && data[515] == 0x20 &&
                       data[516] == 0x50 && data[517] == 0x41 &&
                       data[518] == 0x52 && data[519] == 0x54;
            }
            return data.Length > 0;
        }

        #endregion

        #region Partition Reading (Optimized)

        public override async Task<bool> ReadPartitionAsync(FirehoseClient client, PartitionInfo part, string savePath, 
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            // 如果还没检测模式，先检测
            if (_currentRwMode == OplusRwMode.Unknown)
            {
                _currentRwMode = await DetectRwModeAsync(client, log, ct);
            }

            // 根据模式选择读取策略
            switch (_currentRwMode)
            {
                case OplusRwMode.GptBackup:
                    return await ReadWithGptBackupMode(client, part, savePath, progress, ct, log);

                case OplusRwMode.GptMain_Mode1:
                case OplusRwMode.GptMain_Mode2:
                    return await ReadWithGptMainMode(client, part, savePath, progress, ct, log);

                case OplusRwMode.Normal:
                    return await ReadWithNormalMode(client, part, savePath, progress, ct, log);

                default:
                    return await ReadWithWaterfallStrategy(client, part, savePath, progress, ct, log);
            }
        }

        /// <summary>
        /// GptBackup 模式读取
        /// </summary>
        private async Task<bool> ReadWithGptBackupMode(FirehoseClient client, PartitionInfo part, string savePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            log($"[Read] {part.Name} (gptbackup 模式)");

            var strategies = new[]
            {
                ("gpt_backup0.bin", "BackupGPT"),
                ("gpt_backup0.bin", "gpt_backup0.bin"),
                ("ssd", "ssd"),
            };

            foreach (var (filename, label) in strategies)
            {
                if (ct.IsCancellationRequested) return false;

                try
                {
                    bool success = await client.ReadPartitionChunkedAsync(
                        savePath,
                        part.StartLba.ToString(),
                        (long)part.Sectors,
                        part.Lun.ToString(),
                        progress,
                        ct,
                        label,
                        filename,
                        append: false,
                        suppressError: true
                    );

                    if (success) return true;
                }
                catch { }

                await Task.Delay(50);
            }

            return false;
        }

        /// <summary>
        /// GptMain 模式读取 (支持 Gap 分段)
        /// </summary>
        private async Task<bool> ReadWithGptMainMode(FirehoseClient client, PartitionInfo part, string savePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            long startSector = (long)part.StartLba;
            long endSector = startSector + (long)part.Sectors - 1;

            // 检查是否涉及 Gap
            bool involvesGap = (startSector <= _gapSector && endSector >= _gapSector);

            if (involvesGap && _gapSector > 0)
            {
                // 需要分段读取
                log($"[Read] {part.Name} (gptmain 分段模式, Gap @ {_gapSector})");
                return await ReadSegmentedAroundGap(client, part, savePath, progress, ct, log);
            }
            else
            {
                // 普通 gptmain 读取
                log($"[Read] {part.Name} (gptmain 模式)");
                return await client.ReadPartitionChunkedAsync(
                    savePath,
                    part.StartLba.ToString(),
                    (long)part.Sectors,
                    part.Lun.ToString(),
                    progress,
                    ct,
                    "gpt_main0.bin",
                    "gpt_main0.bin",
                    append: false,
                    suppressError: false
                );
            }
        }

        /// <summary>
        /// 围绕 Gap 分段读取
        /// </summary>
        private async Task<bool> ReadSegmentedAroundGap(FirehoseClient client, PartitionInfo part, string savePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            long currentSector = (long)part.StartLba;
            long remainingSectors = (long)part.Sectors;
            long totalBytes = remainingSectors * client.SectorSize;
            long currentBytesRead = 0;

            // 清空目标文件
            if (File.Exists(savePath)) File.Delete(savePath);

            string firstPartName = _lunFirstPartitions.ContainsKey(part.Lun) 
                ? _lunFirstPartitions[part.Lun] 
                : part.Name;

            while (remainingSectors > 0)
            {
                if (ct.IsCancellationRequested) return false;

                string currentFilename = "gpt_main0.bin";
                string currentLabel = "gpt_main0.bin";
                long sectorsToRead = remainingSectors;

                if (currentSector < _gapSector)
                {
                    // Segment 1: 0 到 Gap-1
                    long dist = _gapSector - currentSector;
                    sectorsToRead = Math.Min(remainingSectors, dist);
                }
                else if (currentSector == _gapSector)
                {
                    // Segment 2: Gap 扇区 (使用首个分区名称伪装)
                    sectorsToRead = 1;
                    currentFilename = firstPartName;
                    currentLabel = firstPartName;
                    log($"[Debug] 读取 Gap 扇区 {_gapSector} (伪装: {firstPartName})");
                }
                // else: Segment 3 - Gap+1 到末尾，使用默认 gpt_main0.bin

                bool success = await client.ReadPartitionChunkedAsync(
                    savePath,
                    currentSector.ToString(),
                    sectorsToRead,
                    part.Lun.ToString(),
                    (c, t) => progress?.Invoke(currentBytesRead + c, totalBytes),
                    ct,
                    currentLabel,
                    currentFilename,
                    append: true,
                    suppressError: false
                );

                if (!success)
                {
                    log($"[Error] 分段读取失败 @ sector {currentSector}");
                    return false;
                }

                currentSector += sectorsToRead;
                remainingSectors -= sectorsToRead;
                currentBytesRead += sectorsToRead * client.SectorSize;
            }

            return true;
        }

        /// <summary>
        /// 普通模式读取
        /// </summary>
        private async Task<bool> ReadWithNormalMode(FirehoseClient client, PartitionInfo part, string savePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            log($"[Read] {part.Name} (普通模式)");
            return await client.ReadPartitionAsync(
                savePath,
                part.StartLba.ToString(),
                (long)part.Sectors,
                part.Lun.ToString(),
                progress,
                ct,
                part.Name
            );
        }

        /// <summary>
        /// 瀑布流策略读取 (未知模式时使用)
        /// </summary>
        private async Task<bool> ReadWithWaterfallStrategy(FirehoseClient client, PartitionInfo part, string savePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            log($"[Read] {part.Name} (瀑布流策略)");

            bool isUfs = client.StorageType.Contains("ufs");
            long gapSector = isUfs ? 6 : 34;

            var strategies = new List<(string filename, string label, bool checkGap)>
            {
                ("gpt_main0.bin", "gpt_main0.bin", true),
                ("gpt_backup0.bin", "BackupGPT", false),
                ("gpt_backup0.bin", "gpt_backup0.bin", false),
                ("ssd", "ssd", false),
                (part.Name, part.Name, false),
            };

            foreach (var (filename, label, checkGap) in strategies)
            {
                if (ct.IsCancellationRequested) return false;

                try
                {
                    bool success;

                    if (checkGap)
                    {
                        // 检查是否涉及 Gap
                        long start = (long)part.StartLba;
                        long end = start + (long)part.Sectors - 1;

                        if (start <= gapSector && end >= gapSector)
                        {
                            // 需要分段
                            _gapSector = gapSector;
                            success = await ReadSegmentedAroundGap(client, part, savePath, progress, ct, log);
                        }
                        else
                        {
                            success = await client.ReadPartitionChunkedAsync(
                                savePath, part.StartLba.ToString(), (long)part.Sectors, part.Lun.ToString(),
                                progress, ct, label, filename, false, true);
                        }
                    }
                    else
                    {
                        success = await client.ReadPartitionChunkedAsync(
                            savePath, part.StartLba.ToString(), (long)part.Sectors, part.Lun.ToString(),
                            progress, ct, label, filename, false, true);
                    }

                    if (success)
                    {
                        log($"[Success] 使用策略: {filename}");
                        return true;
                    }
                }
                catch { }

                await Task.Delay(50);
            }

            log($"[Error] 读取失败: {part.Name}");
            return false;
        }

        #endregion

        #region Partition Writing (Optimized)

        public override async Task<bool> WritePartitionAsync(FirehoseClient client, PartitionInfo part, string imagePath,
            Action<long, long> progress, CancellationToken ct, Action<string> log)
        {
            if (_currentRwMode == OplusRwMode.Unknown)
            {
                _currentRwMode = await DetectRwModeAsync(client, log, ct);
            }

            log($"[Write] {part.Name} (模式: {_currentRwMode})");

            var strategies = GetWriteSpoofStrategies();

            foreach (var (filename, label) in strategies)
            {
                if (ct.IsCancellationRequested) return false;

                try
                {
                    bool success = await client.FlashPartitionAsync(
                        imagePath,
                        part.StartLba.ToString(),
                        (long)part.Sectors,
                        part.Lun.ToString(),
                        progress,
                        ct,
                        label,
                        filename
                    );

                    if (success)
                    {
                        log($"[Success] 写入成功 (策略: {filename})");
                        return true;
                    }
                }
                catch (Exception ex)
                {
                    log($"[Debug] 策略 {filename} 失败: {ex.Message}");
                }

                await Task.Delay(50);
            }

            log($"[Error] 写入失败: {part.Name}");
            return false;
        }

        /// <summary>
        /// 根据模式获取写入策略
        /// </summary>
        private (string filename, string label)[] GetWriteSpoofStrategies()
        {
            switch (_currentRwMode)
            {
                case OplusRwMode.GptBackup:
                    return new[]
                    {
                        ("gpt_backup0.bin", "BackupGPT"),
                        ("gpt_backup0.bin", "gpt_backup0.bin"),
                        ("ssd", "ssd"),
                    };

                case OplusRwMode.GptMain_Mode1:
                case OplusRwMode.GptMain_Mode2:
                    return new[]
                    {
                        ("gpt_main0.bin", "gpt_main0.bin"),
                        ("gpt_backup0.bin", "BackupGPT"),
                        ("ssd", "ssd"),
                    };

                default:
                    return new[]
                    {
                        ("gpt_backup0.bin", "BackupGPT"),
                        ("gpt_backup0.bin", "gpt_backup0.bin"),
                        ("gpt_main0.bin", "gpt_main0.bin"),
                        ("ssd", "ssd"),
                    };
            }
        }

        #endregion

        #region Utilities

        /// <summary>
        /// 重置检测状态 (用于设备更换时)
        /// </summary>
        public void ResetDetection()
        {
            _currentRwMode = OplusRwMode.Unknown;
            _gapSector = 6;
            _lunFirstPartitions.Clear();
        }

        /// <summary>
        /// 获取当前检测到的模式
        /// </summary>
        public OplusRwMode CurrentRwMode => _currentRwMode;

        /// <summary>
        /// 获取 Gap 扇区位置
        /// </summary>
        public long GapSector => _gapSector;

        #endregion
    }
}
