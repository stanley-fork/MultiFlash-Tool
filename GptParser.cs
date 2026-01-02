using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Linq;
using System.Xml.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace OPFlashTool
{
    /// <summary>
    /// 镜像格式
    /// </summary>
    public enum PartitionImageFormat
    {
        Unknown,
        Raw,
        Sparse
    }

    /// <summary>
    /// 文件系统类型
    /// </summary>
    public enum PartitionFileSystem
    {
        Unknown,
        EXT4,
        EROFS,
        F2FS,
        FAT32,
        NTFS,
        SquashFS,
        None  // 无文件系统 (如 bootloader)
    }

    /// <summary>
    /// 分区信息 (增强版)
    /// </summary>
    public class PartitionInfo
    {
        public int Lun { get; set; }
        public string Name { get; set; } = "";
        public ulong StartLba { get; set; }
        public string StartLbaStr { get; set; } // 支持 XML 中的字符串/公式
        public ulong Sectors { get; set; }
        public int SectorSize { get; set; } = 4096;
        public string FileName { get; set; } = ""; // 关联的镜像文件名

        // 新增属性
        public PartitionImageFormat ImageFormat { get; set; } = PartitionImageFormat.Unknown;
        public PartitionFileSystem FileSystem { get; set; } = PartitionFileSystem.Unknown;
        public string Label { get; set; } = "";  // 分区标签
        public bool IsReadOnly { get; set; }
        public bool IsMounted { get; set; }
        public string MountPoint { get; set; } = "";
        
        // 来源信息
        public PartitionSource Source { get; set; } = PartitionSource.Unknown;
        public string SourceFile { get; set; } = ""; // XML 文件路径或设备标识

        public ulong EndLba => StartLba + Sectors - 1;
        public ulong SizeBytes => Sectors * (ulong)SectorSize;
        public double SizeKb => SizeBytes / 1024.0;
        public double SizeMb => SizeKb / 1024.0;
        public double SizeGb => SizeMb / 1024.0;

        /// <summary>
        /// 获取格式化的大小字符串
        /// </summary>
        public string SizeFormatted
        {
            get
            {
                if (SizeGb >= 1) return $"{SizeGb:F2} GB";
                if (SizeMb >= 1) return $"{SizeMb:F2} MB";
                if (SizeKb >= 1) return $"{SizeKb:F2} KB";
                return $"{SizeBytes} B";
            }
        }

        /// <summary>
        /// 获取文件系统的简短名称
        /// </summary>
        public string FileSystemShort
        {
            get
            {
                switch (FileSystem)
                {
                    case PartitionFileSystem.EXT4: return "EXT4";
                    case PartitionFileSystem.EROFS: return "EROFS";
                    case PartitionFileSystem.F2FS: return "F2FS";
                    case PartitionFileSystem.FAT32: return "FAT32";
                    case PartitionFileSystem.NTFS: return "NTFS";
                    case PartitionFileSystem.SquashFS: return "SquashFS";
                    case PartitionFileSystem.None: return "-";
                    case PartitionFileSystem.Unknown: return "-";
                    default: return "-";
                }
            }
        }

        /// <summary>
        /// 获取镜像格式的简短名称
        /// </summary>
        public string ImageFormatShort
        {
            get
            {
                switch (ImageFormat)
                {
                    case PartitionImageFormat.Raw: return "Raw";
                    case PartitionImageFormat.Sparse: return "Sparse";
                    case PartitionImageFormat.Unknown: return "-";
                    default: return "-";
                }
            }
        }

        public override string ToString()
        {
            return $"{Name} @ LUN{Lun} [{SizeFormatted}] {FileSystemShort} ({ImageFormatShort})";
        }
    }

    /// <summary>
    /// 分区表来源
    /// </summary>
    public enum PartitionSource
    {
        Unknown,
        Device,     // 从设备读取
        XmlFile,    // 从 XML 文件解析
        GptFile,    // 从 GPT 文件解析
        Manual      // 手动添加
    }

    /// <summary>
    /// 分区表管理器 - 支持多种来源
    /// </summary>
    public class PartitionTableManager
    {
        private List<PartitionInfo> _partitions = new List<PartitionInfo>();
        private Action<string> _log;
        private Qualcomm.FirehoseClient _firehose;
        private Qualcomm.SparseImageHandler _sparseHandler;

        public IReadOnlyList<PartitionInfo> Partitions => _partitions.AsReadOnly();
        public PartitionSource CurrentSource { get; private set; } = PartitionSource.Unknown;
        public string SourcePath { get; private set; } = "";
        public int SectorSize { get; set; } = 4096;

        public PartitionTableManager(Action<string> logger = null)
        {
            _log = logger ?? Console.WriteLine;
            _sparseHandler = new Qualcomm.SparseImageHandler(_log);
        }

        public PartitionTableManager(Qualcomm.FirehoseClient firehose, Action<string> logger = null)
        {
            _firehose = firehose;
            _log = logger ?? Console.WriteLine;
            _sparseHandler = new Qualcomm.SparseImageHandler(firehose, _log);
        }

        #region 从 XML 文件加载

        /// <summary>
        /// 从 rawprogram XML 文件加载分区表
        /// </summary>
        public List<PartitionInfo> LoadFromXml(string xmlPath)
        {
            _partitions.Clear();
            CurrentSource = PartitionSource.XmlFile;
            SourcePath = xmlPath;

            try
            {
                var doc = XDocument.Load(xmlPath);
                var root = doc.Root;

                if (root == null)
                {
                    _log("[XML] 无效的 XML 文件");
                    return _partitions;
                }

                // 查找所有 program 元素
                var programs = root.Descendants("program");
                
                foreach (var prog in programs)
                {
                    var partition = ParseProgramElement(prog);
                    if (partition != null)
                    {
                        partition.Source = PartitionSource.XmlFile;
                        partition.SourceFile = xmlPath;
                        _partitions.Add(partition);
                    }
                }

                _log($"[XML] 从 {Path.GetFileName(xmlPath)} 加载了 {_partitions.Count} 个分区");
            }
            catch (Exception ex)
            {
                _log($"[XML] 解析错误: {ex.Message}");
            }

            return _partitions;
        }

        /// <summary>
        /// 从多个 rawprogram XML 文件加载 (支持多 LUN)
        /// </summary>
        public List<PartitionInfo> LoadFromXmlFiles(IEnumerable<string> xmlPaths)
        {
            _partitions.Clear();
            CurrentSource = PartitionSource.XmlFile;

            foreach (var xmlPath in xmlPaths)
            {
                if (!File.Exists(xmlPath)) continue;

                try
                {
                    var doc = XDocument.Load(xmlPath);
                    var programs = doc.Root?.Descendants("program") ?? Enumerable.Empty<XElement>();

                    // 尝试从文件名提取 LUN 号 (如 rawprogram0.xml, rawprogram_unsparse1.xml)
                    int defaultLun = ExtractLunFromFileName(xmlPath);

                    foreach (var prog in programs)
                    {
                        var partition = ParseProgramElement(prog, defaultLun);
                        if (partition != null)
                        {
                            partition.Source = PartitionSource.XmlFile;
                            partition.SourceFile = xmlPath;
                            _partitions.Add(partition);
                        }
                    }
                }
                catch (Exception ex)
                {
                    _log($"[XML] 解析 {Path.GetFileName(xmlPath)} 失败: {ex.Message}");
                }
            }

            _log($"[XML] 共加载 {_partitions.Count} 个分区");
            SourcePath = string.Join(";", xmlPaths);
            return _partitions;
        }

        private PartitionInfo ParseProgramElement(XElement prog, int defaultLun = 0)
        {
            string label = prog.Attribute("label")?.Value ?? "";
            string filename = prog.Attribute("filename")?.Value ?? "";
            
            // 跳过没有标签或空白条目
            if (string.IsNullOrWhiteSpace(label) && string.IsNullOrWhiteSpace(filename))
                return null;

            var partition = new PartitionInfo
            {
                Name = label,
                FileName = filename,
                Label = label
            };

            // 解析 LUN
            string lunStr = prog.Attribute("physical_partition_number")?.Value ?? defaultLun.ToString();
            if (int.TryParse(lunStr, out int lun))
                partition.Lun = lun;

            // 解析起始扇区 (可能是公式)
            string startSector = prog.Attribute("start_sector")?.Value ?? "0";
            partition.StartLbaStr = startSector;
            if (ulong.TryParse(startSector, out ulong startLba))
                partition.StartLba = startLba;

            // 解析扇区数
            string numSectors = prog.Attribute("num_partition_sectors")?.Value ?? "0";
            if (ulong.TryParse(numSectors, out ulong sectors))
                partition.Sectors = sectors;

            // 解析扇区大小
            string sectorSizeStr = prog.Attribute("SECTOR_SIZE_IN_BYTES")?.Value ?? SectorSize.ToString();
            if (int.TryParse(sectorSizeStr, out int sectorSize))
                partition.SectorSize = sectorSize;

            // 检查是否 sparse
            string sparse = prog.Attribute("sparse")?.Value ?? "false";
            partition.ImageFormat = sparse.ToLower() == "true" ? 
                PartitionImageFormat.Sparse : PartitionImageFormat.Raw;

            // 只读标记
            string readOnly = prog.Attribute("readonly")?.Value ?? "false";
            partition.IsReadOnly = readOnly.ToLower() == "true";

            return partition;
        }

        private int ExtractLunFromFileName(string filePath)
        {
            string fileName = Path.GetFileNameWithoutExtension(filePath);
            // 尝试匹配 rawprogram0, rawprogram_unsparse1 等模式
            for (int i = fileName.Length - 1; i >= 0; i--)
            {
                if (char.IsDigit(fileName[i]))
                {
                    // 找到最后一个数字
                    int end = i;
                    while (i > 0 && char.IsDigit(fileName[i - 1]))
                        i--;
                    if (int.TryParse(fileName.Substring(i, end - i + 1), out int lun))
                        return lun;
                }
            }
            return 0;
        }

        #endregion

        #region 从 GPT 文件加载

        /// <summary>
        /// 从 GPT 二进制文件加载分区表
        /// </summary>
        public List<PartitionInfo> LoadFromGptFile(string gptPath, int lunId = 0)
        {
            _partitions.Clear();
            CurrentSource = PartitionSource.GptFile;
            SourcePath = gptPath;

            var parsed = GptParser.ParseGptFile(gptPath, lunId);
            foreach (var p in parsed)
            {
                p.Source = PartitionSource.GptFile;
                p.SourceFile = gptPath;
                _partitions.Add(p);
            }

            _log($"[GPT] 从 {Path.GetFileName(gptPath)} 加载了 {_partitions.Count} 个分区");
            return _partitions;
        }

        /// <summary>
        /// 从多个 GPT 文件加载 (多 LUN)
        /// </summary>
        public List<PartitionInfo> LoadFromGptFiles(Dictionary<int, string> lunGptPaths)
        {
            _partitions.Clear();
            CurrentSource = PartitionSource.GptFile;

            foreach (var kvp in lunGptPaths)
            {
                int lun = kvp.Key;
                string gptPath = kvp.Value;

                if (!File.Exists(gptPath)) continue;

                var parsed = GptParser.ParseGptFile(gptPath, lun);
                foreach (var p in parsed)
                {
                    p.Source = PartitionSource.GptFile;
                    p.SourceFile = gptPath;
                    _partitions.Add(p);
                }
            }

            _log($"[GPT] 共加载 {_partitions.Count} 个分区");
            return _partitions;
        }

        #endregion

        #region 从设备读取

        /// <summary>
        /// 从设备读取分区表 (通过 Firehose)
        /// </summary>
        public async Task<List<PartitionInfo>> LoadFromDeviceAsync(int maxLuns = 6, CancellationToken ct = default)
        {
            if (_firehose == null)
            {
                _log("[Device] 未连接设备");
                return _partitions;
            }

            _partitions.Clear();
            CurrentSource = PartitionSource.Device;
            SourcePath = "Device";

            string tempDir = Path.Combine(Path.GetTempPath(), $"gpt_{Guid.NewGuid():N}");
            Directory.CreateDirectory(tempDir);

            try
            {
                for (int lun = 0; lun < maxLuns; lun++)
                {
                    if (ct.IsCancellationRequested) break;

                    string gptPath = Path.Combine(tempDir, $"gpt_lun{lun}.bin");

                    // 读取 GPT (通常是前 34 个扇区)
                    bool success = await _firehose.ReadPartitionChunkedAsync(
                        gptPath, "0", 34, lun.ToString(),
                        null, ct, $"GPT_LUN{lun}", null, false, true);

                    if (!success || !File.Exists(gptPath))
                    {
                        _log($"[Device] LUN{lun} 无 GPT 或读取失败");
                        continue;
                    }

                    var parsed = GptParser.ParseGptFile(gptPath, lun);
                    foreach (var p in parsed)
                    {
                        p.Source = PartitionSource.Device;
                        p.SourceFile = $"LUN{lun}";
                        _partitions.Add(p);
                    }

                    _log($"[Device] LUN{lun}: {parsed.Count} 个分区");
                }
            }
            finally
            {
                try { Directory.Delete(tempDir, true); } catch { }
            }

            _log($"[Device] 共读取 {_partitions.Count} 个分区");
            return _partitions;
        }

        #endregion

        #region 文件系统检测

        /// <summary>
        /// 检测所有分区的文件系统类型 (从镜像文件)
        /// </summary>
        public void DetectFileSystemsFromImages(string imagesDirectory)
        {
            foreach (var partition in _partitions)
            {
                if (string.IsNullOrEmpty(partition.FileName))
                    continue;

                string imagePath = Path.Combine(imagesDirectory, partition.FileName);
                if (!File.Exists(imagePath))
                    continue;

                DetectPartitionFormat(partition, imagePath);
            }
        }

        /// <summary>
        /// 检测单个分区的格式和文件系统
        /// </summary>
        public void DetectPartitionFormat(PartitionInfo partition, string imagePath)
        {
            if (!File.Exists(imagePath))
                return;

            try
            {
                var imageInfo = _sparseHandler.GetImageInfo(imagePath);
                partition.ImageFormat = imageInfo.Format == Qualcomm.ImageFormat.Sparse ? 
                    PartitionImageFormat.Sparse : PartitionImageFormat.Raw;
                partition.FileSystem = ConvertFileSystem(imageInfo.FileSystemType);
            }
            catch (Exception ex)
            {
                _log($"[检测] {partition.Name} 格式检测失败: {ex.Message}");
            }
        }

        /// <summary>
        /// 从设备检测分区的文件系统类型
        /// </summary>
        public async Task DetectFileSystemFromDeviceAsync(PartitionInfo partition, CancellationToken ct = default)
        {
            if (_firehose == null || partition.Sectors == 0)
                return;

            string tempFile = Path.Combine(Path.GetTempPath(), $"fsdetect_{Guid.NewGuid():N}.bin");

            try
            {
                // 只读取前 8KB 用于检测
                int sectorsToRead = Math.Min(16, (int)partition.Sectors);
                
                bool success = await _firehose.ReadPartitionChunkedAsync(
                    tempFile,
                    partition.StartLba.ToString(),
                    sectorsToRead,
                    partition.Lun.ToString(),
                    null, ct,
                    partition.Name,
                    null, false, true);

                if (success && File.Exists(tempFile))
                {
                    byte[] data = File.ReadAllBytes(tempFile);
                    var fsType = _sparseHandler.DetectFileSystemType(data);
                    partition.FileSystem = ConvertFileSystem(fsType);
                    partition.ImageFormat = PartitionImageFormat.Raw; // 从设备读取的都是 Raw
                }
            }
            finally
            {
                try { if (File.Exists(tempFile)) File.Delete(tempFile); } catch { }
            }
        }

        /// <summary>
        /// 批量从设备检测文件系统
        /// </summary>
        public async Task DetectAllFileSystemsFromDeviceAsync(
            IEnumerable<string> partitionNames = null, 
            CancellationToken ct = default)
        {
            var targets = partitionNames == null ? 
                _partitions : 
                _partitions.Where(p => partitionNames.Contains(p.Name, StringComparer.OrdinalIgnoreCase));

            foreach (var partition in targets)
            {
                if (ct.IsCancellationRequested) break;
                await DetectFileSystemFromDeviceAsync(partition, ct);
            }
        }

        private PartitionFileSystem ConvertFileSystem(Qualcomm.DeviceInfoReader.FileSystemType fsType)
        {
            switch (fsType)
            {
                case Qualcomm.DeviceInfoReader.FileSystemType.EXT4:
                    return PartitionFileSystem.EXT4;
                case Qualcomm.DeviceInfoReader.FileSystemType.EROFS:
                    return PartitionFileSystem.EROFS;
                case Qualcomm.DeviceInfoReader.FileSystemType.F2FS:
                    return PartitionFileSystem.F2FS;
                default:
                    return PartitionFileSystem.Unknown;
            }
        }

        #endregion

        #region 查询方法

        /// <summary>
        /// 按名称查找分区
        /// </summary>
        public PartitionInfo FindByName(string name)
        {
            return _partitions.FirstOrDefault(p => 
                p.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>
        /// 按 LUN 获取分区
        /// </summary>
        public IEnumerable<PartitionInfo> GetByLun(int lun)
        {
            return _partitions.Where(p => p.Lun == lun);
        }

        /// <summary>
        /// 获取所有 LUN 编号
        /// </summary>
        public IEnumerable<int> GetAllLuns()
        {
            return _partitions.Select(p => p.Lun).Distinct().OrderBy(l => l);
        }

        /// <summary>
        /// 获取指定文件系统类型的分区
        /// </summary>
        public IEnumerable<PartitionInfo> GetByFileSystem(PartitionFileSystem fs)
        {
            return _partitions.Where(p => p.FileSystem == fs);
        }

        /// <summary>
        /// 获取系统分区列表 (system, vendor, product, super 等)
        /// </summary>
        public IEnumerable<PartitionInfo> GetSystemPartitions()
        {
            var systemNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            {
                "system", "system_a", "system_b",
                "vendor", "vendor_a", "vendor_b",
                "product", "product_a", "product_b",
                "system_ext", "system_ext_a", "system_ext_b",
                "odm", "odm_a", "odm_b",
                "super"
            };
            return _partitions.Where(p => systemNames.Contains(p.Name));
        }

        #endregion

        #region 输出

        /// <summary>
        /// 获取分区表的格式化字符串
        /// </summary>
        public string ToFormattedString(bool includeHeader = true)
        {
            var sb = new StringBuilder();
            
            if (includeHeader)
            {
                sb.AppendLine($"Source: {CurrentSource} ({SourcePath})");
                sb.AppendLine($"Total Partitions: {_partitions.Count}");
                sb.AppendLine();
            }

            // 按 LUN 分组显示
            foreach (var lun in GetAllLuns())
            {
                sb.AppendLine($"═══ LUN {lun} ═══");
                sb.AppendLine($"{"Name",-20} {"Start",-12} {"Size",-12} {"FS",-8} {"Format",-8}");
                sb.AppendLine(new string('─', 64));

                foreach (var p in GetByLun(lun).OrderBy(p => p.StartLba))
                {
                    sb.AppendLine($"{p.Name,-20} {p.StartLba,-12} {p.SizeFormatted,-12} {p.FileSystemShort,-8} {p.ImageFormatShort,-8}");
                }
                sb.AppendLine();
            }

            return sb.ToString();
        }

        #endregion
    }

    // --- 以下参照 gpttool 定义的底层结构 ---

    public class GPT_Header
    {
        public ulong Signature;      // "EFI PART" (8 bytes)
        public uint Revision;        // 0x00010000
        public uint HeaderSize;      // usually 92
        public uint HeaderCRC32;
        public uint Reserved;
        public ulong MyLBA;
        public ulong AlternateLBA;
        public ulong FirstUsableLBA;
        public ulong LastUsableLBA;
        public byte[] DiskGUID = new byte[16];
        public ulong PartitionEntryLBA;
        public uint NumberOfPartitionEntries;
        public uint SizeOfPartitionEntry;
        public uint PartitionEntryArrayCRC32;
        // Remaining bytes are reserved/padding
    }

    public class GPT_Entry
    {
        public byte[] PartitionTypeGUID = new byte[16];
        public byte[] UniquePartitionGUID = new byte[16];
        public ulong StartingLBA;
        public ulong EndingLBA;
        public ulong Attributes;
        public string PartitionName = ""; // 72 bytes (36 UTF-16 chars)
    }

    public static class GptParser
    {
        // 签名: "EFI PART"
        private const ulong GPT_SIGNATURE = 0x5452415020494645; 

        public static List<PartitionInfo> ParseGptFile(string filePath, int lunId)
        {
            if (!File.Exists(filePath)) return new List<PartitionInfo>();
            byte[] data = File.ReadAllBytes(filePath);
            return ParseGptBytes(data, lunId);
        }

        public static List<PartitionInfo> ParseGptBytes(byte[] data, int lunId)
        {
            var partitions = new List<PartitionInfo>();
            int sectorSize = 0;
            int headerOffset = 0;

            // 1. 自动探测扇区大小 (UFS:4096 / eMMC:512)
            // GPT Header 始终位于 LBA 1
            if (CheckSignature(data, 4096)) 
            {
                sectorSize = 4096;
                headerOffset = 4096;
            }
            else if (CheckSignature(data, 512))
            {
                sectorSize = 512;
                headerOffset = 512;
            }
            else
            {
                // 无效 GPT
                return partitions;
            }

            using (var ms = new MemoryStream(data))
            using (var br = new BinaryReader(ms))
            {
                // 2. 解析 Header
                ms.Seek(headerOffset, SeekOrigin.Begin);
                GPT_Header header = ReadHeader(br);

                // (可选) 可以在这里校验 header.HeaderCRC32

                // 3. 定位分区表数组
                // 通常 EntryArray 在 LBA 2，即 headerOffset + sectorSize
                // 但最严谨的做法是使用 Header 中的 PartitionEntryLBA * SectorSize
                long entryStartPos = (long)(header.PartitionEntryLBA * (ulong)sectorSize);
                
                // 如果计算出的偏移量超出了文件范围，尝试回退到标准位置 (LBA 2)
                if (entryStartPos >= data.Length) 
                {
                    entryStartPos = headerOffset + sectorSize;
                }

                ms.Seek(entryStartPos, SeekOrigin.Begin);

                // 4. 遍历分区条目
                for (int i = 0; i < header.NumberOfPartitionEntries; i++)
                {
                    // 记录当前流位置，确保读取固定大小
                    long currentEntryPos = ms.Position;

                    // 读取单个条目
                    byte[] typeGuid = br.ReadBytes(16);
                    byte[] uniqueGuid = br.ReadBytes(16);
                    
                    // [修改] 检查前 32 字节 (TypeGUID + UniqueGUID) 是否全为 0
                    // 如果全为 0，说明是未使用条目
                    if (IsZeroBytes(typeGuid) && IsZeroBytes(uniqueGuid))
                    {
                        // 跳过这 128 字节 (或 SizeOfPartitionEntry 字节)
                        ms.Seek(currentEntryPos + header.SizeOfPartitionEntry, SeekOrigin.Begin);
                        continue;
                    }

                    ulong firstLba = br.ReadUInt64();
                    ulong lastLba = br.ReadUInt64();
                    ulong attributes = br.ReadUInt64();
                    byte[] nameBytes = br.ReadBytes(72); // 36 chars * 2

                    // 解析名称 (去除尾部 \0)
                    string name = Encoding.Unicode.GetString(nameBytes).TrimEnd('\0');

                    // 添加到列表
                    partitions.Add(new PartitionInfo
                    {
                        Lun = lunId,
                        Name = name,
                        StartLba = firstLba,
                        StartLbaStr = firstLba.ToString(), // [新增] 填充字符串形式
                        Sectors = lastLba - firstLba + 1,
                        SectorSize = sectorSize,
                        FileName = "" // 默认不关联文件名，避免误报缺失
                    });

                    // 确保指针移动到下一个条目的正确位置
                    ms.Seek(currentEntryPos + header.SizeOfPartitionEntry, SeekOrigin.Begin);
                }
            }

            return partitions;
        }

        private static GPT_Header ReadHeader(BinaryReader br)
        {
            var h = new GPT_Header();
            h.Signature = br.ReadUInt64();
            h.Revision = br.ReadUInt32();
            h.HeaderSize = br.ReadUInt32();
            h.HeaderCRC32 = br.ReadUInt32();
            h.Reserved = br.ReadUInt32();
            h.MyLBA = br.ReadUInt64();
            h.AlternateLBA = br.ReadUInt64();
            h.FirstUsableLBA = br.ReadUInt64();
            h.LastUsableLBA = br.ReadUInt64();
            h.DiskGUID = br.ReadBytes(16);
            h.PartitionEntryLBA = br.ReadUInt64();
            h.NumberOfPartitionEntries = br.ReadUInt32();
            h.SizeOfPartitionEntry = br.ReadUInt32();
            h.PartitionEntryArrayCRC32 = br.ReadUInt32();
            return h;
        }

        private static bool CheckSignature(byte[] data, int offset)
        {
            if (data.Length < offset + 8) return false;
            // "EFI PART" 
            return BitConverter.ToUInt64(data, offset) == GPT_SIGNATURE;
        }

        private static bool IsZeroBytes(byte[] data)
        {
            for (int i = 0; i < data.Length; i++)
            {
                if (data[i] != 0) return false;
            }
            return true;
        }
    }

    // --- 移植 gpttool 的 CRC32 算法 (为未来修改 GPT 做准备) ---
    public class Crc32
    {
        private static readonly uint[] Table;

        static Crc32()
        {
            uint poly = 0x04c11db7;
            Table = new uint[256];
            uint temp = 0;
            for (uint i = 0; i < Table.Length; ++i)
            {
                temp = i;
                for (int j = 8; j > 0; --j)
                {
                    if ((temp & 1) == 1)
                        temp = (uint)((temp >> 1) ^ poly);
                    else
                        temp >>= 1;
                }
                Table[i] = temp;
            }
        }

        public static uint ComputeChecksum(byte[] bytes)
        {
            uint crc = 0xffffffff;
            for (int i = 0; i < bytes.Length; ++i)
            {
                byte index = (byte)((crc & 0xff) ^ bytes[i]);
                crc = (uint)((crc >> 8) ^ Table[index]);
            }
            return ~crc;
        }
    }
}
