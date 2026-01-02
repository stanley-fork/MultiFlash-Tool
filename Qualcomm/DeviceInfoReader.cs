using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OPFlashTool.Qualcomm
{
    /// <summary>
    /// Android 设备属性信息
    /// </summary>
    public class AndroidBuildProps
    {
        public string Brand { get; set; } = "";
        public string Model { get; set; } = "";
        public string Device { get; set; } = "";
        public string Product { get; set; } = "";
        public string Manufacturer { get; set; } = "";
        public string AndroidVersion { get; set; } = "";
        public string SdkVersion { get; set; } = "";
        public string SecurityPatch { get; set; } = "";
        public string BuildId { get; set; } = "";
        public string BuildFingerprint { get; set; } = "";
        public string BuildDescription { get; set; } = "";
        public string Incremental { get; set; } = "";
        public string RomVersion { get; set; } = "";
        public string BaseOs { get; set; } = "";
        
        // 额外属性
        public Dictionary<string, string> AllProps { get; set; } = new Dictionary<string, string>();

        public override string ToString()
        {
            var sb = new StringBuilder();
            sb.AppendLine($"品牌: {Brand}");
            sb.AppendLine($"型号: {Model}");
            sb.AppendLine($"设备: {Device}");
            sb.AppendLine($"制造商: {Manufacturer}");
            sb.AppendLine($"Android: {AndroidVersion} (SDK {SdkVersion})");
            sb.AppendLine($"安全补丁: {SecurityPatch}");
            sb.AppendLine($"Build ID: {BuildId}");
            sb.AppendLine($"ROM: {RomVersion}");
            return sb.ToString();
        }
    }

    /// <summary>
    /// Super (LP) 分区元数据
    /// </summary>
    public class LpMetadata
    {
        public uint Magic { get; set; }
        public uint MajorVersion { get; set; }
        public uint MinorVersion { get; set; }
        public uint HeaderSize { get; set; }
        public uint PartitionTableSize { get; set; }
        public List<LpPartitionEntry> Partitions { get; set; } = new List<LpPartitionEntry>();
    }

    public class LpPartitionEntry
    {
        public string Name { get; set; }
        public uint GroupIndex { get; set; }
        public uint Attributes { get; set; }
        public long FirstExtentIndex { get; set; }
        public uint NumExtents { get; set; }
        public long Size { get; set; }
        public long Offset { get; set; } // 相对于 super 分区起始的偏移
    }

    /// <summary>
    /// Firehose 模式设备信息读取器
    /// 支持从 system/super 分区读取 build.prop
    /// </summary>
    public class DeviceInfoReader
    {
        private FirehoseClient _firehose;
        private Action<string> _log;
        private int _sectorSize;

        // LP (Super) 分区魔数 (来自 AOSP metadata_format.h)
        private const uint LP_GEOMETRY_MAGIC = 0x616c4467;   // "gDla" - LP_METADATA_GEOMETRY_MAGIC
        private const uint LP_METADATA_MAGIC = 0x414C5030;   // "0PLA" - LP_METADATA_HEADER_MAGIC
        
        // 文件系统魔数
        private const ushort EXT4_MAGIC = 0xEF53;              // EXT4 @ offset 0x438 (1024+0x38)
        private const uint EROFS_MAGIC = 0xE0F5E1E2;           // EROFS @ offset 1024
        private const uint F2FS_MAGIC = 0xF2F52010;            // F2FS @ offset 1024
        
        // Sparse 镜像魔数
        private const uint SPARSE_MAGIC = 0xED26FF3A;
        
        // 文件系统类型枚举
        public enum FileSystemType
        {
            Unknown,
            EXT4,
            EROFS,
            F2FS,
            Sparse
        }

        public DeviceInfoReader(FirehoseClient firehose, Action<string> logger = null)
        {
            _firehose = firehose;
            _log = logger ?? Console.WriteLine;
            _sectorSize = firehose.SectorSize > 0 ? firehose.SectorSize : 4096;
        }

        #region 主要功能方法

        /// <summary>
        /// 自动读取 build.prop (优先从 super，其次从 system)
        /// </summary>
        public async Task<AndroidBuildProps> ReadBuildPropsAsync(List<PartitionInfo> partitions, CancellationToken ct)
        {
            _log("[DeviceInfo] 开始读取设备属性...");

            // 1. 优先尝试从 super 分区读取
            var superPart = partitions.FirstOrDefault(p => 
                p.Name.Equals("super", StringComparison.OrdinalIgnoreCase));
            
            if (superPart != null)
            {
                _log("[DeviceInfo] 检测到 super 分区，尝试解析 LP 格式...");
                var props = await ReadFromSuperPartitionAsync(superPart, ct);
                if (props != null && !string.IsNullOrEmpty(props.Brand))
                {
                    return props;
                }
            }

            // 2. 尝试从 system 分区读取
            var systemPart = partitions.FirstOrDefault(p => 
                p.Name.Equals("system", StringComparison.OrdinalIgnoreCase) ||
                p.Name.Equals("system_a", StringComparison.OrdinalIgnoreCase));
            
            if (systemPart != null)
            {
                _log("[DeviceInfo] 尝试从 system 分区读取...");
                var props = await ReadFromSystemPartitionAsync(systemPart, ct);
                if (props != null && !string.IsNullOrEmpty(props.Brand))
                {
                    return props;
                }
            }

            // 3. 尝试从 vendor 分区读取
            var vendorPart = partitions.FirstOrDefault(p => 
                p.Name.Equals("vendor", StringComparison.OrdinalIgnoreCase) ||
                p.Name.Equals("vendor_a", StringComparison.OrdinalIgnoreCase));
            
            if (vendorPart != null)
            {
                _log("[DeviceInfo] 尝试从 vendor 分区读取...");
                var props = await ReadFromVendorPartitionAsync(vendorPart, ct);
                if (props != null)
                {
                    return props;
                }
            }

            _log("[DeviceInfo] 无法读取 build.prop");
            return new AndroidBuildProps();
        }

        /// <summary>
        /// 从 super 分区读取 build.prop (LP 格式)
        /// </summary>
        public async Task<AndroidBuildProps> ReadFromSuperPartitionAsync(PartitionInfo superPart, CancellationToken ct)
        {
            try
            {
                // 1. 读取 LP 元数据
                _log("[LP] 读取 super 分区元数据...");
                var lpMetadata = await ReadLpMetadataAsync(superPart, ct);
                
                if (lpMetadata == null || lpMetadata.Partitions.Count == 0)
                {
                    _log("[LP] 无法解析 LP 元数据");
                    return null;
                }

                _log($"[LP] 检测到 {lpMetadata.Partitions.Count} 个逻辑分区");
                foreach (var p in lpMetadata.Partitions)
                {
                    _log($"  -> {p.Name}: Offset={p.Offset}, Size={FormatSize(p.Size)}");
                }

                // 2. 查找 system 子分区
                var systemLp = lpMetadata.Partitions.FirstOrDefault(p =>
                    p.Name.Equals("system", StringComparison.OrdinalIgnoreCase) ||
                    p.Name.Equals("system_a", StringComparison.OrdinalIgnoreCase));

                if (systemLp == null)
                {
                    _log("[LP] 未找到 system 逻辑分区");
                    return null;
                }

                // 3. 读取 system 子分区中的 build.prop
                _log($"[LP] 从 system 子分区读取 build.prop (Offset: {systemLp.Offset})...");
                
                // 计算绝对偏移
                long absoluteSector = (long)superPart.StartLba + (systemLp.Offset / _sectorSize);
                
                return await ReadBuildPropFromExt4Async(absoluteSector, systemLp.Size, superPart.Lun, ct);
            }
            catch (Exception ex)
            {
                _log($"[LP] 读取失败: {ex.Message}");
                return null;
            }
        }

        /// <summary>
        /// 从 system 分区读取 build.prop (直接 ext4)
        /// </summary>
        public async Task<AndroidBuildProps> ReadFromSystemPartitionAsync(PartitionInfo systemPart, CancellationToken ct)
        {
            try
            {
                _log("[EXT4] 读取 system 分区...");
                return await ReadBuildPropFromExt4Async(
                    (long)systemPart.StartLba, 
                    (long)systemPart.Sectors * _sectorSize,
                    systemPart.Lun, 
                    ct);
            }
            catch (Exception ex)
            {
                _log($"[EXT4] 读取失败: {ex.Message}");
                return null;
            }
        }

        /// <summary>
        /// 从 vendor 分区读取 build.prop
        /// </summary>
        public async Task<AndroidBuildProps> ReadFromVendorPartitionAsync(PartitionInfo vendorPart, CancellationToken ct)
        {
            try
            {
                _log("[EXT4] 读取 vendor 分区...");
                return await ReadBuildPropFromExt4Async(
                    (long)vendorPart.StartLba, 
                    (long)vendorPart.Sectors * _sectorSize,
                    vendorPart.Lun, 
                    ct,
                    "vendor/build.prop");
            }
            catch (Exception ex)
            {
                _log($"[EXT4] 读取失败: {ex.Message}");
                return null;
            }
        }

        #endregion

        #region LP (Super) 分区解析

        /// <summary>
        /// 读取 LP 分区元数据
        /// </summary>
        private async Task<LpMetadata> ReadLpMetadataAsync(PartitionInfo superPart, CancellationToken ct)
        {
            // LP 元数据通常在 super 分区的开头
            // 结构: 
            //   Offset 0: LP_METADATA_GEOMETRY (4KB)
            //   Offset 4096: LP_METADATA_HEADER
            //   Offset 8192: LP_PARTITION_TABLE

            string tempFile = Path.Combine(Path.GetTempPath(), $"lp_meta_{Guid.NewGuid():N}.bin");
            
            try
            {
                // 读取前 64KB (足够容纳元数据)
                int sectorsToRead = 64 * 1024 / _sectorSize;
                
                bool success = await _firehose.ReadPartitionChunkedAsync(
                    tempFile,
                    superPart.StartLba.ToString(),
                    sectorsToRead,
                    superPart.Lun.ToString(),
                    null,
                    ct,
                    "super",
                    "super",
                    false,
                    true
                );

                if (!success || !File.Exists(tempFile))
                {
                    return null;
                }

                byte[] data = File.ReadAllBytes(tempFile);
                return ParseLpMetadata(data);
            }
            finally
            {
                try { if (File.Exists(tempFile)) File.Delete(tempFile); } catch { }
            }
        }

        /// <summary>
        /// 解析 LP 元数据
        /// </summary>
        private LpMetadata ParseLpMetadata(byte[] data)
        {
            if (data == null || data.Length < 8192)
                return null;

            var metadata = new LpMetadata();

            // 查找 LP_METADATA_HEADER (可能在 offset 4096 或 8192)
            int headerOffset = -1;
            for (int offset = 0; offset < Math.Min(data.Length - 4, 16384); offset += 4096)
            {
                uint magic = BitConverter.ToUInt32(data, offset);
                if (magic == LP_METADATA_MAGIC)
                {
                    headerOffset = offset;
                    _log($"[LP] 找到 Metadata Header @ offset {offset}");
                    break;
                }
            }

            if (headerOffset < 0)
            {
                // 尝试查找 Geometry magic
                for (int offset = 0; offset < Math.Min(data.Length - 4, 8192); offset += 4096)
                {
                    uint magic = BitConverter.ToUInt32(data, offset);
                    if (magic == LP_GEOMETRY_MAGIC)
                    {
                        // Geometry 之后是 Metadata Header
                        headerOffset = offset + 4096;
                        break;
                    }
                }
            }

            if (headerOffset < 0)
            {
                _log("[LP] 未找到 LP 元数据魔数");
                return null;
            }

            // 解析 LP_METADATA_HEADER
            // 结构 (简化):
            // 0x00: magic (4)
            // 0x04: major_version (2)
            // 0x06: minor_version (2)
            // 0x08: header_size (4)
            // 0x0C: header_checksum (32)
            // 0x2C: tables_size (4)
            // 0x30: tables_checksum (32)
            // 0x50: partitions offset (4)
            // 0x54: partitions size (4)
            // 0x58: partitions entry size (4)
            // 0x5C: partitions count (4)

            metadata.Magic = BitConverter.ToUInt32(data, headerOffset);
            metadata.MajorVersion = BitConverter.ToUInt16(data, headerOffset + 4);
            metadata.MinorVersion = BitConverter.ToUInt16(data, headerOffset + 6);
            metadata.HeaderSize = BitConverter.ToUInt32(data, headerOffset + 8);

            _log($"[LP] 版本: {metadata.MajorVersion}.{metadata.MinorVersion}");

            // 定位分区表
            int partitionsOffset = headerOffset + 0x50;
            if (partitionsOffset + 16 > data.Length)
                return metadata;

            uint partTableOffset = BitConverter.ToUInt32(data, partitionsOffset);
            uint partTableSize = BitConverter.ToUInt32(data, partitionsOffset + 4);
            uint partEntrySize = BitConverter.ToUInt32(data, partitionsOffset + 8);
            uint partCount = BitConverter.ToUInt32(data, partitionsOffset + 12);

            _log($"[LP] 分区表: Offset={partTableOffset}, Count={partCount}, EntrySize={partEntrySize}");

            // 解析分区条目
            int tableStart = headerOffset + (int)metadata.HeaderSize + (int)partTableOffset;
            
            for (int i = 0; i < partCount; i++)
            {
                int entryOffset = tableStart + (int)(i * partEntrySize);
                if (entryOffset + partEntrySize > data.Length)
                    break;

                var entry = ParseLpPartitionEntry(data, entryOffset, (int)partEntrySize);
                if (entry != null && !string.IsNullOrEmpty(entry.Name))
                {
                    metadata.Partitions.Add(entry);
                }
            }

            // 解析 extents (偏移信息)
            int extentsOffset = partitionsOffset + 16;
            if (extentsOffset + 16 <= data.Length)
            {
                uint extTableOffset = BitConverter.ToUInt32(data, extentsOffset);
                uint extTableSize = BitConverter.ToUInt32(data, extentsOffset + 4);
                uint extEntrySize = BitConverter.ToUInt32(data, extentsOffset + 8);
                uint extCount = BitConverter.ToUInt32(data, extentsOffset + 12);

                int extStart = headerOffset + (int)metadata.HeaderSize + (int)extTableOffset;
                
                // 为每个分区计算实际偏移
                foreach (var part in metadata.Partitions)
                {
                    if (part.FirstExtentIndex >= 0 && part.FirstExtentIndex < extCount)
                    {
                        int extOffset = extStart + (int)(part.FirstExtentIndex * extEntrySize);
                        if (extOffset + 24 <= data.Length)
                        {
                            // extent 结构: target_type(4), target_data(8), target_source(4), num_sectors(8)
                            part.Offset = (long)BitConverter.ToUInt64(data, extOffset + 4);
                            part.Size = (long)BitConverter.ToUInt64(data, extOffset + 16) * 512;
                        }
                    }
                }
            }

            return metadata;
        }

        /// <summary>
        /// 解析单个 LP 分区条目
        /// </summary>
        private LpPartitionEntry ParseLpPartitionEntry(byte[] data, int offset, int entrySize)
        {
            if (offset + entrySize > data.Length)
                return null;

            // LP_PARTITION 结构:
            // 0x00: name (36 bytes, null-terminated)
            // 0x24: attributes (4)
            // 0x28: first_extent_index (4)
            // 0x2C: num_extents (4)
            // 0x30: group_index (4)

            var entry = new LpPartitionEntry();

            // 读取名称
            int nameEnd = Array.IndexOf(data, (byte)0, offset, 36);
            if (nameEnd < 0) nameEnd = offset + 36;
            entry.Name = Encoding.UTF8.GetString(data, offset, nameEnd - offset).TrimEnd('\0');

            if (string.IsNullOrEmpty(entry.Name))
                return null;

            entry.Attributes = BitConverter.ToUInt32(data, offset + 0x24);
            entry.FirstExtentIndex = BitConverter.ToUInt32(data, offset + 0x28);
            entry.NumExtents = BitConverter.ToUInt32(data, offset + 0x2C);
            entry.GroupIndex = BitConverter.ToUInt32(data, offset + 0x30);

            return entry;
        }

        #endregion

        #region EXT4 文件系统解析

        /// <summary>
        /// 从 EXT4 分区读取 build.prop
        /// </summary>
        private async Task<AndroidBuildProps> ReadBuildPropFromExt4Async(
            long startSector, long partitionSize, int lun, CancellationToken ct,
            string buildPropPath = "system/build.prop")
        {
            string tempFile = Path.Combine(Path.GetTempPath(), $"ext4_{Guid.NewGuid():N}.bin");
            
            try
            {
                // 读取 EXT4 超级块和 inode 表区域
                // 超级块位于 offset 1024
                // 我们需要读取足够的数据来定位 build.prop
                
                int sectorsToRead = Math.Min(1024, (int)(partitionSize / _sectorSize)); // 读取最多 4MB
                
                bool success = await _firehose.ReadPartitionChunkedAsync(
                    tempFile,
                    startSector.ToString(),
                    sectorsToRead,
                    lun.ToString(),
                    null,
                    ct,
                    "system",
                    "system",
                    false,
                    true
                );

                if (!success || !File.Exists(tempFile))
                {
                    _log("[EXT4] 读取分区数据失败");
                    return null;
                }

                // 解析 EXT4 并查找 build.prop
                return ParseExt4AndFindBuildProp(tempFile, buildPropPath);
            }
            finally
            {
                try { if (File.Exists(tempFile)) File.Delete(tempFile); } catch { }
            }
        }

        /// <summary>
        /// 检测文件系统类型
        /// </summary>
        public FileSystemType DetectFileSystemType(byte[] data)
        {
            if (data == null || data.Length < 2048)
                return FileSystemType.Unknown;

            // 检查 sparse 魔数 (offset 0)
            uint magic0 = BitConverter.ToUInt32(data, 0);
            if (magic0 == SPARSE_MAGIC)
                return FileSystemType.Sparse;

            // 检查 EROFS 魔数 (offset 1024)
            if (data.Length >= 1028)
            {
                uint erofsCheck = BitConverter.ToUInt32(data, 1024);
                if (erofsCheck == EROFS_MAGIC)
                    return FileSystemType.EROFS;
            }

            // 检查 F2FS 魔数 (offset 1024)
            if (data.Length >= 1028)
            {
                uint f2fsCheck = BitConverter.ToUInt32(data, 1024);
                if (f2fsCheck == F2FS_MAGIC)
                    return FileSystemType.F2FS;
            }

            // 检查 EXT4 魔数 (offset 1024 + 0x38 = 1080)
            if (data.Length >= 1082)
            {
                ushort ext4Check = BitConverter.ToUInt16(data, 1080);
                if (ext4Check == EXT4_MAGIC)
                    return FileSystemType.EXT4;
            }

            return FileSystemType.Unknown;
        }

        /// <summary>
        /// 解析分区镜像并查找 build.prop (支持 EXT4/EROFS/F2FS)
        /// </summary>
        private AndroidBuildProps ParsePartitionAndFindBuildProp(string imageFile, string targetPath)
        {
            try
            {
                using (var fs = new FileStream(imageFile, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    // 读取头部数据检测文件系统类型
                    byte[] header = br.ReadBytes(Math.Min(2048, (int)fs.Length));
                    fs.Seek(0, SeekOrigin.Begin);

                    var fsType = DetectFileSystemType(header);
                    _log($"[FS] 检测到文件系统类型: {fsType}");

                    switch (fsType)
                    {
                        case FileSystemType.Sparse:
                            return ParseSparseImage(br, targetPath);
                            
                        case FileSystemType.EROFS:
                            return ParseErofsImage(br, targetPath);
                            
                        case FileSystemType.F2FS:
                            return ParseF2fsImage(br, targetPath);
                            
                        case FileSystemType.EXT4:
                            return ParseRawExt4(br, targetPath);
                            
                        default:
                            _log("[FS] 未知文件系统类型，尝试扫描 build.prop...");
                            return ScanForBuildProp(br);
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[FS] 解析失败: {ex.Message}");
                return null;
            }
        }

        /// <summary>
        /// 解析 EXT4 镜像并查找 build.prop
        /// </summary>
        private AndroidBuildProps ParseExt4AndFindBuildProp(string ext4File, string targetPath)
        {
            return ParsePartitionAndFindBuildProp(ext4File, targetPath);
        }

        /// <summary>
        /// 解析原始 EXT4 镜像
        /// </summary>
        private AndroidBuildProps ParseRawExt4(BinaryReader br, string targetPath)
        {
            var fs = br.BaseStream;

            // EXT4 超级块位于 offset 1024
            if (fs.Length < 2048)
            {
                _log("[EXT4] 文件太小，不是有效的 EXT4 镜像");
                return null;
            }

            fs.Seek(1024, SeekOrigin.Begin);
            
            // 读取超级块关键字段
            uint s_inodes_count = br.ReadUInt32();     // 0x00
            uint s_blocks_count_lo = br.ReadUInt32(); // 0x04
            br.ReadBytes(16);                          // 跳过
            uint s_log_block_size = br.ReadUInt32();  // 0x18
            br.ReadBytes(4);
            uint s_blocks_per_group = br.ReadUInt32(); // 0x20
            br.ReadBytes(4);
            uint s_inodes_per_group = br.ReadUInt32(); // 0x28
            br.ReadBytes(24);
            ushort s_magic = br.ReadUInt16();         // 0x38

            if (s_magic != EXT4_MAGIC)
            {
                _log($"[EXT4] 无效的 EXT4 魔数: 0x{s_magic:X4}");
                return null;
            }

            uint blockSize = (uint)(1024 << (int)s_log_block_size);
            _log($"[EXT4] 块大小: {blockSize}, inode 数: {s_inodes_count}");

            // 读取 inode 表，查找 build.prop
            // build.prop 通常在 /system/build.prop (inode 2 -> root -> system -> build.prop)
            
            // 简化处理：扫描文件系统查找 build.prop 内容
            return ScanForBuildProp(br);
        }

        /// <summary>
        /// 解析 Sparse 镜像 (可能包含 EXT4/EROFS/F2FS)
        /// </summary>
        private AndroidBuildProps ParseSparseImage(BinaryReader br, string targetPath)
        {
            var fs = br.BaseStream;

            // Sparse header
            uint magic = br.ReadUInt32();
            ushort majorVersion = br.ReadUInt16();
            ushort minorVersion = br.ReadUInt16();
            ushort headerSize = br.ReadUInt16();
            ushort chunkHeaderSize = br.ReadUInt16();
            uint blockSize = br.ReadUInt32();
            uint totalBlocks = br.ReadUInt32();
            uint totalChunks = br.ReadUInt32();
            uint imageCrc = br.ReadUInt32();

            _log($"[Sparse] 块大小: {blockSize}, 总块数: {totalBlocks}, 块数: {totalChunks}");

            // 展开 sparse 数据到内存中
            using (var ms = new MemoryStream())
            {
                fs.Seek(headerSize, SeekOrigin.Begin);

                for (uint i = 0; i < totalChunks && fs.Position < fs.Length; i++)
                {
                    ushort chunkType = br.ReadUInt16();
                    br.ReadUInt16(); // reserved
                    uint chunkBlocks = br.ReadUInt32();
                    uint totalSize = br.ReadUInt32();

                    int dataSize = (int)(totalSize - chunkHeaderSize);

                    switch (chunkType)
                    {
                        case 0xCAC1: // Raw
                            byte[] rawData = br.ReadBytes(dataSize);
                            ms.Write(rawData, 0, rawData.Length);
                            break;

                        case 0xCAC2: // Fill
                            uint fillValue = br.ReadUInt32();
                            byte[] fillBytes = BitConverter.GetBytes(fillValue);
                            for (uint b = 0; b < chunkBlocks * blockSize; b += 4)
                                ms.Write(fillBytes, 0, 4);
                            break;

                        case 0xCAC3: // Don't care
                            // 写入零填充
                            byte[] zeros = new byte[Math.Min(chunkBlocks * blockSize, 4096)];
                            for (uint b = 0; b < chunkBlocks * blockSize; b += (uint)zeros.Length)
                                ms.Write(zeros, 0, (int)Math.Min(zeros.Length, chunkBlocks * blockSize - b));
                            break;

                        case 0xCAC4: // CRC
                            br.ReadUInt32(); // crc value
                            break;
                    }

                    // 只解析前几 MB 就够了
                    if (ms.Length > 16 * 1024 * 1024)
                        break;
                }

                // 检测解压后的文件系统类型
                ms.Seek(0, SeekOrigin.Begin);
                byte[] header = new byte[Math.Min(2048, (int)ms.Length)];
                ms.Read(header, 0, header.Length);
                ms.Seek(0, SeekOrigin.Begin);
                
                var fsType = DetectFileSystemType(header);
                _log($"[Sparse] 解压后文件系统: {fsType}");

                using (var br2 = new BinaryReader(ms))
                {
                    switch (fsType)
                    {
                        case FileSystemType.EROFS:
                            return ParseErofsImage(br2, targetPath);
                        case FileSystemType.F2FS:
                            return ParseF2fsImage(br2, targetPath);
                        case FileSystemType.EXT4:
                            return ParseRawExt4(br2, targetPath);
                        default:
                            return ScanForBuildProp(br2);
                    }
                }
            }
        }

        #region EROFS 文件系统解析

        /// <summary>
        /// 解析 EROFS 镜像 (Enhanced Read-Only File System)
        /// Android 12+ 系统分区使用
        /// </summary>
        private AndroidBuildProps ParseErofsImage(BinaryReader br, string targetPath)
        {
            var fs = br.BaseStream;
            
            if (fs.Length < 2048)
            {
                _log("[EROFS] 文件太小，不是有效的 EROFS 镜像");
                return null;
            }

            fs.Seek(1024, SeekOrigin.Begin);

            // EROFS Superblock 结构 (128 bytes)
            uint magic = br.ReadUInt32();           // 0x00: magic
            uint checksum = br.ReadUInt32();        // 0x04: crc32c checksum
            uint feature_compat = br.ReadUInt32(); // 0x08: compatible features
            byte blkszbits = br.ReadByte();         // 0x0C: block size in bits (9-12)
            byte sb_extslots = br.ReadByte();       // 0x0D: superblock extension slots
            ushort root_nid = br.ReadUInt16();      // 0x0E: root inode nid
            ulong inos = br.ReadUInt64();           // 0x10: total valid ino #
            ulong build_time = br.ReadUInt64();     // 0x18: build time
            uint build_time_nsec = br.ReadUInt32(); // 0x20: build time (nsec)
            uint blocks = br.ReadUInt32();          // 0x24: total block count
            uint meta_blkaddr = br.ReadUInt32();    // 0x28: start block of metadata
            uint xattr_blkaddr = br.ReadUInt32();   // 0x2C: start block of xattr
            byte[] uuid = br.ReadBytes(16);         // 0x30: uuid
            byte[] volume_name = br.ReadBytes(16); // 0x40: volume name

            if (magic != EROFS_MAGIC)
            {
                _log($"[EROFS] 无效的 EROFS 魔数: 0x{magic:X8}");
                return null;
            }

            uint blockSize = (uint)(1 << blkszbits);
            string volName = Encoding.UTF8.GetString(volume_name).TrimEnd('\0');
            
            _log($"[EROFS] 块大小: {blockSize}, 根 inode: {root_nid}, 卷名: {volName}");
            _log($"[EROFS] 总 inode 数: {inos}, 总块数: {blocks}");

            // EROFS 使用压缩存储，直接扫描查找 build.prop
            return ScanForBuildProp(br);
        }

        #endregion

        #region F2FS 文件系统解析

        /// <summary>
        /// 解析 F2FS 镜像 (Flash-Friendly File System)
        /// </summary>
        private AndroidBuildProps ParseF2fsImage(BinaryReader br, string targetPath)
        {
            var fs = br.BaseStream;
            
            if (fs.Length < 2048)
            {
                _log("[F2FS] 文件太小，不是有效的 F2FS 镜像");
                return null;
            }

            fs.Seek(1024, SeekOrigin.Begin);

            // F2FS Superblock 结构 (简化)
            uint magic = br.ReadUInt32();            // 0x00: magic
            ushort major_ver = br.ReadUInt16();     // 0x04: major version
            ushort minor_ver = br.ReadUInt16();     // 0x06: minor version
            uint log_sectorsize = br.ReadUInt32();  // 0x08: log2 sector size
            uint log_sectors_per_block = br.ReadUInt32(); // 0x0C: log2 sectors per block
            uint log_blocksize = br.ReadUInt32();   // 0x10: log2 block size
            uint log_blocks_per_seg = br.ReadUInt32(); // 0x14: log2 blocks per segment
            uint segs_per_sec = br.ReadUInt32();    // 0x18: # of segments per section
            uint secs_per_zone = br.ReadUInt32();   // 0x1C: # of sections per zone
            uint checksum_offset = br.ReadUInt32(); // 0x20: checksum offset
            ulong block_count = br.ReadUInt64();    // 0x24: total # of blocks
            uint section_count = br.ReadUInt32();   // 0x2C: total # of sections
            uint segment_count = br.ReadUInt32();   // 0x30: total # of segments
            uint segment_count_ckpt = br.ReadUInt32(); // 0x34: # of segments for checkpoint
            uint segment_count_sit = br.ReadUInt32();  // 0x38: # of segments for SIT
            uint segment_count_nat = br.ReadUInt32();  // 0x3C: # of segments for NAT
            uint segment_count_ssa = br.ReadUInt32();  // 0x40: # of segments for SSA
            uint segment_count_main = br.ReadUInt32(); // 0x44: # of segments for main area
            uint segment0_blkaddr = br.ReadUInt32();   // 0x48: start block address of segment 0
            uint cp_blkaddr = br.ReadUInt32();         // 0x4C: start block address of checkpoint
            uint sit_blkaddr = br.ReadUInt32();        // 0x50: start block address of SIT
            uint nat_blkaddr = br.ReadUInt32();        // 0x54: start block address of NAT
            uint ssa_blkaddr = br.ReadUInt32();        // 0x58: start block address of SSA
            uint main_blkaddr = br.ReadUInt32();       // 0x5C: start block address of main area
            uint root_ino = br.ReadUInt32();           // 0x60: root inode number

            if (magic != F2FS_MAGIC)
            {
                _log($"[F2FS] 无效的 F2FS 魔数: 0x{magic:X8}");
                return null;
            }

            uint blockSize = (uint)(1 << (int)log_blocksize);
            _log($"[F2FS] 版本: {major_ver}.{minor_ver}, 块大小: {blockSize}");
            _log($"[F2FS] 总块数: {block_count}, 主区域起始块: {main_blkaddr}");

            // F2FS 结构复杂，直接扫描查找 build.prop
            return ScanForBuildProp(br);
        }

        #endregion

        /// <summary>
        /// 扫描文件系统查找 build.prop 内容
        /// </summary>
        private AndroidBuildProps ScanForBuildProp(BinaryReader br)
        {
            var fs = br.BaseStream;
            fs.Seek(0, SeekOrigin.Begin);

            byte[] buffer = new byte[Math.Min(fs.Length, 16 * 1024 * 1024)]; // 最多 16MB
            int read = fs.Read(buffer, 0, buffer.Length);

            string content = Encoding.UTF8.GetString(buffer, 0, read);
            
            // 查找 build.prop 的特征内容
            var markers = new[] {
                "ro.build.fingerprint=",
                "ro.product.model=",
                "ro.product.brand=",
                "ro.build.version.release="
            };

            foreach (var marker in markers)
            {
                int idx = content.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
                if (idx >= 0)
                {
                    // 找到 build.prop，向前后扩展读取完整内容
                    int start = content.LastIndexOf('\n', Math.Max(0, idx - 1000)) + 1;
                    if (start < 0) start = 0;
                    
                    int end = idx + 5000;
                    while (end < content.Length && end < idx + 50000)
                    {
                        int nextEnd = content.IndexOf('\n', end);
                        if (nextEnd < 0) break;
                        
                        string line = content.Substring(end, nextEnd - end);
                        if (!line.Contains("=") || line.StartsWith("#"))
                        {
                            // 可能是 build.prop 结束
                            if (content.Substring(end, Math.Min(100, content.Length - end)).Contains("\0\0\0"))
                                break;
                        }
                        end = nextEnd + 1;
                    }

                    string propContent = content.Substring(start, Math.Min(end - start, 50000));
                    return ParseBuildPropContent(propContent);
                }
            }

            _log("[EXT4] 未找到 build.prop 内容");
            return null;
        }

        #endregion

        #region Build.prop 解析

        /// <summary>
        /// 解析 build.prop 内容
        /// </summary>
        private AndroidBuildProps ParseBuildPropContent(string content)
        {
            var props = new AndroidBuildProps();

            foreach (var line in content.Split('\n'))
            {
                string trimmed = line.Trim();
                if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith("#"))
                    continue;

                int eqIdx = trimmed.IndexOf('=');
                if (eqIdx <= 0) continue;

                string key = trimmed.Substring(0, eqIdx).Trim();
                string value = trimmed.Substring(eqIdx + 1).Trim();

                // 过滤无效值
                if (string.IsNullOrEmpty(value) || value.Contains("\0"))
                    continue;

                props.AllProps[key] = value;

                // 映射常用属性
                switch (key.ToLower())
                {
                    case "ro.product.brand":
                    case "ro.product.system.brand":
                        if (string.IsNullOrEmpty(props.Brand)) props.Brand = value;
                        break;
                    case "ro.product.model":
                    case "ro.product.system.model":
                        if (string.IsNullOrEmpty(props.Model)) props.Model = value;
                        break;
                    case "ro.product.device":
                    case "ro.product.system.device":
                        if (string.IsNullOrEmpty(props.Device)) props.Device = value;
                        break;
                    case "ro.product.name":
                    case "ro.product.system.name":
                        if (string.IsNullOrEmpty(props.Product)) props.Product = value;
                        break;
                    case "ro.product.manufacturer":
                    case "ro.product.system.manufacturer":
                        if (string.IsNullOrEmpty(props.Manufacturer)) props.Manufacturer = value;
                        break;
                    case "ro.build.version.release":
                    case "ro.system.build.version.release":
                        if (string.IsNullOrEmpty(props.AndroidVersion)) props.AndroidVersion = value;
                        break;
                    case "ro.build.version.sdk":
                    case "ro.system.build.version.sdk":
                        if (string.IsNullOrEmpty(props.SdkVersion)) props.SdkVersion = value;
                        break;
                    case "ro.build.version.security_patch":
                        if (string.IsNullOrEmpty(props.SecurityPatch)) props.SecurityPatch = value;
                        break;
                    case "ro.build.id":
                    case "ro.system.build.id":
                        if (string.IsNullOrEmpty(props.BuildId)) props.BuildId = value;
                        break;
                    case "ro.build.fingerprint":
                    case "ro.system.build.fingerprint":
                        if (string.IsNullOrEmpty(props.BuildFingerprint)) props.BuildFingerprint = value;
                        break;
                    case "ro.build.description":
                        if (string.IsNullOrEmpty(props.BuildDescription)) props.BuildDescription = value;
                        break;
                    case "ro.build.version.incremental":
                    case "ro.system.build.version.incremental":
                        if (string.IsNullOrEmpty(props.Incremental)) props.Incremental = value;
                        break;
                    case "ro.build.version.base_os":
                        if (string.IsNullOrEmpty(props.BaseOs)) props.BaseOs = value;
                        break;
                    // 厂商特有属性
                    case "ro.build.display.id":
                    case "ro.miui.ui.version.name":
                    case "ro.build.version.oplusrom":
                    case "ro.vivo.os.version":
                        if (string.IsNullOrEmpty(props.RomVersion)) props.RomVersion = value;
                        break;
                }
            }

            _log($"[BuildProp] 解析完成: {props.Brand} {props.Model}");
            return props;
        }

        #endregion

        #region 辅助方法

        private string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1024 * 1024) return $"{bytes / 1024.0:F1} KB";
            if (bytes < 1024 * 1024 * 1024) return $"{bytes / 1024.0 / 1024.0:F1} MB";
            return $"{bytes / 1024.0 / 1024.0 / 1024.0:F2} GB";
        }

        #endregion
    }
}
