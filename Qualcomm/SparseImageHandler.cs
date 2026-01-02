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
    /// Sparse 镜像头信息
    /// </summary>
    public class SparseHeader
    {
        public const uint SPARSE_HEADER_MAGIC = 0xED26FF3A;
        
        public uint Magic { get; set; }
        public ushort MajorVersion { get; set; }
        public ushort MinorVersion { get; set; }
        public ushort FileHeaderSize { get; set; }
        public ushort ChunkHeaderSize { get; set; }
        public uint BlockSize { get; set; }
        public uint TotalBlocks { get; set; }
        public uint TotalChunks { get; set; }
        public uint ImageChecksum { get; set; }
        
        public long TotalSize => (long)BlockSize * TotalBlocks;
        
        public bool IsValid => Magic == SPARSE_HEADER_MAGIC;
        
        public static SparseHeader Read(BinaryReader reader)
        {
            var header = new SparseHeader
            {
                Magic = reader.ReadUInt32(),
                MajorVersion = reader.ReadUInt16(),
                MinorVersion = reader.ReadUInt16(),
                FileHeaderSize = reader.ReadUInt16(),
                ChunkHeaderSize = reader.ReadUInt16(),
                BlockSize = reader.ReadUInt32(),
                TotalBlocks = reader.ReadUInt32(),
                TotalChunks = reader.ReadUInt32(),
                ImageChecksum = reader.ReadUInt32()
            };
            return header;
        }
        
        public void Write(BinaryWriter writer)
        {
            writer.Write(Magic);
            writer.Write(MajorVersion);
            writer.Write(MinorVersion);
            writer.Write(FileHeaderSize);
            writer.Write(ChunkHeaderSize);
            writer.Write(BlockSize);
            writer.Write(TotalBlocks);
            writer.Write(TotalChunks);
            writer.Write(ImageChecksum);
        }
        
        public override string ToString()
        {
            return $"Sparse v{MajorVersion}.{MinorVersion}, BlockSize={BlockSize}, Blocks={TotalBlocks}, Chunks={TotalChunks}, Size={TotalSize / 1024 / 1024}MB";
        }
    }

    /// <summary>
    /// Sparse 块类型
    /// </summary>
    public enum SparseChunkType : ushort
    {
        Raw = 0xCAC1,      // 原始数据块
        Fill = 0xCAC2,     // 填充块 (4字节值重复)
        DontCare = 0xCAC3, // 不关心块 (可跳过)
        Crc32 = 0xCAC4     // CRC32 校验块
    }

    /// <summary>
    /// Sparse 块信息
    /// </summary>
    public class SparseChunk
    {
        public SparseChunkType Type { get; set; }
        public ushort Reserved { get; set; }
        public uint ChunkBlocks { get; set; }
        public uint TotalSize { get; set; }
        
        public long DataOffset { get; set; }  // 数据在文件中的偏移
        public byte[] RawData { get; set; }   // 原始数据 (仅 Raw 类型)
        public uint FillValue { get; set; }   // 填充值 (仅 Fill 类型)
        
        public static SparseChunk Read(BinaryReader reader, uint blockSize, ushort chunkHeaderSize)
        {
            var chunk = new SparseChunk
            {
                Type = (SparseChunkType)reader.ReadUInt16(),
                Reserved = reader.ReadUInt16(),
                ChunkBlocks = reader.ReadUInt32(),
                TotalSize = reader.ReadUInt32()
            };
            
            chunk.DataOffset = reader.BaseStream.Position;
            
            int dataSize = (int)(chunk.TotalSize - chunkHeaderSize);
            
            switch (chunk.Type)
            {
                case SparseChunkType.Raw:
                    // 不立即读取，只记录位置
                    reader.BaseStream.Seek(dataSize, SeekOrigin.Current);
                    break;
                    
                case SparseChunkType.Fill:
                    chunk.FillValue = reader.ReadUInt32();
                    break;
                    
                case SparseChunkType.DontCare:
                    // 无数据
                    break;
                    
                case SparseChunkType.Crc32:
                    reader.ReadUInt32(); // CRC value
                    break;
            }
            
            return chunk;
        }
    }

    /// <summary>
    /// 镜像源类型
    /// </summary>
    public enum ImageSource
    {
        Device,  // 从设备读取
        File     // 从文件读取
    }

    /// <summary>
    /// 镜像格式
    /// </summary>
    public enum ImageFormat
    {
        Unknown,
        Raw,
        Sparse
    }

    /// <summary>
    /// 镜像信息
    /// </summary>
    public class ImageInfo
    {
        public string Path { get; set; }
        public ImageSource Source { get; set; }
        public ImageFormat Format { get; set; }
        public long FileSize { get; set; }        // 文件大小 (Sparse 压缩后)
        public long ActualSize { get; set; }      // 实际数据大小 (展开后)
        public SparseHeader SparseHeader { get; set; }
        public DeviceInfoReader.FileSystemType FileSystemType { get; set; }
        public string PartitionName { get; set; }
        
        public double CompressionRatio => FileSize > 0 ? (double)ActualSize / FileSize : 1.0;
        
        public override string ToString()
        {
            var sb = new StringBuilder();
            sb.AppendLine($"路径: {Path}");
            sb.AppendLine($"来源: {Source}");
            sb.AppendLine($"格式: {Format}");
            sb.AppendLine($"文件大小: {FileSize / 1024.0 / 1024.0:F2} MB");
            sb.AppendLine($"实际大小: {ActualSize / 1024.0 / 1024.0:F2} MB");
            if (Format == ImageFormat.Sparse)
                sb.AppendLine($"压缩比: {CompressionRatio:F2}x");
            sb.AppendLine($"文件系统: {FileSystemType}");
            return sb.ToString();
        }
    }

    /// <summary>
    /// Raw/Sparse 镜像处理器
    /// 支持从设备和文件两种来源读取、转换
    /// </summary>
    public class SparseImageHandler
    {
        private Action<string> _log;
        private FirehoseClient _firehose;
        
        public SparseImageHandler(Action<string> logger = null)
        {
            _log = logger ?? Console.WriteLine;
        }
        
        public SparseImageHandler(FirehoseClient firehose, Action<string> logger = null)
        {
            _firehose = firehose;
            _log = logger ?? Console.WriteLine;
        }

        #region 镜像检测

        /// <summary>
        /// 检测文件格式
        /// </summary>
        public ImageFormat DetectFormat(string filePath)
        {
            if (!File.Exists(filePath))
                return ImageFormat.Unknown;
                
            using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read))
            using (var br = new BinaryReader(fs))
            {
                if (fs.Length < 4)
                    return ImageFormat.Unknown;
                    
                uint magic = br.ReadUInt32();
                return magic == SparseHeader.SPARSE_HEADER_MAGIC ? ImageFormat.Sparse : ImageFormat.Raw;
            }
        }

        /// <summary>
        /// 检测字节数据格式
        /// </summary>
        public ImageFormat DetectFormat(byte[] data)
        {
            if (data == null || data.Length < 4)
                return ImageFormat.Unknown;
                
            uint magic = BitConverter.ToUInt32(data, 0);
            return magic == SparseHeader.SPARSE_HEADER_MAGIC ? ImageFormat.Sparse : ImageFormat.Raw;
        }

        /// <summary>
        /// 获取镜像详细信息
        /// </summary>
        public ImageInfo GetImageInfo(string filePath)
        {
            var info = new ImageInfo
            {
                Path = filePath,
                Source = ImageSource.File,
                FileSize = new FileInfo(filePath).Length
            };
            
            info.Format = DetectFormat(filePath);
            
            using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read))
            using (var br = new BinaryReader(fs))
            {
                if (info.Format == ImageFormat.Sparse)
                {
                    var header = SparseHeader.Read(br);
                    info.SparseHeader = header;
                    info.ActualSize = header.TotalSize;
                    
                    // 检测展开后的文件系统类型
                    info.FileSystemType = DetectFileSystemInSparse(br, header);
                }
                else
                {
                    info.ActualSize = info.FileSize;
                    info.FileSystemType = DetectFileSystemInRaw(br);
                }
            }
            
            return info;
        }

        /// <summary>
        /// 检测 Sparse 镜像内部的文件系统类型
        /// </summary>
        private DeviceInfoReader.FileSystemType DetectFileSystemInSparse(BinaryReader br, SparseHeader header)
        {
            br.BaseStream.Seek(header.FileHeaderSize, SeekOrigin.Begin);
            
            // 读取第一个 chunk，应该包含文件系统头
            using (var ms = new MemoryStream())
            {
                // 读取前 8KB 的数据
                long bytesNeeded = 8192;
                long bytesRead = 0;
                
                while (bytesRead < bytesNeeded && br.BaseStream.Position < br.BaseStream.Length)
                {
                    var chunk = SparseChunk.Read(br, header.BlockSize, header.ChunkHeaderSize);
                    long chunkSize = chunk.ChunkBlocks * header.BlockSize;
                    
                    switch (chunk.Type)
                    {
                        case SparseChunkType.Raw:
                            br.BaseStream.Seek(chunk.DataOffset, SeekOrigin.Begin);
                            int toRead = (int)Math.Min(chunkSize, bytesNeeded - bytesRead);
                            byte[] data = br.ReadBytes(toRead);
                            ms.Write(data, 0, data.Length);
                            bytesRead += toRead;
                            // 跳回 chunk 结束位置
                            br.BaseStream.Seek(chunk.DataOffset + (chunk.TotalSize - header.ChunkHeaderSize), SeekOrigin.Begin);
                            break;
                            
                        case SparseChunkType.Fill:
                            byte[] fillData = BitConverter.GetBytes(chunk.FillValue);
                            for (int i = 0; i < Math.Min(chunkSize, bytesNeeded - bytesRead); i += 4)
                            {
                                ms.Write(fillData, 0, Math.Min(4, (int)(bytesNeeded - bytesRead - i)));
                            }
                            bytesRead += Math.Min(chunkSize, bytesNeeded - bytesRead);
                            break;
                            
                        case SparseChunkType.DontCare:
                            // 写入零
                            byte[] zeros = new byte[Math.Min(chunkSize, bytesNeeded - bytesRead)];
                            ms.Write(zeros, 0, zeros.Length);
                            bytesRead += zeros.Length;
                            break;
                    }
                }
                
                return DetectFileSystemType(ms.ToArray());
            }
        }

        /// <summary>
        /// 检测 Raw 镜像的文件系统类型
        /// </summary>
        private DeviceInfoReader.FileSystemType DetectFileSystemInRaw(BinaryReader br)
        {
            br.BaseStream.Seek(0, SeekOrigin.Begin);
            byte[] header = br.ReadBytes(Math.Min(2048, (int)br.BaseStream.Length));
            return DetectFileSystemType(header);
        }

        /// <summary>
        /// 从数据检测文件系统类型
        /// </summary>
        public DeviceInfoReader.FileSystemType DetectFileSystemType(byte[] data)
        {
            if (data == null || data.Length < 2048)
                return DeviceInfoReader.FileSystemType.Unknown;

            // 检查 EROFS 魔数 (offset 1024)
            if (data.Length >= 1028)
            {
                uint erofsCheck = BitConverter.ToUInt32(data, 1024);
                if (erofsCheck == 0xE0F5E1E2)
                    return DeviceInfoReader.FileSystemType.EROFS;
            }

            // 检查 F2FS 魔数 (offset 1024)
            if (data.Length >= 1028)
            {
                uint f2fsCheck = BitConverter.ToUInt32(data, 1024);
                if (f2fsCheck == 0xF2F52010)
                    return DeviceInfoReader.FileSystemType.F2FS;
            }

            // 检查 EXT4 魔数 (offset 1024 + 0x38 = 1080)
            if (data.Length >= 1082)
            {
                ushort ext4Check = BitConverter.ToUInt16(data, 1080);
                if (ext4Check == 0xEF53)
                    return DeviceInfoReader.FileSystemType.EXT4;
            }

            return DeviceInfoReader.FileSystemType.Unknown;
        }

        #endregion

        #region Sparse 转 Raw

        /// <summary>
        /// 将 Sparse 镜像转换为 Raw 镜像
        /// </summary>
        public async Task<bool> SparseToRawAsync(
            string sparseFile, 
            string rawFile, 
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            _log($"[Sparse→Raw] 开始转换: {Path.GetFileName(sparseFile)}");
            
            try
            {
                using (var input = new FileStream(sparseFile, FileMode.Open, FileAccess.Read))
                using (var reader = new BinaryReader(input))
                using (var output = new FileStream(rawFile, FileMode.Create, FileAccess.Write))
                {
                    var header = SparseHeader.Read(reader);
                    
                    if (!header.IsValid)
                    {
                        _log("[Sparse→Raw] 无效的 Sparse 头");
                        return false;
                    }
                    
                    _log($"[Sparse→Raw] {header}");
                    
                    input.Seek(header.FileHeaderSize, SeekOrigin.Begin);
                    
                    long totalBytes = header.TotalSize;
                    long writtenBytes = 0;
                    byte[] blockBuffer = new byte[header.BlockSize];
                    
                    for (uint i = 0; i < header.TotalChunks; i++)
                    {
                        if (ct.IsCancellationRequested)
                            return false;
                        
                        ushort chunkType = reader.ReadUInt16();
                        reader.ReadUInt16(); // reserved
                        uint chunkBlocks = reader.ReadUInt32();
                        uint totalSize = reader.ReadUInt32();
                        
                        int dataSize = (int)(totalSize - header.ChunkHeaderSize);
                        long chunkDataSize = (long)chunkBlocks * header.BlockSize;
                        
                        switch ((SparseChunkType)chunkType)
                        {
                            case SparseChunkType.Raw:
                                // 直接复制数据
                                for (int j = 0; j < chunkBlocks; j++)
                                {
                                    int read = reader.Read(blockBuffer, 0, (int)header.BlockSize);
                                    output.Write(blockBuffer, 0, read);
                                    writtenBytes += read;
                                }
                                break;
                                
                            case SparseChunkType.Fill:
                                uint fillValue = reader.ReadUInt32();
                                byte[] fillBytes = BitConverter.GetBytes(fillValue);
                                // 填充块数据
                                for (int j = 0; j < header.BlockSize / 4; j++)
                                    Array.Copy(fillBytes, 0, blockBuffer, j * 4, 4);
                                    
                                for (int j = 0; j < chunkBlocks; j++)
                                {
                                    output.Write(blockBuffer, 0, (int)header.BlockSize);
                                    writtenBytes += header.BlockSize;
                                }
                                break;
                                
                            case SparseChunkType.DontCare:
                                // 写入零
                                Array.Clear(blockBuffer, 0, blockBuffer.Length);
                                for (int j = 0; j < chunkBlocks; j++)
                                {
                                    output.Write(blockBuffer, 0, (int)header.BlockSize);
                                    writtenBytes += header.BlockSize;
                                }
                                break;
                                
                            case SparseChunkType.Crc32:
                                reader.ReadUInt32(); // skip CRC
                                break;
                        }
                        
                        progress?.Invoke(writtenBytes, totalBytes);
                    }
                    
                    _log($"[Sparse→Raw] 完成: {writtenBytes / 1024 / 1024} MB");
                    return true;
                }
            }
            catch (Exception ex)
            {
                _log($"[Sparse→Raw] 错误: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 将 Sparse 镜像展开到内存流
        /// </summary>
        public MemoryStream SparseToMemoryStream(string sparseFile, long maxSize = 64 * 1024 * 1024)
        {
            var ms = new MemoryStream();
            
            using (var input = new FileStream(sparseFile, FileMode.Open, FileAccess.Read))
            using (var reader = new BinaryReader(input))
            {
                var header = SparseHeader.Read(reader);
                
                if (!header.IsValid)
                    return null;
                
                input.Seek(header.FileHeaderSize, SeekOrigin.Begin);
                byte[] blockBuffer = new byte[header.BlockSize];
                
                for (uint i = 0; i < header.TotalChunks && ms.Length < maxSize; i++)
                {
                    ushort chunkType = reader.ReadUInt16();
                    reader.ReadUInt16();
                    uint chunkBlocks = reader.ReadUInt32();
                    uint totalSize = reader.ReadUInt32();
                    
                    switch ((SparseChunkType)chunkType)
                    {
                        case SparseChunkType.Raw:
                            for (int j = 0; j < chunkBlocks && ms.Length < maxSize; j++)
                            {
                                int read = reader.Read(blockBuffer, 0, (int)header.BlockSize);
                                ms.Write(blockBuffer, 0, read);
                            }
                            break;
                            
                        case SparseChunkType.Fill:
                            uint fillValue = reader.ReadUInt32();
                            byte[] fillBytes = BitConverter.GetBytes(fillValue);
                            for (int j = 0; j < (int)header.BlockSize / 4; j++)
                                Array.Copy(fillBytes, 0, blockBuffer, j * 4, 4);
                            for (int j = 0; j < chunkBlocks && ms.Length < maxSize; j++)
                                ms.Write(blockBuffer, 0, (int)header.BlockSize);
                            break;
                            
                        case SparseChunkType.DontCare:
                            Array.Clear(blockBuffer, 0, blockBuffer.Length);
                            for (int j = 0; j < chunkBlocks && ms.Length < maxSize; j++)
                                ms.Write(blockBuffer, 0, (int)header.BlockSize);
                            break;
                            
                        case SparseChunkType.Crc32:
                            reader.ReadUInt32();
                            break;
                    }
                }
            }
            
            ms.Seek(0, SeekOrigin.Begin);
            return ms;
        }

        #endregion

        #region Raw 转 Sparse

        /// <summary>
        /// 将 Raw 镜像转换为 Sparse 镜像
        /// </summary>
        public async Task<bool> RawToSparseAsync(
            string rawFile, 
            string sparseFile, 
            uint blockSize = 4096,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            _log($"[Raw→Sparse] 开始转换: {Path.GetFileName(rawFile)}");
            
            try
            {
                using (var input = new FileStream(rawFile, FileMode.Open, FileAccess.Read))
                using (var output = new FileStream(sparseFile, FileMode.Create, FileAccess.Write))
                using (var writer = new BinaryWriter(output))
                {
                    long fileSize = input.Length;
                    uint totalBlocks = (uint)((fileSize + blockSize - 1) / blockSize);
                    
                    // 先写入占位头，稍后更新
                    long headerPos = output.Position;
                    var header = new SparseHeader
                    {
                        Magic = SparseHeader.SPARSE_HEADER_MAGIC,
                        MajorVersion = 1,
                        MinorVersion = 0,
                        FileHeaderSize = 28,
                        ChunkHeaderSize = 12,
                        BlockSize = blockSize,
                        TotalBlocks = totalBlocks,
                        TotalChunks = 0,
                        ImageChecksum = 0
                    };
                    header.Write(writer);
                    
                    byte[] blockBuffer = new byte[blockSize];
                    byte[] zeroBlock = new byte[blockSize];
                    byte[] fillCheckBuffer = new byte[4];
                    
                    List<SparseChunk> chunks = new List<SparseChunk>();
                    long processedBytes = 0;
                    
                    // 当前累积的块信息
                    SparseChunkType? currentChunkType = null;
                    uint currentChunkBlocks = 0;
                    uint currentFillValue = 0;
                    long rawDataStart = 0;
                    List<byte[]> rawDataBlocks = new List<byte[]>();
                    
                    while (input.Position < input.Length)
                    {
                        if (ct.IsCancellationRequested)
                            return false;
                        
                        int bytesRead = input.Read(blockBuffer, 0, (int)blockSize);
                        if (bytesRead == 0) break;
                        
                        // 填充到完整块大小
                        if (bytesRead < blockSize)
                            Array.Clear(blockBuffer, bytesRead, (int)blockSize - bytesRead);
                        
                        // 检测块类型
                        SparseChunkType blockType;
                        uint fillValue = 0;
                        
                        if (IsZeroBlock(blockBuffer))
                        {
                            blockType = SparseChunkType.DontCare;
                        }
                        else if (IsFillBlock(blockBuffer, out fillValue))
                        {
                            blockType = SparseChunkType.Fill;
                        }
                        else
                        {
                            blockType = SparseChunkType.Raw;
                        }
                        
                        // 合并相同类型的块
                        if (currentChunkType == null)
                        {
                            currentChunkType = blockType;
                            currentChunkBlocks = 1;
                            if (blockType == SparseChunkType.Fill)
                                currentFillValue = fillValue;
                            else if (blockType == SparseChunkType.Raw)
                                rawDataBlocks.Add((byte[])blockBuffer.Clone());
                        }
                        else if (blockType == currentChunkType && 
                                 (blockType != SparseChunkType.Fill || fillValue == currentFillValue))
                        {
                            currentChunkBlocks++;
                            if (blockType == SparseChunkType.Raw)
                                rawDataBlocks.Add((byte[])blockBuffer.Clone());
                        }
                        else
                        {
                            // 写入当前 chunk
                            WriteChunk(writer, currentChunkType.Value, currentChunkBlocks, 
                                       currentFillValue, rawDataBlocks, header.ChunkHeaderSize);
                            header.TotalChunks++;
                            
                            // 开始新 chunk
                            currentChunkType = blockType;
                            currentChunkBlocks = 1;
                            rawDataBlocks.Clear();
                            if (blockType == SparseChunkType.Fill)
                                currentFillValue = fillValue;
                            else if (blockType == SparseChunkType.Raw)
                                rawDataBlocks.Add((byte[])blockBuffer.Clone());
                        }
                        
                        processedBytes += bytesRead;
                        progress?.Invoke(processedBytes, fileSize);
                    }
                    
                    // 写入最后一个 chunk
                    if (currentChunkType != null && currentChunkBlocks > 0)
                    {
                        WriteChunk(writer, currentChunkType.Value, currentChunkBlocks, 
                                   currentFillValue, rawDataBlocks, header.ChunkHeaderSize);
                        header.TotalChunks++;
                    }
                    
                    // 更新头部
                    output.Seek(headerPos, SeekOrigin.Begin);
                    header.Write(writer);
                    
                    long outputSize = output.Length;
                    double ratio = (double)fileSize / outputSize;
                    
                    _log($"[Raw→Sparse] 完成: {fileSize / 1024 / 1024}MB → {outputSize / 1024 / 1024}MB (压缩比 {ratio:F2}x)");
                    return true;
                }
            }
            catch (Exception ex)
            {
                _log($"[Raw→Sparse] 错误: {ex.Message}");
                return false;
            }
        }

        private bool IsZeroBlock(byte[] block)
        {
            for (int i = 0; i < block.Length; i++)
                if (block[i] != 0) return false;
            return true;
        }

        private bool IsFillBlock(byte[] block, out uint fillValue)
        {
            fillValue = BitConverter.ToUInt32(block, 0);
            for (int i = 4; i < block.Length; i += 4)
            {
                if (BitConverter.ToUInt32(block, i) != fillValue)
                    return false;
            }
            return true;
        }

        private void WriteChunk(BinaryWriter writer, SparseChunkType type, uint blocks, 
                                uint fillValue, List<byte[]> rawData, ushort chunkHeaderSize)
        {
            writer.Write((ushort)type);
            writer.Write((ushort)0); // reserved
            writer.Write(blocks);
            
            switch (type)
            {
                case SparseChunkType.Raw:
                    uint rawSize = (uint)(chunkHeaderSize + rawData.Sum(b => b.Length));
                    writer.Write(rawSize);
                    foreach (var block in rawData)
                        writer.Write(block);
                    break;
                    
                case SparseChunkType.Fill:
                    writer.Write((uint)(chunkHeaderSize + 4));
                    writer.Write(fillValue);
                    break;
                    
                case SparseChunkType.DontCare:
                    writer.Write((uint)chunkHeaderSize);
                    break;
            }
        }

        #endregion

        #region 从设备读取

        /// <summary>
        /// 从设备读取分区为 Raw 格式
        /// </summary>
        public async Task<bool> ReadPartitionAsRawAsync(
            PartitionInfo partition,
            string outputFile,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            if (_firehose == null)
            {
                _log("[读取] 未连接设备");
                return false;
            }
            
            _log($"[读取] 从设备读取分区 {partition.Name} 为 Raw 格式");
            
            return await _firehose.ReadPartitionChunkedAsync(
                savePath: outputFile,
                startSector: partition.StartLba.ToString(),
                numSectors: (long)partition.Sectors,
                lun: partition.Lun.ToString(),
                progress: progress,
                ct: ct,
                label: partition.Name,
                forceFilename: partition.Name,
                append: false,
                suppressError: true
            );
        }

        /// <summary>
        /// 从设备读取分区并直接转换为 Sparse 格式
        /// </summary>
        public async Task<bool> ReadPartitionAsSparseAsync(
            PartitionInfo partition,
            string outputFile,
            uint blockSize = 4096,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            // 先读取为临时 Raw 文件
            string tempRaw = Path.Combine(Path.GetTempPath(), $"temp_raw_{Guid.NewGuid():N}.img");
            
            try
            {
                _log($"[读取] 从设备读取分区 {partition.Name}");
                
                bool readSuccess = await ReadPartitionAsRawAsync(partition, tempRaw, 
                    (current, total) => progress?.Invoke(current / 2, total), ct);
                
                if (!readSuccess)
                    return false;
                
                _log($"[读取] 转换为 Sparse 格式");
                
                return await RawToSparseAsync(tempRaw, outputFile, blockSize,
                    (current, total) => progress?.Invoke(total / 2 + current / 2, total), ct);
            }
            finally
            {
                try { if (File.Exists(tempRaw)) File.Delete(tempRaw); } catch { }
            }
        }

        #endregion

        #region 写入设备

        /// <summary>
        /// 写入 Raw 镜像到设备
        /// </summary>
        public async Task<bool> WriteRawToDeviceAsync(
            string rawFile,
            PartitionInfo partition,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            if (_firehose == null)
            {
                _log("[写入] 未连接设备");
                return false;
            }
            
            _log($"[写入] 写入 Raw 镜像到分区 {partition.Name}");
            
            // 计算文件包含的扇区数
            long fileSize = new FileInfo(rawFile).Length;
            long numSectors = (fileSize + partition.SectorSize - 1) / partition.SectorSize;
            
            return await _firehose.FlashPartitionAsync(
                filePath: rawFile,
                startSector: partition.StartLba.ToString(),
                numSectors: numSectors,
                lun: partition.Lun.ToString(),
                progress: progress,
                ct: ct,
                label: partition.Name,
                overrideFilename: partition.Name,
                fileOffsetBytes: 0
            );
        }

        /// <summary>
        /// 写入 Sparse 镜像到设备 (自动展开)
        /// </summary>
        public async Task<bool> WriteSparseToDeviceAsync(
            string sparseFile,
            PartitionInfo partition,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            if (_firehose == null)
            {
                _log("[写入] 未连接设备");
                return false;
            }
            
            _log($"[写入] 写入 Sparse 镜像到分区 {partition.Name} (自动展开)");
            
            // 获取 Sparse 镜像的实际大小
            long actualSize;
            using (var fs = new FileStream(sparseFile, FileMode.Open, FileAccess.Read))
            using (var br = new BinaryReader(fs))
            {
                var header = SparseHeader.Read(br);
                actualSize = header.TotalSize;
            }
            
            long numSectors = (actualSize + partition.SectorSize - 1) / partition.SectorSize;
            
            return await _firehose.FlashPartitionAsync(
                filePath: sparseFile,
                startSector: partition.StartLba.ToString(),
                numSectors: numSectors,
                lun: partition.Lun.ToString(),
                progress: progress,
                ct: ct,
                label: partition.Name,
                overrideFilename: partition.Name,
                fileOffsetBytes: 0
            );
        }

        /// <summary>
        /// 自动检测格式并写入设备
        /// </summary>
        public async Task<bool> WriteImageToDeviceAsync(
            string imageFile,
            PartitionInfo partition,
            Action<long, long> progress = null,
            CancellationToken ct = default)
        {
            var format = DetectFormat(imageFile);
            _log($"[写入] 检测到镜像格式: {format}");
            
            if (format == ImageFormat.Sparse)
                return await WriteSparseToDeviceAsync(imageFile, partition, progress, ct);
            else
                return await WriteRawToDeviceAsync(imageFile, partition, progress, ct);
        }

        #endregion

        #region 实用方法

        /// <summary>
        /// 分割 Sparse 镜像为多个小文件
        /// </summary>
        public async Task<List<string>> SplitSparseAsync(
            string sparseFile,
            string outputDir,
            long maxChunkSize = 512 * 1024 * 1024,
            CancellationToken ct = default)
        {
            var outputFiles = new List<string>();
            string baseName = Path.GetFileNameWithoutExtension(sparseFile);
            
            using (var input = new FileStream(sparseFile, FileMode.Open, FileAccess.Read))
            using (var reader = new BinaryReader(input))
            {
                var header = SparseHeader.Read(reader);
                
                if (!header.IsValid)
                {
                    _log("[分割] 无效的 Sparse 文件");
                    return outputFiles;
                }
                
                int partIndex = 0;
                FileStream currentOutput = null;
                BinaryWriter currentWriter = null;
                long currentSize = 0;
                uint currentChunks = 0;
                uint currentBlocks = 0;
                
                input.Seek(header.FileHeaderSize, SeekOrigin.Begin);
                
                try
                {
                    for (uint i = 0; i < header.TotalChunks; i++)
                    {
                        if (ct.IsCancellationRequested)
                            break;
                        
                        long chunkStart = input.Position;
                        ushort chunkType = reader.ReadUInt16();
                        reader.ReadUInt16();
                        uint chunkBlocks = reader.ReadUInt32();
                        uint totalSize = reader.ReadUInt32();
                        
                        // 检查是否需要创建新文件
                        if (currentOutput == null || currentSize + totalSize > maxChunkSize)
                        {
                            // 更新并关闭当前文件
                            if (currentWriter != null)
                            {
                                UpdateSparseHeader(currentWriter, currentChunks, currentBlocks, header);
                                currentWriter.Dispose();
                                currentOutput.Dispose();
                            }
                            
                            // 创建新文件
                            string partFile = Path.Combine(outputDir, $"{baseName}.part{partIndex:D3}.img");
                            outputFiles.Add(partFile);
                            currentOutput = new FileStream(partFile, FileMode.Create, FileAccess.Write);
                            currentWriter = new BinaryWriter(currentOutput);
                            
                            // 写入占位头
                            header.Write(currentWriter);
                            
                            currentSize = header.FileHeaderSize;
                            currentChunks = 0;
                            currentBlocks = 0;
                            partIndex++;
                        }
                        
                        // 复制 chunk 到当前文件
                        input.Seek(chunkStart, SeekOrigin.Begin);
                        byte[] chunkData = reader.ReadBytes((int)totalSize);
                        currentWriter.Write(chunkData);
                        
                        currentSize += totalSize;
                        currentChunks++;
                        currentBlocks += chunkBlocks;
                    }
                    
                    // 更新最后一个文件的头
                    if (currentWriter != null)
                    {
                        UpdateSparseHeader(currentWriter, currentChunks, currentBlocks, header);
                    }
                }
                finally
                {
                    currentWriter?.Dispose();
                    currentOutput?.Dispose();
                }
            }
            
            _log($"[分割] 完成，生成 {outputFiles.Count} 个文件");
            return outputFiles;
        }

        private void UpdateSparseHeader(BinaryWriter writer, uint chunks, uint blocks, SparseHeader originalHeader)
        {
            writer.BaseStream.Seek(0, SeekOrigin.Begin);
            var newHeader = new SparseHeader
            {
                Magic = SparseHeader.SPARSE_HEADER_MAGIC,
                MajorVersion = originalHeader.MajorVersion,
                MinorVersion = originalHeader.MinorVersion,
                FileHeaderSize = originalHeader.FileHeaderSize,
                ChunkHeaderSize = originalHeader.ChunkHeaderSize,
                BlockSize = originalHeader.BlockSize,
                TotalBlocks = blocks,
                TotalChunks = chunks,
                ImageChecksum = 0
            };
            newHeader.Write(writer);
        }

        /// <summary>
        /// 合并多个 Sparse 镜像文件
        /// </summary>
        public async Task<bool> MergeSparseAsync(
            List<string> sparseFiles,
            string outputFile,
            CancellationToken ct = default)
        {
            if (sparseFiles == null || sparseFiles.Count == 0)
                return false;
            
            _log($"[合并] 合并 {sparseFiles.Count} 个 Sparse 文件");
            
            using (var output = new FileStream(outputFile, FileMode.Create, FileAccess.Write))
            using (var writer = new BinaryWriter(output))
            {
                SparseHeader firstHeader = null;
                uint totalChunks = 0;
                uint totalBlocks = 0;
                
                // 先计算总数
                foreach (var file in sparseFiles)
                {
                    using (var input = new FileStream(file, FileMode.Open, FileAccess.Read))
                    using (var reader = new BinaryReader(input))
                    {
                        var header = SparseHeader.Read(reader);
                        if (firstHeader == null)
                            firstHeader = header;
                        
                        totalChunks += header.TotalChunks;
                        totalBlocks += header.TotalBlocks;
                    }
                }
                
                // 写入合并后的头
                var mergedHeader = new SparseHeader
                {
                    Magic = SparseHeader.SPARSE_HEADER_MAGIC,
                    MajorVersion = firstHeader.MajorVersion,
                    MinorVersion = firstHeader.MinorVersion,
                    FileHeaderSize = firstHeader.FileHeaderSize,
                    ChunkHeaderSize = firstHeader.ChunkHeaderSize,
                    BlockSize = firstHeader.BlockSize,
                    TotalBlocks = totalBlocks,
                    TotalChunks = totalChunks,
                    ImageChecksum = 0
                };
                mergedHeader.Write(writer);
                
                // 复制所有 chunk 数据
                foreach (var file in sparseFiles)
                {
                    if (ct.IsCancellationRequested)
                        return false;
                    
                    using (var input = new FileStream(file, FileMode.Open, FileAccess.Read))
                    {
                        input.Seek(firstHeader.FileHeaderSize, SeekOrigin.Begin);
                        await input.CopyToAsync(output, 81920, ct);
                    }
                }
            }
            
            _log("[合并] 完成");
            return true;
        }

        #endregion
    }
}
