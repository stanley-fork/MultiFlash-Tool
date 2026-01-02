using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using OPFlashTool.Qualcomm;
using OPFlashTool.Strategies;
using OPFlashTool.Authentication;

namespace OPFlashTool.Services
{
    /// <summary>
    /// 智能刷写任务配置
    /// </summary>
    public class SmartFlashConfig
    {
        /// <summary>
        /// 固件目录或 XML 文件路径
        /// </summary>
        public string FirmwarePath { get; set; }
        
        /// <summary>
        /// Loader 文件路径 (可选，为空则自动匹配)
        /// </summary>
        public string LoaderPath { get; set; }
        
        /// <summary>
        /// 认证类型
        /// </summary>
        public AuthType AuthType { get; set; } = AuthType.Standard;
        
        /// <summary>
        /// Digest 文件路径 (VIP/Xiaomi 模式需要)
        /// </summary>
        public string DigestPath { get; set; }
        
        /// <summary>
        /// 签名文件路径 (VIP/Xiaomi 模式需要)
        /// </summary>
        public string SignaturePath { get; set; }
        
        /// <summary>
        /// 跳过 Loader 加载 (设备已在 Firehose 模式)
        /// </summary>
        public bool SkipLoader { get; set; }
        
        /// <summary>
        /// LUN5 保护
        /// </summary>
        public bool ProtectLun5 { get; set; } = true;
        
        /// <summary>
        /// 自动擦除分区再写入
        /// </summary>
        public bool EraseBeforeWrite { get; set; }
        
        /// <summary>
        /// 写入完成后重启设备
        /// </summary>
        public bool RebootAfterFlash { get; set; } = true;
        
        /// <summary>
        /// 要刷写的分区列表 (为空则根据 XML 自动确定)
        /// </summary>
        public List<string> PartitionsToFlash { get; set; } = new List<string>();
        
        /// <summary>
        /// 进度回调上下文
        /// </summary>
        public ProgressContext ProgressContext { get; set; }
    }

    /// <summary>
    /// 智能刷写结果
    /// </summary>
    public class SmartFlashResult
    {
        public bool Success { get; set; }
        public string ErrorMessage { get; set; } = "";
        public SmartFlashPhase FailedPhase { get; set; } = SmartFlashPhase.None;
        public int PartitionsWritten { get; set; }
        public int PartitionsFailed { get; set; }
        public TimeSpan ElapsedTime { get; set; }
        public SaharaPblInfo DeviceInfo { get; set; }
        public List<PartitionInfo> PartitionTable { get; set; }
        
        /// <summary>
        /// 需要用户操作 (如选择 Loader)
        /// </summary>
        public bool RequiresUserAction { get; set; }
        
        /// <summary>
        /// 用户操作指引
        /// </summary>
        public string UserGuidance { get; set; } = "";
    }

    /// <summary>
    /// 智能刷写阶段
    /// </summary>
    public enum SmartFlashPhase
    {
        None,
        Connecting,         // 连接设备
        SaharaHandshake,    // Sahara 握手
        LoaderUpload,       // Loader 上传
        FirehoseConfig,     // Firehose 配置
        ReadPartitionTable, // 读取分区表
        ValidatePartitions, // 验证分区
        Flashing,           // 刷写中
        ApplyingPatch,      // 应用补丁
        Rebooting,          // 重启设备
        Completed           // 完成
    }

    /// <summary>
    /// 智能刷写服务 - 自动完成 EDL 刷机全流程
    /// </summary>
    public class SmartFlashService
    {
        private readonly string _baseDir;
        private readonly string _loaderDir;
        private readonly Action<string> _log;
        
        // 当前状态
        private SmartFlashPhase _currentPhase = SmartFlashPhase.None;
        private FirehoseClient _firehose;
        private List<PartitionInfo> _partitionTable;
        private SaharaPblInfo _deviceInfo;

        // 事件
        public event Action<SmartFlashPhase, string> PhaseChanged;
        public event Action<long, long> ProgressChanged;
        public event Action<string> StatusChanged;

        public SmartFlashService(string baseDirectory, Action<string> logger)
        {
            _baseDir = baseDirectory;
            _loaderDir = Path.Combine(baseDirectory, "Loaders");
            _log = logger ?? Console.WriteLine;
            
            if (!Directory.Exists(_loaderDir))
                Directory.CreateDirectory(_loaderDir);
        }

        /// <summary>
        /// 执行智能刷写全流程
        /// 
        /// 流程:
        /// 1. 连接设备 (打开串口)
        /// 2. Sahara 握手 (读取设备信息 + 上传 Loader)
        /// 3. Firehose 配置
        /// 4. 读取分区表 (GPT)
        /// 5. 验证刷写任务
        /// 6. 执行刷写
        /// 7. 应用补丁 (如果有)
        /// 8. 重启设备 (可选)
        /// </summary>
        public async Task<SmartFlashResult> ExecuteSmartFlashAsync(
            string portName,
            SmartFlashConfig config,
            CancellationToken ct = default)
        {
            var result = new SmartFlashResult();
            var sw = System.Diagnostics.Stopwatch.StartNew();
            SerialPort port = null;

            try
            {
                // ========== 阶段 1: 连接设备 ==========
                UpdatePhase(SmartFlashPhase.Connecting, "正在连接设备...");
                
                port = await OpenPortWithRetryAsync(portName, 3, ct);
                if (port == null)
                {
                    result.ErrorMessage = "无法打开串口，请检查设备连接";
                    result.FailedPhase = SmartFlashPhase.Connecting;
                    return result;
                }
                
                _log($"[连接] 串口 {portName} 已打开");

                // ========== 阶段 2: Sahara 握手 ==========
                UpdatePhase(SmartFlashPhase.SaharaHandshake, "正在进行 Sahara 握手...");
                
                if (!config.SkipLoader)
                {
                    var sahara = new SaharaClient(port, _log);
                    
                    // 使用智能握手
                    var handshakeResult = await sahara.SmartHandshakeAsync(
                        userSelectedLoader: config.LoaderPath,
                        loaderDirectory: _loaderDir,
                        enableCloudLookup: false
                    );
                    
                    _deviceInfo = handshakeResult.PblInfo;
                    result.DeviceInfo = _deviceInfo;

                    // 检查握手结果
                    if (!handshakeResult.Success)
                    {
                        if (handshakeResult.RequiresUserAction)
                        {
                            result.RequiresUserAction = true;
                            result.UserGuidance = handshakeResult.UserGuidance;
                            result.FailedPhase = SmartFlashPhase.LoaderUpload;
                            result.ErrorMessage = "需要手动选择 Loader 文件";
                            return result;
                        }
                        
                        result.ErrorMessage = $"Sahara 握手失败: {handshakeResult.ErrorMessage}";
                        result.FailedPhase = SmartFlashPhase.SaharaHandshake;
                        return result;
                    }
                    
                    _log("[Sahara] Loader 上传成功，Firehose 启动中...");
                    await Task.Delay(1500, ct); // 等待 Firehose 启动
                }
                else
                {
                    _log("[跳过] Loader 加载 (设备已在 Firehose 模式)");
                }

                // ========== 阶段 3: Firehose 配置 ==========
                UpdatePhase(SmartFlashPhase.FirehoseConfig, "正在配置 Firehose...");
                
                _firehose = new FirehoseClient(port, _log);
                
                // 智能配置 (自动检测 UFS/eMMC)
                bool configOk = _firehose.Configure();
                if (!configOk)
                {
                    // 尝试其他存储类型
                    configOk = _firehose.Configure("emmc");
                }
                
                if (!configOk)
                {
                    result.ErrorMessage = "Firehose 配置失败，设备可能需要认证";
                    result.FailedPhase = SmartFlashPhase.FirehoseConfig;
                    return result;
                }
                
                _log($"[Firehose] 配置成功 (Storage: {_firehose.StorageType}, Sector: {_firehose.SectorSize})");

                // ========== 阶段 4: 读取分区表 ==========
                UpdatePhase(SmartFlashPhase.ReadPartitionTable, "正在读取分区表...");
                
                var strategy = GetStrategy(config.AuthType);
                _partitionTable = await strategy.ReadGptAsync(_firehose, ct, _log);
                
                if (_partitionTable == null || _partitionTable.Count == 0)
                {
                    result.ErrorMessage = "无法读取分区表 (GPT)";
                    result.FailedPhase = SmartFlashPhase.ReadPartitionTable;
                    return result;
                }
                
                result.PartitionTable = _partitionTable;
                _log($"[GPT] 成功读取 {_partitionTable.Count} 个分区");

                // ========== 阶段 5: 准备刷写任务 ==========
                UpdatePhase(SmartFlashPhase.ValidatePartitions, "正在验证刷写任务...");
                
                var flashTasks = PrepareFlashTasks(config);
                
                if (flashTasks == null || flashTasks.Count == 0)
                {
                    result.ErrorMessage = "没有有效的刷写任务";
                    result.FailedPhase = SmartFlashPhase.ValidatePartitions;
                    return result;
                }
                
                _log($"[任务] 准备刷写 {flashTasks.Count} 个分区");
                
                // 显示任务列表
                foreach (var task in flashTasks)
                {
                    _log($"  → {task.Name} (LUN{task.Lun}, Sector: {task.StartSector})");
                }

                // ========== 阶段 6: 执行刷写 ==========
                UpdatePhase(SmartFlashPhase.Flashing, "正在写入分区...");
                
                var executor = new FlashTaskExecutor(_firehose, strategy, _log, _firehose.SectorSize, config.ProgressContext);
                executor.ProgressChanged += (c, t) => ProgressChanged?.Invoke(c, t);
                executor.StatusChanged += (s) => StatusChanged?.Invoke(s);

                // 获取补丁文件
                var patchFiles = GetPatchFiles(config.FirmwarePath);
                
                await executor.ExecuteFlashTasksAsync(flashTasks, config.ProtectLun5, patchFiles, ct);
                
                result.PartitionsWritten = flashTasks.Count;

                // ========== 阶段 7: 重启设备 ==========
                if (config.RebootAfterFlash)
                {
                    UpdatePhase(SmartFlashPhase.Rebooting, "正在重启设备...");
                    _firehose.Reset();
                    _log("[重启] 设备正在重启...");
                }

                // ========== 完成 ==========
                UpdatePhase(SmartFlashPhase.Completed, "刷写完成！");
                
                result.Success = true;
                sw.Stop();
                result.ElapsedTime = sw.Elapsed;
                
                _log($"[完成] 刷写成功！耗时: {result.ElapsedTime.Minutes}分{result.ElapsedTime.Seconds}秒");
                
                return result;
            }
            catch (OperationCanceledException)
            {
                result.ErrorMessage = "操作已取消";
                result.FailedPhase = _currentPhase;
                return result;
            }
            catch (Exception ex)
            {
                _log($"[错误] 刷写失败: {ex.Message}");
                result.ErrorMessage = ex.Message;
                result.FailedPhase = _currentPhase;
                return result;
            }
            finally
            {
                sw.Stop();
                result.ElapsedTime = sw.Elapsed;
                
                // 清理资源
                try
                {
                    if (_firehose != null)
                    {
                        _firehose.Dispose();
                        _firehose = null;
                    }
                }
                catch { }

                try
                {
                    if (port != null && port.IsOpen)
                    {
                        port.Close();
                    }
                    port?.Dispose();
                }
                catch { }
            }
        }

        /// <summary>
        /// 仅读取设备信息和分区表 (不刷写)
        /// </summary>
        public async Task<SmartFlashResult> ReadDeviceInfoAndPartitionsAsync(
            string portName,
            SmartFlashConfig config,
            CancellationToken ct = default)
        {
            var result = new SmartFlashResult();
            SerialPort port = null;

            try
            {
                // 连接设备
                UpdatePhase(SmartFlashPhase.Connecting, "正在连接设备...");
                port = await OpenPortWithRetryAsync(portName, 3, ct);
                if (port == null)
                {
                    result.ErrorMessage = "无法打开串口";
                    result.FailedPhase = SmartFlashPhase.Connecting;
                    return result;
                }

                // Sahara 握手
                UpdatePhase(SmartFlashPhase.SaharaHandshake, "正在读取设备信息...");
                
                if (!config.SkipLoader)
                {
                    var sahara = new SaharaClient(port, _log);
                    var handshakeResult = await sahara.SmartHandshakeAsync(
                        config.LoaderPath, _loaderDir, false);
                    
                    _deviceInfo = handshakeResult.PblInfo;
                    result.DeviceInfo = _deviceInfo;

                    if (!handshakeResult.Success)
                    {
                        if (handshakeResult.RequiresUserAction)
                        {
                            result.RequiresUserAction = true;
                            result.UserGuidance = handshakeResult.UserGuidance;
                        }
                        result.ErrorMessage = handshakeResult.ErrorMessage;
                        result.FailedPhase = SmartFlashPhase.SaharaHandshake;
                        return result;
                    }
                    
                    await Task.Delay(1500, ct);
                }

                // Firehose 配置
                UpdatePhase(SmartFlashPhase.FirehoseConfig, "正在配置 Firehose...");
                _firehose = new FirehoseClient(port, _log);
                
                if (!_firehose.Configure() && !_firehose.Configure("emmc"))
                {
                    result.ErrorMessage = "Firehose 配置失败";
                    result.FailedPhase = SmartFlashPhase.FirehoseConfig;
                    return result;
                }

                // 读取分区表
                UpdatePhase(SmartFlashPhase.ReadPartitionTable, "正在读取分区表...");
                var strategy = GetStrategy(config.AuthType);
                _partitionTable = await strategy.ReadGptAsync(_firehose, ct, _log);
                
                if (_partitionTable == null || _partitionTable.Count == 0)
                {
                    result.ErrorMessage = "无法读取分区表";
                    result.FailedPhase = SmartFlashPhase.ReadPartitionTable;
                    return result;
                }

                result.PartitionTable = _partitionTable;
                result.Success = true;
                
                _log($"[完成] 成功读取 {_partitionTable.Count} 个分区");
                
                return result;
            }
            catch (Exception ex)
            {
                result.ErrorMessage = ex.Message;
                result.FailedPhase = _currentPhase;
                return result;
            }
            finally
            {
                try { _firehose?.Dispose(); } catch { }
                try { port?.Close(); port?.Dispose(); } catch { }
            }
        }

        #region 私有方法

        private void UpdatePhase(SmartFlashPhase phase, string message)
        {
            _currentPhase = phase;
            _log($"[{phase}] {message}");
            PhaseChanged?.Invoke(phase, message);
            StatusChanged?.Invoke(message);
        }

        private async Task<SerialPort> OpenPortWithRetryAsync(string portName, int maxRetries, CancellationToken ct)
        {
            for (int i = 0; i < maxRetries; i++)
            {
                if (ct.IsCancellationRequested) return null;
                
                try
                {
                    var port = new SerialPort(portName)
                    {
                        BaudRate = 115200,
                        DataBits = 8,
                        Parity = Parity.None,
                        StopBits = StopBits.One,
                        ReadTimeout = 5000,
                        WriteTimeout = 5000
                    };
                    
                    port.Open();
                    return port;
                }
                catch (Exception ex)
                {
                    _log($"[重试] 打开串口失败 ({i + 1}/{maxRetries}): {ex.Message}");
                    await Task.Delay(1000, ct);
                }
            }
            
            return null;
        }

        private IDeviceStrategy GetStrategy(AuthType authType)
        {
            switch (authType)
            {
                case AuthType.Vip:
                    return new OppoVipDeviceStrategy();
                case AuthType.Xiaomi:
                    return new XiaomiDeviceStrategy();
                default:
                    return new StandardDeviceStrategy();
            }
        }

        private List<FlashPartitionInfo> PrepareFlashTasks(SmartFlashConfig config)
        {
            var tasks = new List<FlashPartitionInfo>();
            string firmwarePath = config.FirmwarePath;

            if (string.IsNullOrEmpty(firmwarePath))
                return tasks;

            // 如果是 XML 文件
            if (firmwarePath.EndsWith(".xml", StringComparison.OrdinalIgnoreCase))
            {
                return ParseRawProgramXml(firmwarePath);
            }

            // 如果是目录，查找 rawprogram*.xml
            if (Directory.Exists(firmwarePath))
            {
                var xmlFiles = Directory.GetFiles(firmwarePath, "rawprogram*.xml", SearchOption.AllDirectories);
                foreach (var xmlFile in xmlFiles.OrderBy(f => f))
                {
                    tasks.AddRange(ParseRawProgramXml(xmlFile));
                }
            }

            // 如果指定了特定分区列表，进行过滤
            if (config.PartitionsToFlash != null && config.PartitionsToFlash.Count > 0)
            {
                tasks = tasks.Where(t => 
                    config.PartitionsToFlash.Any(p => 
                        p.Equals(t.Name, StringComparison.OrdinalIgnoreCase))).ToList();
            }

            return tasks;
        }

        private List<FlashPartitionInfo> ParseRawProgramXml(string xmlPath)
        {
            var tasks = new List<FlashPartitionInfo>();
            
            if (!File.Exists(xmlPath))
                return tasks;

            try
            {
                string xmlDir = Path.GetDirectoryName(xmlPath);
                var doc = System.Xml.Linq.XDocument.Load(xmlPath);
                
                foreach (var program in doc.Descendants("program"))
                {
                    string filename = program.Attribute("filename")?.Value ?? "";
                    if (string.IsNullOrWhiteSpace(filename)) continue;
                    
                    string label = program.Attribute("label")?.Value ?? Path.GetFileNameWithoutExtension(filename);
                    string startSector = program.Attribute("start_sector")?.Value ?? "0";
                    string numSectorsStr = program.Attribute("num_partition_sectors")?.Value ?? "0";
                    string lun = program.Attribute("physical_partition_number")?.Value ?? "0";
                    
                    // 构建完整文件路径
                    string fullPath = Path.IsPathRooted(filename) 
                        ? filename 
                        : Path.Combine(xmlDir, filename);
                    
                    if (!File.Exists(fullPath))
                    {
                        _log($"[警告] 文件不存在: {filename}");
                        continue;
                    }

                    long numSectors = 0;
                    long.TryParse(numSectorsStr, out numSectors);

                    var task = new FlashPartitionInfo(
                        lun,
                        label,
                        startSector,
                        numSectors,
                        fullPath,
                        0
                    );
                    
                    tasks.Add(task);
                }
            }
            catch (Exception ex)
            {
                _log($"[错误] 解析 XML 失败: {ex.Message}");
            }

            return tasks;
        }

        private List<string> GetPatchFiles(string firmwarePath)
        {
            var patches = new List<string>();
            
            if (string.IsNullOrEmpty(firmwarePath))
                return patches;

            string searchDir = Directory.Exists(firmwarePath) 
                ? firmwarePath 
                : Path.GetDirectoryName(firmwarePath);

            if (string.IsNullOrEmpty(searchDir) || !Directory.Exists(searchDir))
                return patches;

            try
            {
                patches = Directory.GetFiles(searchDir, "patch*.xml", SearchOption.AllDirectories)
                    .OrderBy(f => f)
                    .ToList();
            }
            catch { }

            return patches;
        }

        #endregion
    }
}
