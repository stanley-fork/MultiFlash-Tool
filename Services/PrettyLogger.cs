using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using OPFlashTool.Qualcomm;

namespace OPFlashTool.Services
{
    /// <summary>
    /// 专业格式化日志输出器
    /// 模拟专业刷机工具的日志风格
    /// </summary>
    public class PrettyLogger
    {
        private Action<string> _output;
        private Stopwatch _operationTimer;
        private string _currentOperation;
        private int _stepIndex;

        // 颜色/样式标记 (用于富文本控件)
        public bool UseColors { get; set; } = true;
        
        // 状态标记
        public const string OK = ":Ok";
        public const string FAIL = ":Failed";
        public const string WAIT = "...";

        public PrettyLogger(Action<string> outputAction)
        {
            _output = outputAction ?? Console.WriteLine;
            _operationTimer = new Stopwatch();
        }

        #region 基础输出方法

        /// <summary>
        /// 输出普通行
        /// </summary>
        public void Log(string message)
        {
            _output(message);
        }

        /// <summary>
        /// 输出带状态的行 (自动追加 :Ok 或 :Failed)
        /// </summary>
        public void LogStatus(string action, bool success)
        {
            _output($"{action} {(success ? OK : FAIL)}");
        }

        /// <summary>
        /// 输出等待状态行
        /// </summary>
        public void LogWaiting(string action)
        {
            _output($"{action} {WAIT}");
        }

        /// <summary>
        /// 输出带结果的行
        /// </summary>
        public void LogResult(string label, string value)
        {
            _output($"{label} :{value}");
        }

        /// <summary>
        /// 输出空行
        /// </summary>
        public void NewLine()
        {
            _output("");
        }

        #endregion

        #region 分隔符和标题

        /// <summary>
        /// 输出分隔线
        /// </summary>
        public void Separator(char ch = '═', int length = 50)
        {
            _output(new string(ch, length));
        }

        /// <summary>
        /// 输出标题块
        /// </summary>
        public void Title(string title)
        {
            int padding = Math.Max(0, (50 - title.Length - 4) / 2);
            string line = new string('═', 50);
            _output(line);
            _output($"║{new string(' ', padding)} {title} {new string(' ', 50 - padding - title.Length - 4)}║");
            _output(line);
        }

        /// <summary>
        /// 输出章节标题
        /// </summary>
        public void Section(string title)
        {
            _output($"▶️ {title}");
        }

        #endregion

        #region 设备信息输出

        /// <summary>
        /// 输出设备连接等待
        /// </summary>
        public void WaitingForDevice(string deviceType, int timeoutMinutes = 3)
        {
            _output($"Hold boot key And Connect To PC (power off mode)");
            _output($"Waiting for {deviceType} port (Timeout {timeoutMinutes} minute) {WAIT}");
        }

        /// <summary>
        /// 输出设备检测成功
        /// </summary>
        public void DeviceDetected(string mode)
        {
            _output($"{mode} service detected");
        }

        /// <summary>
        /// 输出 CPU 信息
        /// </summary>
        public void CpuInfo(string cpuId, string cpuName, string description = "")
        {
            string full = string.IsNullOrEmpty(description) 
                ? $"{cpuId} {cpuName}" 
                : $"{cpuId} {cpuName} ({description})";
            _output($"CPU Info :{full}");
        }

        /// <summary>
        /// 输出硬件信息块
        /// </summary>
        public void HardwareInfo(
            string hwCode = null,
            string hwSubCode = null, 
            string hwVersion = null,
            string swVersion = null,
            bool? isSecureBoot = null,
            bool? slaProtect = null,
            bool? daaProtect = null,
            string meId = null,
            string socId = null)
        {
            if (hwSubCode != null) _output($"Hardware Sub Code :{hwSubCode}");
            if (hwCode != null) _output($"Hardware Code :{hwCode}");
            if (hwVersion != null) _output($"Hardware Version :{hwVersion}");
            if (swVersion != null) _output($"Software Version :{swVersion}");
            if (isSecureBoot.HasValue) _output($"Is Secure boot :{isSecureBoot.Value}");
            if (slaProtect.HasValue) _output($"Serial Link authorization Protect :{slaProtect.Value}");
            if (daaProtect.HasValue) _output($"download agent authorization Protect :{daaProtect.Value}");
            if (meId != null) _output($"ME_ID:{meId}");
            if (socId != null) _output($"SOCID:{socId}");
        }

        /// <summary>
        /// 输出 Qualcomm 设备信息
        /// </summary>
        public void QualcommDeviceInfo(
            string chipName,
            string msmId,
            string pkHash,
            string oemId = null,
            string modelId = null,
            string serial = null,
            int saharaVersion = 2,
            bool is64Bit = true)
        {
            _output($"CPU Info :{chipName} [{msmId}]");
            if (serial != null) _output($"Serial Number :{serial}");
            if (oemId != null) _output($"OEM ID :{oemId}");
            if (modelId != null) _output($"Model ID :{modelId}");
            _output($"Sahara Version :V{saharaVersion}");
            _output($"Architecture :{(is64Bit ? "64-bit" : "32-bit")}");
            if (!string.IsNullOrEmpty(pkHash))
            {
                if (pkHash.Length > 32)
                {
                    _output($"PK_HASH[0] :{pkHash.Substring(0, 32)}");
                    _output($"PK_HASH[1] :{pkHash.Substring(pkHash.Length - 32)}");
                }
                else
                {
                    _output($"PK_HASH :{pkHash}");
                }
            }
        }

        /// <summary>
        /// 输出 Android 系统信息块
        /// </summary>
        public void AndroidInfo(
            string oem,
            string model,
            string name,
            string product,
            string sdkVer,
            string codeName,
            string incremental,
            string buildId,
            string androidVer,
            string securityVer,
            string cpuAbi = null,
            string buildDate = null,
            string fingerprint = null)
        {
            Section("Android OS Info");
            _output($"  • OEM          : {oem}");
            _output($"  • Model        : {model}");
            _output($"  • Name         : {name}");
            _output($"  • Product      : {product}");
            _output($"  • SDK Ver      : {sdkVer}");
            _output($"  • Code Name    : {codeName}");
            _output($"  • Incremental  : {incremental}");
            _output($"  • Build ID     : {buildId}");
            _output($"  • Android Ver  : {androidVer}");
            _output($"  • Security Ver : {securityVer}");
            if (cpuAbi != null) _output($"  • CPU ABI      : {cpuAbi}");
            if (buildDate != null) _output($"  • Build Date   : {buildDate}");
            if (fingerprint != null) _output($"  • Fingerprint  : {fingerprint}");
        }

        /// <summary>
        /// 输出分区信息
        /// </summary>
        public void PartitionInfo(string name, long sizeBytes, string fsType = null)
        {
            string size = FormatSize(sizeBytes);
            string fs = string.IsNullOrEmpty(fsType) ? "" : $"   [{fsType}]";
            _output($"    • {name,-12} :  {size,-12}{fs}");
        }

        /// <summary>
        /// 输出分区列表标题
        /// </summary>
        public void PartitionListHeader()
        {
            Section("Finding target to read info");
        }

        #endregion

        #region 操作流程输出

        /// <summary>
        /// 开始一个操作
        /// </summary>
        public void StartOperation(string operationName)
        {
            _currentOperation = operationName;
            _operationTimer.Restart();
            _stepIndex = 0;
            NewLine();
            _output($"Operation: {operationName}");
        }

        /// <summary>
        /// 结束当前操作
        /// </summary>
        public void EndOperation(bool success = true)
        {
            _operationTimer.Stop();
            var elapsed = _operationTimer.Elapsed;
            _output($"Elapsed time: {elapsed:mm\\:ss}");
            if (!success)
            {
                _output($"Operation {_currentOperation} {FAIL}");
            }
            NewLine();
        }

        /// <summary>
        /// 输出操作步骤
        /// </summary>
        public void Step(string action, bool success = true)
        {
            _stepIndex++;
            _output($"{action} {(success ? OK : FAIL)}");
        }

        /// <summary>
        /// 输出进行中的步骤
        /// </summary>
        public void StepInProgress(string action)
        {
            _output($"{action} {WAIT}");
        }

        #endregion

        #region Qualcomm 特定流程

        /// <summary>
        /// Sahara 握手流程
        /// </summary>
        public void SaharaHandshake()
        {
            _output("Qualcomm EDL Mode detected");
            _output($"Handshake device {OK}");
            _output($"Reading device Info {OK}");
        }

        /// <summary>
        /// Sahara 加载器上传
        /// </summary>
        public void SaharaUploadLoader(string loaderName, long size)
        {
            _output($"Sending Firehose Loader ({loaderName}, {FormatSize(size)}) {WAIT}");
        }

        /// <summary>
        /// Sahara 完成
        /// </summary>
        public void SaharaComplete()
        {
            _output($"Firehose Loader uploaded successfully {OK}");
            _output($"Switching to Firehose mode {OK}");
        }

        /// <summary>
        /// Firehose 配置
        /// </summary>
        public void FirehoseConfigure(string storageType, int payloadSize)
        {
            _output($"Qualcomm Firehose Mode Active");
            _output($"Storage Type :{storageType.ToUpper()}");
            _output($"Max Payload Size :{FormatSize(payloadSize)}");
        }

        /// <summary>
        /// VIP 认证
        /// </summary>
        public void VipAuth(bool success)
        {
            _output($"Sending VIP Digest {OK}");
            _output($"Sending VIP Signature {OK}");
            _output($"VIP Authentication {(success ? OK : FAIL)}");
        }

        /// <summary>
        /// 小米认证
        /// </summary>
        public void MiAuth(bool success, int signatureIndex = 0)
        {
            if (success)
            {
                _output($"MiAuth Bypass (Signature #{signatureIndex}) {OK}");
            }
            else
            {
                _output($"MiAuth Bypass {FAIL}");
            }
        }

        #endregion

        #region 读写操作

        /// <summary>
        /// 分区读取进度
        /// </summary>
        public void ReadProgress(string partitionName, long current, long total, double speedMBps)
        {
            int percent = (int)((double)current / total * 100);
            string progress = $"[{'█'.ToString().PadRight(percent / 5, '█').PadRight(20, '░')}] {percent}%";
            _output($"\rReading {partitionName}: {progress} {speedMBps:F1} MB/s");
        }

        /// <summary>
        /// 分区写入进度
        /// </summary>
        public void WriteProgress(string partitionName, long current, long total, double speedMBps)
        {
            int percent = (int)((double)current / total * 100);
            string progress = $"[{'█'.ToString().PadRight(percent / 5, '█').PadRight(20, '░')}] {percent}%";
            _output($"\rWriting {partitionName}: {progress} {speedMBps:F1} MB/s");
        }

        /// <summary>
        /// 读取完成
        /// </summary>
        public void ReadComplete(string partitionName, long size, double seconds)
        {
            double speed = size / 1024.0 / 1024.0 / seconds;
            _output($"Reading {partitionName} ({FormatSize(size)}) {OK} [{speed:F1} MB/s]");
        }

        /// <summary>
        /// 写入完成
        /// </summary>
        public void WriteComplete(string partitionName, long size, double seconds)
        {
            double speed = size / 1024.0 / 1024.0 / seconds;
            _output($"Writing {partitionName} ({FormatSize(size)}) {OK} [{speed:F1} MB/s]");
        }

        /// <summary>
        /// 擦除完成
        /// </summary>
        public void EraseComplete(string partitionName, bool success)
        {
            _output($"Erasing {partitionName} {(success ? OK : FAIL)}");
        }

        #endregion

        #region GPT 和分区表

        /// <summary>
        /// GPT 读取结果
        /// </summary>
        public void GptResult(int lunCount, int partitionCount)
        {
            _output($"Reading GPT from device {OK}");
            _output($"  LUNs detected: {lunCount}");
            _output($"  Partitions found: {partitionCount}");
        }

        /// <summary>
        /// 输出分区表 (简单版)
        /// </summary>
        public void PrintPartitionTable(List<OPFlashTool.PartitionInfo> partitions)
        {
            Section("Partition Table");
            _output($"{"Name",-20} {"Start",-12} {"Size",-12} {"LUN",-4}");
            _output(new string('-', 52));
            
            foreach (var p in partitions)
            {
                string size = FormatSize((long)p.Sectors * p.SectorSize);
                _output($"{p.Name,-20} {p.StartLba,-12} {size,-12} {p.Lun,-4}");
            }
        }

        /// <summary>
        /// 输出分区表 (增强版 - 显示文件系统和镜像格式)
        /// </summary>
        public void PrintPartitionTableEnhanced(List<OPFlashTool.PartitionInfo> partitions, OPFlashTool.PartitionSource source = OPFlashTool.PartitionSource.Unknown, string sourcePath = "")
        {
            // 来源信息
            string sourceStr = source switch
            {
                OPFlashTool.PartitionSource.Device => "📱 Device",
                OPFlashTool.PartitionSource.XmlFile => "📄 XML File",
                OPFlashTool.PartitionSource.GptFile => "💾 GPT File",
                _ => "❓ Unknown"
            };
            
            Section($"Partition Table ({sourceStr})");
            if (!string.IsNullOrEmpty(sourcePath))
                _output($"  Source: {sourcePath}");
            _output($"  Total: {partitions.Count} partitions");
            NewLine();

            // 按 LUN 分组
            var luns = partitions.Select(p => p.Lun).Distinct().OrderBy(l => l).ToList();
            
            foreach (var lun in luns)
            {
                var lunPartitions = partitions.Where(p => p.Lun == lun).OrderBy(p => p.StartLba).ToList();
                
                _output($"══════════════════════════════════════════════════════════════════");
                _output($"  LUN {lun} ({lunPartitions.Count} partitions)");
                _output($"══════════════════════════════════════════════════════════════════");
                _output($"  {"Name",-18} {"Size",-10} {"FS",-7} {"Format",-7} {"Start LBA",-12}");
                _output($"  {new string('─', 58)}");

                foreach (var p in lunPartitions)
                {
                    string fs = p.FileSystemShort;
                    string format = p.ImageFormatShort;
                    string size = p.SizeFormatted;
                    
                    // 使用颜色标记不同文件系统
                    string fsDisplay = p.FileSystem switch
                    {
                        OPFlashTool.PartitionFileSystem.EXT4 => $"{fs}",
                        OPFlashTool.PartitionFileSystem.EROFS => $"{fs}",
                        OPFlashTool.PartitionFileSystem.F2FS => $"{fs}",
                        _ => fs
                    };

                    _output($"  {p.Name,-18} {size,-10} {fsDisplay,-7} {format,-7} {p.StartLba,-12}");
                }
                NewLine();
            }
        }

        /// <summary>
        /// 输出分区表摘要 (按文件系统分组统计)
        /// </summary>
        public void PrintPartitionSummary(List<OPFlashTool.PartitionInfo> partitions)
        {
            Section("Partition Summary");

            // 统计文件系统
            var fsCounts = partitions
                .GroupBy(p => p.FileSystem)
                .Select(g => new { FS = g.Key, Count = g.Count(), TotalSize = g.Sum(p => (long)p.SizeBytes) })
                .OrderByDescending(x => x.TotalSize);

            _output("  By Filesystem:");
            foreach (var item in fsCounts)
            {
                string fsName = item.FS switch
                {
                    OPFlashTool.PartitionFileSystem.EXT4 => "EXT4",
                    OPFlashTool.PartitionFileSystem.EROFS => "EROFS",
                    OPFlashTool.PartitionFileSystem.F2FS => "F2FS",
                    OPFlashTool.PartitionFileSystem.FAT32 => "FAT32",
                    OPFlashTool.PartitionFileSystem.None => "None",
                    _ => "Unknown"
                };
                _output($"    • {fsName,-10}: {item.Count,3} partitions ({FormatSize(item.TotalSize)})");
            }

            NewLine();

            // 统计镜像格式
            var formatCounts = partitions
                .GroupBy(p => p.ImageFormat)
                .Select(g => new { Format = g.Key, Count = g.Count() });

            _output("  By Format:");
            foreach (var item in formatCounts)
            {
                string formatName = item.Format switch
                {
                    OPFlashTool.PartitionImageFormat.Raw => "Raw",
                    OPFlashTool.PartitionImageFormat.Sparse => "Sparse",
                    _ => "Unknown"
                };
                _output($"    • {formatName,-10}: {item.Count,3} partitions");
            }

            NewLine();

            // 统计 LUN
            _output("  By LUN:");
            foreach (var lun in partitions.Select(p => p.Lun).Distinct().OrderBy(l => l))
            {
                var lunParts = partitions.Where(p => p.Lun == lun).ToList();
                long totalSize = lunParts.Sum(p => (long)p.SizeBytes);
                _output($"    • LUN {lun,-3}: {lunParts.Count,3} partitions ({FormatSize(totalSize)})");
            }
        }

        /// <summary>
        /// 输出关键分区信息 (system, vendor, super 等)
        /// </summary>
        public void PrintKeyPartitions(List<OPFlashTool.PartitionInfo> partitions)
        {
            var keyNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                "boot", "boot_a", "boot_b",
                "recovery", "recovery_a", "recovery_b",
                "system", "system_a", "system_b",
                "vendor", "vendor_a", "vendor_b",
                "product", "product_a", "product_b",
                "super", "userdata", "metadata",
                "vbmeta", "vbmeta_a", "vbmeta_b",
                "dtbo", "dtbo_a", "dtbo_b"
            };

            var keyPartitions = partitions
                .Where(p => keyNames.Contains(p.Name))
                .OrderBy(p => p.Lun)
                .ThenBy(p => p.StartLba)
                .ToList();

            if (!keyPartitions.Any())
                return;

            Section("Key Partitions");
            _output($"  {"Name",-18} {"Size",-10} {"FS",-7} {"Format",-7} {"LUN"}");
            _output($"  {new string('─', 50)}");

            foreach (var p in keyPartitions)
            {
                _output($"  {p.Name,-18} {p.SizeFormatted,-10} {p.FileSystemShort,-7} {p.ImageFormatShort,-7} {p.Lun}");
            }
            NewLine();
        }

        /// <summary>
        /// 分区表来源选择提示
        /// </summary>
        public void PartitionTableSourcePrompt()
        {
            _output("Select partition table source:");
            _output("  1. 📱 Read from Device (Firehose)");
            _output("  2. 📄 Parse from XML (rawprogram*.xml)");
            _output("  3. 💾 Parse from GPT file (gpt_*.bin)");
        }

        /// <summary>
        /// 显示分区文件系统类型信息 (用于 build.prop 读取)
        /// </summary>
        public void PartitionFileSystemInfo(string partitionName, long size, string fsType)
        {
            _output($"  • {partitionName,-12} : {FormatSize(size),-10} [{fsType}]");
        }

        /// <summary>
        /// 输出发现的可读取分区列表
        /// </summary>
        public void FindingTargetPartitions(List<(string name, long size, string fsType)> partitions)
        {
            Section("Finding target to read info");
            foreach (var (name, size, fsType) in partitions)
            {
                PartitionFileSystemInfo(name, size, fsType);
            }
        }

        /// <summary>
        /// 文件系统检测结果
        /// </summary>
        public void FileSystemDetected(string partitionName, string fsType, bool isSparse = false)
        {
            string sparseInfo = isSparse ? " (sparse)" : "";
            _output($"Filesystem on {partitionName}: {fsType}{sparseInfo}");
        }

        /// <summary>
        /// 获取友好的文件系统类型名称
        /// </summary>
        public static string GetFsTypeName(OPFlashTool.Qualcomm.DeviceInfoReader.FileSystemType fsType)
        {
            switch (fsType)
            {
                case OPFlashTool.Qualcomm.DeviceInfoReader.FileSystemType.EXT4:
                    return "EXT4";
                case OPFlashTool.Qualcomm.DeviceInfoReader.FileSystemType.EROFS:
                    return "EROFS";
                case OPFlashTool.Qualcomm.DeviceInfoReader.FileSystemType.F2FS:
                    return "F2FS";
                case OPFlashTool.Qualcomm.DeviceInfoReader.FileSystemType.Sparse:
                    return "Sparse";
                default:
                    return "Unknown";
            }
        }

        #endregion

        #region 镜像处理日志

        /// <summary>
        /// 镜像信息
        /// </summary>
        public void ImageInfo(OPFlashTool.Qualcomm.ImageInfo info)
        {
            Section("Image Info");
            _output($"  Path      : {Path.GetFileName(info.Path)}");
            _output($"  Source    : {info.Source}");
            _output($"  Format    : {info.Format}");
            _output($"  File Size : {FormatSize(info.FileSize)}");
            _output($"  Real Size : {FormatSize(info.ActualSize)}");
            if (info.Format == OPFlashTool.Qualcomm.ImageFormat.Sparse)
                _output($"  Ratio     : {info.CompressionRatio:F2}x");
            _output($"  FS Type   : {info.FileSystemType}");
        }

        /// <summary>
        /// Sparse 头信息
        /// </summary>
        public void SparseHeader(OPFlashTool.Qualcomm.SparseHeader header)
        {
            _output($"Sparse Image v{header.MajorVersion}.{header.MinorVersion}");
            _output($"  Block Size   : {FormatSize(header.BlockSize)}");
            _output($"  Total Blocks : {header.TotalBlocks}");
            _output($"  Total Chunks : {header.TotalChunks}");
            _output($"  Image Size   : {FormatSize(header.TotalSize)}");
        }

        /// <summary>
        /// 镜像转换开始
        /// </summary>
        public void ImageConvertStart(string sourceFormat, string targetFormat, string fileName)
        {
            _output($"Converting {sourceFormat} → {targetFormat}: {fileName}");
        }

        /// <summary>
        /// 镜像转换完成
        /// </summary>
        public void ImageConvertComplete(long inputSize, long outputSize, double seconds)
        {
            double ratio = inputSize > 0 ? (double)outputSize / inputSize : 1.0;
            double speed = inputSize / 1024.0 / 1024.0 / seconds;
            _output($"Convert complete {OK}");
            _output($"  Input  : {FormatSize(inputSize)}");
            _output($"  Output : {FormatSize(outputSize)}");
            _output($"  Ratio  : {ratio:F2}x");
            _output($"  Speed  : {speed:F1} MB/s");
        }

        /// <summary>
        /// 镜像分割
        /// </summary>
        public void ImageSplit(string fileName, int partCount, long partSize)
        {
            _output($"Splitting {fileName} into {partCount} parts ({FormatSize(partSize)} each)");
        }

        /// <summary>
        /// 镜像合并
        /// </summary>
        public void ImageMerge(int partCount, string outputFile)
        {
            _output($"Merging {partCount} parts → {outputFile}");
        }

        /// <summary>
        /// 从设备读取镜像
        /// </summary>
        public void ReadImageFromDevice(string partitionName, string format, long size)
        {
            _output($"Reading {partitionName} as {format} ({FormatSize(size)}) {WAIT}");
        }

        /// <summary>
        /// 写入镜像到设备
        /// </summary>
        public void WriteImageToDevice(string partitionName, string format, long size)
        {
            _output($"Writing {partitionName} as {format} ({FormatSize(size)}) {WAIT}");
        }

        /// <summary>
        /// 镜像格式检测
        /// </summary>
        public void ImageFormatDetected(string fileName, string format, string fsType)
        {
            _output($"Detected: {fileName}");
            _output($"  Format : {format}");
            _output($"  FS     : {fsType}");
        }

        #endregion

        #region 错误和警告

        /// <summary>
        /// 输出错误
        /// </summary>
        public void Error(string message)
        {
            _output($"[ERROR] {message}");
        }

        /// <summary>
        /// 输出警告
        /// </summary>
        public void Warning(string message)
        {
            _output($"[WARNING] {message}");
        }

        /// <summary>
        /// 输出提示
        /// </summary>
        public void Info(string message)
        {
            _output($"[INFO] {message}");
        }

        /// <summary>
        /// 输出调试信息
        /// </summary>
        public void Debug(string message)
        {
            _output($"[DEBUG] {message}");
        }

        #endregion

        #region 辅助方法

        private string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1024 * 1024) return $"{bytes / 1024.0:F2} KB";
            if (bytes < 1024L * 1024 * 1024) return $"{bytes / 1024.0 / 1024.0:F2} MB";
            return $"{bytes / 1024.0 / 1024.0 / 1024.0:F2} GB";
        }

        #endregion

        #region 完整流程模板

        /// <summary>
        /// Qualcomm 完整连接流程日志
        /// </summary>
        public void QualcommConnectSequence(
            Qualcomm.SaharaPblInfo pblInfo,
            Qualcomm.DeviceIdentifyResult identifyResult,
            string loaderPath,
            long loaderSize)
        {
            // 1. 设备检测
            _output("Qualcomm EDL Mode (9008) detected");
            _output($"Handshake device {OK}");
            _output($"Reading device Info {OK}");
            NewLine();

            // 2. 设备信息
            if (identifyResult != null)
            {
                _output($"Qualcomm {identifyResult.StorageType.ToUpper()} Detected!");
            }
            
            QualcommDeviceInfo(
                pblInfo.ChipName,
                pblInfo.MsmId,
                pblInfo.PkHash,
                pblInfo.OemId,
                pblInfo.ModelId,
                pblInfo.Serial,
                (int)pblInfo.SaharaVersion,
                pblInfo.Is64Bit);
            
            NewLine();

            // 3. 设备识别
            if (identifyResult != null && identifyResult.Vendor != "Unknown")
            {
                _output($"Device Vendor :{identifyResult.Vendor}");
                _output($"Device Model :{identifyResult.Model}");
                _output($"Recommended Strategy :{identifyResult.RecommendedStrategy}");
                if (identifyResult.RequiresAuth)
                {
                    _output($"Authentication Required :{identifyResult.AuthType}");
                }
                NewLine();
            }

            // 4. 加载器上传
            _output($"Choosing the right loader {OK}");
            _output($"Sending Firehose Loader : {System.IO.Path.GetFileName(loaderPath)}");
            _output($"Uploading Loader ({FormatSize(loaderSize)}) {OK}");
            _output($"Jumping to Firehose {OK}");
            NewLine();
        }

        /// <summary>
        /// Firehose 配置完成日志
        /// </summary>
        public void FirehoseReadySequence(string storageType, int sectorSize, int payloadSize)
        {
            _output($"Firehose Mode Active");
            _output($"Storage Type :{storageType.ToUpper()}");
            _output($"Sector Size :{sectorSize}");
            _output($"Max Payload :{FormatSize(payloadSize)}");
            _output($"Configure device {OK}");
            NewLine();
        }

        /// <summary>
        /// 输出完整的 Android 设备信息
        /// </summary>
        public void PrintAndroidDeviceInfo(Qualcomm.AndroidBuildProps props)
        {
            if (props == null) return;

            Section("Android OS Info");
            _output($"  • OEM          : {props.Manufacturer}");
            _output($"  • Brand        : {props.Brand}");
            _output($"  • Model        : {props.Model}");
            _output($"  • Device       : {props.Device}");
            _output($"  • Product      : {props.Product}");
            _output($"  • SDK Ver      : {props.SdkVersion}");
            _output($"  • Android Ver  : {props.AndroidVersion}");
            _output($"  • Build ID     : {props.BuildId}");
            _output($"  • Security Ver : {props.SecurityPatch}");
            _output($"  • Incremental  : {props.Incremental}");
            if (!string.IsNullOrEmpty(props.RomVersion))
                _output($"  • ROM Version  : {props.RomVersion}");
            if (!string.IsNullOrEmpty(props.BuildFingerprint))
                _output($"  • Fingerprint  : {props.BuildFingerprint}");
            NewLine();
        }

        #endregion
    }
}
