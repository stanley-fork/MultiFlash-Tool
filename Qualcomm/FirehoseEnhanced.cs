using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace OPFlashTool.Qualcomm
{
    #region 增强型 Firehose 配置 (基于 bkerler/edl)

    /// <summary>
    /// Firehose 配置参数 - 支持自动协商
    /// </summary>
    public class FirehoseConfig
    {
        public string TargetName { get; set; } = "Unknown";
        public string Version { get; set; } = "1";
        public string MemoryName { get; set; } = "UFS";
        public int MaxPayloadSizeToTargetInBytes { get; set; } = 1048576;
        public int MaxPayloadSizeFromTargetInBytes { get; set; } = 8192;
        public int MaxPayloadSizeToTargetInBytesSupported { get; set; } = 1048576;
        public int MaxXMLSizeInBytes { get; set; } = 4096;
        public int SECTOR_SIZE_IN_BYTES { get; set; } = 4096;
        public int ZLPAwareHost { get; set; } = 1;
        public int SkipStorageInit { get; set; } = 0;
        public int SkipWrite { get; set; } = 0;
        public int MaxLun { get; set; } = 6;
        public int TotalBlocks { get; set; } = 0;
        public int BlockSize { get; set; } = 0;
        public int NumPhysical { get; set; } = 0;
        public string ProductName { get; set; } = "Unknown";
        public bool Bit64 { get; set; } = true;
        
        // 自动检测的参数
        public bool IsUFS => MemoryName.ToLower() == "ufs";
        public bool IsEMMC => MemoryName.ToLower() == "emmc";
        public bool IsNAND => MemoryName.ToLower() == "nand";
    }

    #endregion

    #region 异步文件写入器 (基于 bkerler/edl)

    /// <summary>
    /// 高性能异步文件写入器 - 使用队列和后台线程
    /// </summary>
    public class AsyncFileWriter : IDisposable
    {
        private readonly BlockingCollection<byte[]> _writeQueue;
        private readonly Task _writerTask;
        private readonly FileStream _fileStream;
        private volatile bool _stopRequested;
        private long _bytesWritten;

        public long BytesWritten => _bytesWritten;

        public AsyncFileWriter(string filePath, int queueCapacity = 100)
        {
            _writeQueue = new BlockingCollection<byte[]>(queueCapacity);
            _fileStream = new FileStream(filePath, FileMode.Create, FileAccess.Write, FileShare.None, 1024 * 1024, true);
            _writerTask = Task.Run(WriteLoop);
        }

        private void WriteLoop()
        {
            try
            {
                foreach (var data in _writeQueue.GetConsumingEnumerable())
                {
                    if (_stopRequested) break;
                    _fileStream.Write(data, 0, data.Length);
                    _bytesWritten += data.Length;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"AsyncFileWriter error: {ex.Message}");
            }
        }

        public void Write(byte[] data)
        {
            if (!_stopRequested && data != null && data.Length > 0)
            {
                _writeQueue.Add(data);
            }
        }

        public void Flush()
        {
            _writeQueue.CompleteAdding();
            _writerTask.Wait(TimeSpan.FromSeconds(30));
            _fileStream.Flush();
        }

        public void Dispose()
        {
            _stopRequested = true;
            _writeQueue.CompleteAdding();
            try { _writerTask.Wait(TimeSpan.FromSeconds(5)); } catch { }
            _fileStream?.Dispose();
            _writeQueue?.Dispose();
        }
    }

    #endregion

    #region Firehose 响应解析器

    /// <summary>
    /// Firehose XML 响应解析器
    /// </summary>
    public class FirehoseResponse
    {
        public bool Success { get; set; }
        public string Value { get; set; }
        public byte[] RawData { get; set; }
        public string Error { get; set; }
        public Dictionary<string, string> Attributes { get; set; } = new Dictionary<string, string>();
        public List<string> LogMessages { get; set; } = new List<string>();
        public bool RawMode { get; set; }
        public bool NeedsAuth { get; set; }

        public static FirehoseResponse Parse(byte[] data)
        {
            var response = new FirehoseResponse { RawData = data };
            
            try
            {
                string content = Encoding.UTF8.GetString(data);
                
                // 提取 response 标签的属性
                var responseMatch = Regex.Match(content, @"<response\s+([^>]*)/?>");
                if (responseMatch.Success)
                {
                    string attrs = responseMatch.Groups[1].Value;
                    
                    // 提取 value 属性
                    var valueMatch = Regex.Match(attrs, @"value=""([^""]*)""");
                    if (valueMatch.Success)
                    {
                        response.Value = valueMatch.Groups[1].Value;
                        response.Success = response.Value == "ACK" || response.Value == "true";
                    }
                    
                    // 提取 rawmode 属性
                    var rawmodeMatch = Regex.Match(attrs, @"rawmode=""([^""]*)""");
                    if (rawmodeMatch.Success)
                    {
                        response.RawMode = rawmodeMatch.Groups[1].Value == "true";
                    }
                    
                    // 提取所有属性
                    var attrMatches = Regex.Matches(attrs, @"(\w+)=""([^""]*)""");
                    foreach (Match m in attrMatches)
                    {
                        response.Attributes[m.Groups[1].Value] = m.Groups[2].Value;
                    }
                }
                
                // 提取 log 消息
                var logMatches = Regex.Matches(content, @"<log\s+value=""([^""]*)""\s*/>");
                foreach (Match m in logMatches)
                {
                    response.LogMessages.Add(m.Groups[1].Value);
                }
                
                // 检查是否需要认证
                response.NeedsAuth = content.Contains("Authenticate") || 
                                    content.Contains("Only nop and sig tag can be") ||
                                    content.Contains("AUTH");
                
                // 提取错误信息
                if (response.Value == "NAK" && response.LogMessages.Count > 0)
                {
                    response.Error = string.Join("; ", response.LogMessages);
                }
            }
            catch (Exception ex)
            {
                response.Error = ex.Message;
            }
            
            return response;
        }
    }

    #endregion

    #region 增强型 Firehose 客户端

    /// <summary>
    /// 增强型 Firehose 客户端 - 基于 bkerler/edl 优化
    /// 功能特性:
    /// - 智能配置协商 (自动检测 UFS/eMMC/NAND)
    /// - 异步高速传输
    /// - 支持函数检测
    /// - 小米/OPPO 认证支持
    /// - 自动重试机制
    /// - A/B Slot 切换
    /// </summary>
    public class FirehoseEnhanced
    {
        private readonly FirehoseClient _client;
        private readonly Action<string> _log;
        private FirehoseConfig _config;
        private List<string> _supportedFunctions = new List<string>();
        private uint _serial;
        private string _deviceModel;
        private Dictionary<int, long> _lunSizes = new Dictionary<int, long>();

        // 默认支持的函数列表
        private static readonly string[] DefaultFunctions = new[]
        {
            "configure", "program", "firmwarewrite", "patch", "setbootablestoragedrive",
            "ufs", "emmc", "power", "benchmark", "read", "getstorageinfo",
            "getcrc16digest", "getsha256digest", "erase", "peek", "poke", "nop", "xml"
        };

        public FirehoseConfig Config => _config;
        public IReadOnlyList<string> SupportedFunctions => _supportedFunctions;
        public uint SerialNumber => _serial;
        public string DeviceModel => _deviceModel;

        public FirehoseEnhanced(FirehoseClient client, Action<string> logger)
        {
            _client = client;
            _log = logger;
            _config = new FirehoseConfig();
        }

        #region 智能配置协商

        /// <summary>
        /// 智能配置 - 自动检测存储类型和参数
        /// </summary>
        public async Task<bool> SmartConfigureAsync(string preferredMemory = null, CancellationToken ct = default)
        {
            _log?.Invoke("[Config] 开始智能配置...");
            
            // 首先尝试首选存储类型
            if (!string.IsNullOrEmpty(preferredMemory))
            {
                _config.MemoryName = preferredMemory;
            }
            
            return await ConfigureWithRetryAsync(0, ct);
        }

        private async Task<bool> ConfigureWithRetryAsync(int level, CancellationToken ct)
        {
            if (level > 3)
            {
                _log?.Invoke("[Config] 配置重试次数过多，放弃");
                return false;
            }

            // 设置默认扇区大小
            if (_config.SECTOR_SIZE_IN_BYTES == 0)
            {
                _config.SECTOR_SIZE_IN_BYTES = _config.IsEMMC ? 512 : 4096;
            }

            string connectCmd = BuildConfigureXml();
            _log?.Invoke($"[Config] 发送配置 (Level {level}): {_config.MemoryName}, Sector: {_config.SECTOR_SIZE_IN_BYTES}");

            var response = await SendXmlAsync(connectCmd, ct);

            if (!response.Success)
            {
                return await HandleConfigureFailure(response, level, ct);
            }

            // 解析响应中的参数
            ParseConfigResponse(response);

            // 验证配置 - 尝试读取第一个扇区
            _log?.Invoke("[Config] 验证配置...");
            var testRead = await TestReadFirstSectorAsync(ct);
            
            if (!testRead.success)
            {
                return await HandleReadTestFailure(testRead.error, level, ct);
            }

            // 解析存储信息
            await ParseStorageInfoAsync(ct);

            _log?.Invoke($"[Config] 配置成功!");
            _log?.Invoke($"  TargetName: {_config.TargetName}");
            _log?.Invoke($"  MemoryName: {_config.MemoryName}");
            _log?.Invoke($"  SectorSize: {_config.SECTOR_SIZE_IN_BYTES}");
            _log?.Invoke($"  MaxPayload: {_config.MaxPayloadSizeToTargetInBytes / 1024}KB");
            _log?.Invoke($"  MaxLun: {_config.MaxLun}");

            return true;
        }

        private string BuildConfigureXml()
        {
            return $"<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>" +
                   $"<configure MemoryName=\"{_config.MemoryName}\" " +
                   $"Verbose=\"0\" " +
                   $"AlwaysValidate=\"0\" " +
                   $"MaxDigestTableSizeInBytes=\"2048\" " +
                   $"MaxPayloadSizeToTargetInBytes=\"{_config.MaxPayloadSizeToTargetInBytes}\" " +
                   $"ZLPAwareHost=\"{_config.ZLPAwareHost}\" " +
                   $"SkipStorageInit=\"{_config.SkipStorageInit}\" " +
                   $"SkipWrite=\"{_config.SkipWrite}\"/>" +
                   "</data>";
        }

        private async Task<bool> HandleConfigureFailure(FirehoseResponse response, int level, CancellationToken ct)
        {
            // 检查是否需要认证
            if (response.NeedsAuth)
            {
                _log?.Invoke("[Config] 检测到需要 EDL 认证");
                // TODO: 调用认证模块
                return false;
            }

            // 检查错误消息
            foreach (var line in response.LogMessages)
            {
                if (line.Contains("Not support configure MemoryName eMMC"))
                {
                    _log?.Invoke("[Config] eMMC 不支持，尝试 UFS...");
                    _config.MemoryName = "UFS";
                    return await ConfigureWithRetryAsync(level + 1, ct);
                }
                else if (line.Contains("Not support configure MemoryName UFS") || 
                         line.Contains("Failed to open the SDCC Device"))
                {
                    _log?.Invoke("[Config] UFS 不支持，尝试 eMMC...");
                    _config.MemoryName = "eMMC";
                    _config.SECTOR_SIZE_IN_BYTES = 512;
                    return await ConfigureWithRetryAsync(level + 1, ct);
                }
                else if (line.Contains("MaxPayloadSizeToTargetInBytes"))
                {
                    // 解析返回的参数并重试
                    ParseConfigFromLog(line);
                    return await ConfigureWithRetryAsync(level + 1, ct);
                }
            }

            // 尝试从响应数据中提取参数
            if (response.Attributes.Count > 0)
            {
                ParseConfigFromAttributes(response.Attributes);
                if (level == 0)
                {
                    return await ConfigureWithRetryAsync(level + 1, ct);
                }
            }

            return false;
        }

        private async Task<(bool success, string error)> TestReadFirstSectorAsync(CancellationToken ct)
        {
            try
            {
                string xml = $"<?xml version=\"1.0\" ?><data><read SECTOR_SIZE_IN_BYTES=\"{_config.SECTOR_SIZE_IN_BYTES}\" " +
                            $"num_partition_sectors=\"1\" physical_partition_number=\"0\" start_sector=\"0\"/></data>";
                
                var response = await SendXmlAsync(xml, ct);
                
                if (!response.Success && response.LogMessages.Count > 0)
                {
                    return (false, string.Join("; ", response.LogMessages));
                }
                
                // 读取数据
                if (response.RawMode)
                {
                    byte[] data = await ReadRawDataAsync(_config.SECTOR_SIZE_IN_BYTES, ct);
                    var ackResponse = await WaitForAckAsync(ct);
                    return (ackResponse.Success, ackResponse.Error);
                }
                
                return (response.Success, response.Error);
            }
            catch (Exception ex)
            {
                return (false, ex.Message);
            }
        }

        private async Task<bool> HandleReadTestFailure(string error, int level, CancellationToken ct)
        {
            if (error.Contains("SECTOR_SIZE_IN_BYTES") && error.Contains("512"))
            {
                _log?.Invoke("[Config] 扇区大小不匹配，切换到 512...");
                _config.SECTOR_SIZE_IN_BYTES = 512;
                return await ConfigureWithRetryAsync(level + 1, ct);
            }
            else if (error.Contains("SECTOR_SIZE_IN_BYTES") && error.Contains("4096"))
            {
                _log?.Invoke("[Config] 扇区大小不匹配，切换到 4096...");
                _config.SECTOR_SIZE_IN_BYTES = 4096;
                return await ConfigureWithRetryAsync(level + 1, ct);
            }
            else if (error.Contains("Failed to set the IO options") && _config.MemoryName != "nand")
            {
                _log?.Invoke("[Config] IO 选项失败，尝试 NAND...");
                _config.MemoryName = "nand";
                return await ConfigureWithRetryAsync(level + 1, ct);
            }
            else if (error.Contains("Failed to open the SDCC Device") && _config.MemoryName != "UFS")
            {
                _log?.Invoke("[Config] SDCC 打开失败，尝试 UFS...");
                _config.MemoryName = "UFS";
                _config.SECTOR_SIZE_IN_BYTES = 4096;
                return await ConfigureWithRetryAsync(level + 1, ct);
            }
            else if (error.Contains("Failed to initialize") && error.Contains("UFS") && _config.MemoryName != "eMMC")
            {
                _log?.Invoke("[Config] UFS 初始化失败，尝试 eMMC...");
                _config.MemoryName = "eMMC";
                _config.SECTOR_SIZE_IN_BYTES = 512;
                return await ConfigureWithRetryAsync(level + 1, ct);
            }

            _log?.Invoke($"[Config] 读取测试失败: {error}");
            return false;
        }

        private void ParseConfigResponse(FirehoseResponse response)
        {
            if (response.Attributes.TryGetValue("MaxPayloadSizeToTargetInBytes", out string maxPayload))
            {
                if (int.TryParse(maxPayload, out int val)) _config.MaxPayloadSizeToTargetInBytes = val;
            }
            if (response.Attributes.TryGetValue("MaxPayloadSizeToTargetInBytesSupported", out string maxPayloadSupported))
            {
                if (int.TryParse(maxPayloadSupported, out int val)) _config.MaxPayloadSizeToTargetInBytesSupported = val;
            }
            if (response.Attributes.TryGetValue("MaxPayloadSizeFromTargetInBytes", out string maxFrom))
            {
                if (int.TryParse(maxFrom, out int val)) _config.MaxPayloadSizeFromTargetInBytes = val;
            }
            if (response.Attributes.TryGetValue("MaxXMLSizeInBytes", out string maxXml))
            {
                if (int.TryParse(maxXml, out int val)) _config.MaxXMLSizeInBytes = val;
            }
            if (response.Attributes.TryGetValue("MemoryName", out string memName))
            {
                _config.MemoryName = memName;
            }
            if (response.Attributes.TryGetValue("TargetName", out string targetName))
            {
                _config.TargetName = targetName;
                if (!_config.TargetName.StartsWith("MSM")) _config.TargetName = "MSM" + _config.TargetName;
            }
            if (response.Attributes.TryGetValue("Version", out string version))
            {
                _config.Version = version;
            }
        }

        private void ParseConfigFromLog(string line)
        {
            // 从日志行中提取配置参数
            var match = Regex.Match(line, @"MaxPayloadSizeToTargetInBytes[^\d]*(\d+)");
            if (match.Success && int.TryParse(match.Groups[1].Value, out int val))
            {
                _config.MaxPayloadSizeToTargetInBytes = val;
            }
        }

        private void ParseConfigFromAttributes(Dictionary<string, string> attrs)
        {
            foreach (var kv in attrs)
            {
                switch (kv.Key)
                {
                    case "MaxPayloadSizeToTargetInBytes":
                        if (int.TryParse(kv.Value, out int maxPayload)) _config.MaxPayloadSizeToTargetInBytes = maxPayload;
                        break;
                    case "MemoryName":
                        _config.MemoryName = kv.Value;
                        break;
                    case "TargetName":
                        _config.TargetName = kv.Value;
                        break;
                }
            }
        }

        #endregion

        #region 支持函数检测

        /// <summary>
        /// 获取设备支持的函数列表
        /// </summary>
        public async Task<List<string>> GetSupportedFunctionsAsync(CancellationToken ct = default)
        {
            _log?.Invoke("[NOP] 检测支持的函数...");
            
            string xml = "<?xml version=\"1.0\" ?><data><nop /></data>";
            var response = await SendXmlAsync(xml, ct, skipResponse: true);

            _supportedFunctions.Clear();
            bool supFuncMode = false;

            // 继续读取所有响应
            var allResponses = await ReadAllResponsesAsync(ct);

            foreach (var line in allResponses)
            {
                // 检测序列号
                if (line.ToLower().Contains("chip serial num"))
                {
                    var serialMatch = Regex.Match(line, @"0x([0-9a-fA-F]+)");
                    if (serialMatch.Success)
                    {
                        _serial = Convert.ToUInt32(serialMatch.Groups[1].Value, 16);
                        _log?.Invoke($"[NOP] 序列号: {_serial}");
                    }
                }

                // 解析支持的函数
                if (supFuncMode && !line.ToLower().Contains("end of supported functions"))
                {
                    string func = line.Replace("INFO:", "").Trim();
                    if (!string.IsNullOrWhiteSpace(func) && !_supportedFunctions.Contains(func))
                    {
                        _supportedFunctions.Add(func);
                    }
                }

                if (line.ToLower().Contains("supported functions"))
                {
                    supFuncMode = true;
                    // 检查是否在同一行列出了函数
                    int idx = line.IndexOf("Functions:");
                    if (idx != -1)
                    {
                        string funcs = line.Substring(idx + 10);
                        foreach (var f in funcs.Split(' ', ','))
                        {
                            if (!string.IsNullOrWhiteSpace(f) && !_supportedFunctions.Contains(f))
                            {
                                _supportedFunctions.Add(f);
                            }
                        }
                    }
                }
            }

            // 如果没有检测到，使用默认列表
            if (_supportedFunctions.Count == 0)
            {
                _supportedFunctions.AddRange(DefaultFunctions);
                _log?.Invoke("[NOP] 使用默认支持函数列表");
            }
            else
            {
                _log?.Invoke($"[NOP] 检测到 {_supportedFunctions.Count} 个支持的函数");
            }

            return _supportedFunctions;
        }

        /// <summary>
        /// 检查是否支持指定函数
        /// </summary>
        public bool IsFunctionSupported(string functionName)
        {
            return _supportedFunctions.Contains(functionName, StringComparer.OrdinalIgnoreCase);
        }

        #endregion

        #region 存储信息解析

        /// <summary>
        /// 解析存储信息
        /// </summary>
        public async Task<Dictionary<string, string>> ParseStorageInfoAsync(CancellationToken ct = default)
        {
            var info = new Dictionary<string, string>();
            
            try
            {
                string xml = "<?xml version=\"1.0\" ?><data><getstorageinfo physical_partition_number=\"0\"/></data>";
                var response = await SendXmlAsync(xml, ct);

                foreach (var line in response.LogMessages)
                {
                    // 解析 key=value 格式
                    if (line.Contains("="))
                    {
                        var parts = line.Split('=');
                        if (parts.Length == 2)
                        {
                            info[parts[0].Trim()] = parts[1].Trim();
                        }
                    }
                    // 解析 key: value 格式
                    else if (line.Contains(":"))
                    {
                        var parts = line.Split(':');
                        if (parts.Length >= 2)
                        {
                            info[parts[0].Trim()] = parts[1].Trim();
                        }
                    }
                    
                    // 解析 JSON 格式的 storage_info
                    if (line.Contains("\"storage_info\""))
                    {
                        try
                        {
                            // 简单解析 JSON
                            ParseStorageInfoJson(line, info);
                        }
                        catch { }
                    }
                }

                // 更新配置
                if (info.TryGetValue("UFS Total Active LU", out string maxLun))
                {
                    if (int.TryParse(maxLun, System.Globalization.NumberStyles.HexNumber, null, out int val))
                        _config.MaxLun = val;
                }
                if (info.TryGetValue("bNumberLu", out string bNumberLu))
                {
                    if (int.TryParse(bNumberLu, out int val))
                        _config.MaxLun = val;
                }
                if (info.TryGetValue("UFS Inquiry Command Output", out string prodName))
                {
                    _config.ProductName = prodName;
                }
                if (info.TryGetValue("SECTOR_SIZE_IN_BYTES", out string sectorSize))
                {
                    if (int.TryParse(sectorSize, out int val))
                        _config.SECTOR_SIZE_IN_BYTES = val;
                }
                if (info.TryGetValue("num_physical_partitions", out string numPhys))
                {
                    if (int.TryParse(numPhys, out int val))
                    {
                        _config.NumPhysical = val;
                        _config.MaxLun = val;
                    }
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[StorageInfo] 解析异常: {ex.Message}");
            }

            return info;
        }

        private void ParseStorageInfoJson(string json, Dictionary<string, string> info)
        {
            // 简单的 JSON 解析
            var matches = Regex.Matches(json, @"""(\w+)""\s*:\s*([""']?)([^,}\]]+)\2");
            foreach (Match m in matches)
            {
                string key = m.Groups[1].Value;
                string value = m.Groups[3].Value.Trim('"', '\'');
                info[key] = value;
            }
        }

        #endregion

        #region LUN 操作

        /// <summary>
        /// 获取所有可用的 LUN
        /// </summary>
        public List<int> GetAvailableLuns(int? specificLun = null)
        {
            if (specificLun.HasValue)
            {
                return new List<int> { specificLun.Value };
            }

            var luns = new List<int>();
            if (_config.IsUFS)
            {
                for (int i = 0; i < _config.MaxLun; i++)
                {
                    luns.Add(i);
                }
            }
            else
            {
                luns.Add(0);
            }
            return luns;
        }

        /// <summary>
        /// 获取 LUN 的总大小 (扇区数)
        /// </summary>
        public async Task<long> GetLunSizeAsync(int lun, CancellationToken ct = default)
        {
            if (_lunSizes.TryGetValue(lun, out long cached))
            {
                return cached;
            }

            try
            {
                // 通过读取 GPT 头来获取总扇区数
                var gptData = await _client.ReadGptAsync(lun.ToString(), 0, 2, "GPT", "gpt.bin", ct);
                if (gptData != null && gptData.Length >= 1024)
                {
                    // GPT 头在扇区 1，offset 32 是 backup_lba (总扇区数 - 1)
                    int offset = _config.SECTOR_SIZE_IN_BYTES + 32;
                    if (offset + 8 <= gptData.Length)
                    {
                        long backupLba = BitConverter.ToInt64(gptData, offset);
                        _lunSizes[lun] = backupLba + 1;
                        return _lunSizes[lun];
                    }
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[LUN] 获取 LUN{lun} 大小失败: {ex.Message}");
            }

            return -1;
        }

        #endregion

        #region 高级读写操作

        /// <summary>
        /// 高速读取分区 (使用异步文件写入器)
        /// </summary>
        public async Task<bool> FastReadPartitionAsync(int lun, long startSector, long numSectors, 
            string outputPath, Action<long, long> progress = null, CancellationToken ct = default)
        {
            _log?.Invoke($"[Read] 快速读取 LUN{lun} @ Sector {startSector}, {numSectors} sectors");

            long totalBytes = numSectors * _config.SECTOR_SIZE_IN_BYTES;
            
            using (var asyncWriter = new AsyncFileWriter(outputPath))
            {
                string xml = $"<?xml version=\"1.0\" ?><data><read SECTOR_SIZE_IN_BYTES=\"{_config.SECTOR_SIZE_IN_BYTES}\" " +
                            $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{lun}\" " +
                            $"start_sector=\"{startSector}\"/></data>";

                var response = await SendXmlAsync(xml, ct);
                
                if (!response.Success && !response.RawMode)
                {
                    _log?.Invoke($"[Read] 命令失败: {response.Error}");
                    return false;
                }

                long bytesRead = 0;
                byte[] buffer = new byte[_config.MaxPayloadSizeFromTargetInBytes > 0 ? 
                    _config.MaxPayloadSizeFromTargetInBytes : 5 * 1024 * 1024];

                while (bytesRead < totalBytes)
                {
                    if (ct.IsCancellationRequested) return false;

                    int toRead = (int)Math.Min(buffer.Length, totalBytes - bytesRead);
                    byte[] data = await ReadRawDataAsync(toRead, ct);
                    
                    if (data == null || data.Length == 0)
                    {
                        _log?.Invoke("[Read] 数据读取超时");
                        break;
                    }

                    asyncWriter.Write(data);
                    bytesRead += data.Length;
                    progress?.Invoke(bytesRead, totalBytes);
                }

                asyncWriter.Flush();

                var ackResponse = await WaitForAckAsync(ct);
                if (!ackResponse.Success)
                {
                    _log?.Invoke($"[Read] ACK 失败: {ackResponse.Error}");
                    return bytesRead >= totalBytes; // 如果已经读完，忽略 ACK 错误
                }

                return true;
            }
        }

        /// <summary>
        /// 并行读取多个分区
        /// </summary>
        public async Task<bool> ParallelReadPartitionsAsync(
            List<(int lun, long startSector, long numSectors, string outputPath)> partitions,
            Action<string, long, long> progress = null, CancellationToken ct = default)
        {
            // 按 LUN 分组
            var groupedByLun = partitions.GroupBy(p => p.lun).OrderBy(g => g.Key);
            
            foreach (var lunGroup in groupedByLun)
            {
                foreach (var part in lunGroup)
                {
                    if (ct.IsCancellationRequested) return false;
                    
                    bool success = await FastReadPartitionAsync(
                        part.lun, part.startSector, part.numSectors, part.outputPath,
                        (read, total) => progress?.Invoke(Path.GetFileName(part.outputPath), read, total),
                        ct);
                    
                    if (!success)
                    {
                        _log?.Invoke($"[Read] 分区读取失败: {part.outputPath}");
                        return false;
                    }
                }
            }
            
            return true;
        }

        #endregion

        #region A/B Slot 切换

        /// <summary>
        /// 设置活动 Slot
        /// </summary>
        public async Task<bool> SetActiveSlotAsync(string slot, CancellationToken ct = default)
        {
            if (slot.ToLower() != "a" && slot.ToLower() != "b")
            {
                _log?.Invoke("[Slot] 错误: 只支持 slot a 或 b");
                return false;
            }

            _log?.Invoke($"[Slot] 切换到 Slot {slot.ToUpper()}...");

            // 这需要修改 GPT 分区表中的 slot flags
            // 完整实现需要：
            // 1. 读取所有 LUN 的 GPT
            // 2. 找到所有 _a 和 _b 分区
            // 3. 修改 partition flags (offset 48-55 in partition entry)
            // 4. 更新 GPT CRC
            // 5. 写回 GPT

            // TODO: 实现完整的 slot 切换逻辑
            _log?.Invoke("[Slot] 功能尚未完全实现");
            return false;
        }

        #endregion

        #region 底层通信

        private async Task<FirehoseResponse> SendXmlAsync(string xml, CancellationToken ct, bool skipResponse = false)
        {
            return await Task.Run(() =>
            {
                try
                {
                    _client.SendXmlCommand(xml, ignoreResponse: skipResponse);
                    
                    if (skipResponse)
                    {
                        return new FirehoseResponse { Success = true };
                    }

                    // 读取响应
                    var data = _client.ReadRawBytes(4096);
                    return FirehoseResponse.Parse(data);
                }
                catch (Exception ex)
                {
                    return new FirehoseResponse { Success = false, Error = ex.Message };
                }
            }, ct);
        }

        private async Task<byte[]> ReadRawDataAsync(int size, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                try
                {
                    return _client.ReadRawBytes(size);
                }
                catch
                {
                    return null;
                }
            }, ct);
        }

        private async Task<FirehoseResponse> WaitForAckAsync(CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                try
                {
                    int maxRetries = 50;
                    StringBuilder responseBuffer = new StringBuilder();

                    while (maxRetries-- > 0)
                    {
                        if (ct.IsCancellationRequested)
                            return new FirehoseResponse { Success = false, Error = "Cancelled" };

                        byte[] data = _client.ReadRawBytes(4096);
                        if (data == null || data.Length == 0)
                        {
                            Thread.Sleep(50);
                            continue;
                        }

                        responseBuffer.Append(Encoding.UTF8.GetString(data));
                        string response = responseBuffer.ToString();

                        if (response.Contains("</data>"))
                        {
                            return FirehoseResponse.Parse(Encoding.UTF8.GetBytes(response));
                        }
                    }

                    return new FirehoseResponse { Success = false, Error = "Timeout waiting for ACK" };
                }
                catch (Exception ex)
                {
                    return new FirehoseResponse { Success = false, Error = ex.Message };
                }
            }, ct);
        }

        private async Task<List<string>> ReadAllResponsesAsync(CancellationToken ct)
        {
            var messages = new List<string>();
            
            await Task.Run(() =>
            {
                try
                {
                    int emptyCount = 0;
                    while (emptyCount < 5)
                    {
                        if (ct.IsCancellationRequested) break;

                        byte[] data = _client.ReadRawBytes(4096);
                        if (data == null || data.Length == 0)
                        {
                            emptyCount++;
                            Thread.Sleep(50);
                            continue;
                        }

                        emptyCount = 0;
                        var response = FirehoseResponse.Parse(data);
                        messages.AddRange(response.LogMessages);

                        if (response.Value == "ACK" || response.Value == "NAK")
                            break;
                    }
                }
                catch { }
            }, ct);

            return messages;
        }

        #endregion
    }

    #endregion

    #region FirehoseClient 扩展方法

    public static class FirehoseClientExtensions
    {
        /// <summary>
        /// 读取原始字节 (扩展方法)
        /// </summary>
        public static byte[] ReadRawBytes(this FirehoseClient client, int count)
        {
            // 使用反射或者在 FirehoseClient 中添加此方法
            // 这里提供一个模拟实现
            try
            {
                var field = typeof(FirehoseClient).GetField("_highSpeedWriter", 
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
                
                if (field != null)
                {
                    var writer = field.GetValue(client) as HighSpeedUsbWriter;
                    if (writer != null && writer.IsOpen)
                    {
                        return writer.Read(count);
                    }
                }
                
                // 回退到 SerialPort
                var portField = typeof(FirehoseClient).GetField("_serialPort",
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
                
                if (portField != null)
                {
                    var port = portField.GetValue(client) as System.IO.Ports.SerialPort;
                    if (port != null && port.IsOpen)
                    {
                        byte[] buffer = new byte[count];
                        int read = port.Read(buffer, 0, count);
                        if (read > 0)
                        {
                            byte[] result = new byte[read];
                            Array.Copy(buffer, result, read);
                            return result;
                        }
                    }
                }
            }
            catch { }
            
            return Array.Empty<byte>();
        }
    }

    #endregion
}
