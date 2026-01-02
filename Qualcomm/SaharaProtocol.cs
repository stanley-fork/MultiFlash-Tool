using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;
using System.Linq;
using System.Text;

namespace OPFlashTool.Qualcomm
{
    // ============================================
    // 基于 UnlockTool 重写的 Sahara 协议实现
    // ============================================

    #region 枚举定义

    /// <summary>
    /// Sahara 协议命令
    /// </summary>
    public enum SaharaCommandId : uint
    {
        Hello = 0x01,           // 设备发送 Hello 请求
        HelloResp = 0x02,       // 主机响应 Hello
        ReadData = 0x03,        // 32位数据读取请求 (老设备)
        EndImageTx = 0x04,      // 镜像传输结束
        Done = 0x05,            // 镜像完成请求
        DoneResp = 0x06,        // 镜像完成响应
        Reset = 0x07,           // 重置请求
        ResetResp = 0x08,       // 重置响应
        MemoryDebug = 0x09,     // 内存调试
        MemoryRead = 0x0A,      // 内存读取
        CmdReady = 0x0B,        // 命令模式就绪
        CmdSwitchMode = 0x0C,   // 切换模式
        CmdExec = 0x0D,         // 执行命令请求
        CmdExecResp = 0x0E,     // 执行命令响应
        CmdExecData = 0x0F,     // 执行命令数据传输
        MemoryDebug64 = 0x10,   // 64位内存调试
        MemoryRead64 = 0x11,    // 64位内存读取
        ReadData64 = 0x12       // 64位数据读取请求 (新设备 SM8450+)
    }

    /// <summary>
    /// Sahara 协议模式
    /// </summary>
    public enum SaharaMode : uint
    {
        ImageTxPending = 0x00,   // 等待镜像传输 (用于加载 Firehose Programmer)
        ImageTxComplete = 0x01,  // 镜像传输完成
        MemoryDebug = 0x02,      // 内存调试模式
        Command = 0x03           // 命令模式 (用于读取设备信息)
    }

    /// <summary>
    /// Sahara 执行命令类型
    /// </summary>
    public enum SaharaExecCmdId : uint
    {
        Nop = 0x00,              // 空操作
        SerialNumRead = 0x01,    // 读取序列号
        MsmHwIdRead = 0x02,      // 读取 MSM 硬件 ID
        OemPkHashRead = 0x03,    // 读取 OEM PK Hash
        SwitchDmssDload = 0x04,  // 切换到 DMSS 下载模式
        SwitchStreamDload = 0x05,// 切换到流下载模式
        ReadDebugData = 0x06,    // 读取调试数据
        GetSblVersion = 0x07     // 获取 SBL 软件版本
    }

    /// <summary>
    /// Sahara 协议状态码
    /// </summary>
    public enum SaharaStatus : uint
    {
        Success32 = 0x00,                    // 成功 (32位模式)
        Success64 = 0x01,                    // 成功 (64位模式)
        ProtocolMismatch = 0x02,             // 协议不匹配
        InvalidTargetProtocol = 0x03,        // 无效的目标协议版本
        InvalidHostProtocol = 0x04,          // 无效的主机协议版本
        InvalidPacketSize = 0x05,            // 无效的数据包大小
        UnexpectedImageId = 0x06,            // 意外的镜像 ID
        InvalidHeaderSize = 0x07,            // 无效的头大小
        InvalidDataSize = 0x08,              // 无效的数据大小
        InvalidImageType = 0x09,             // 无效的镜像类型
        InvalidTxLength = 0x0A,              // 无效的传输长度
        InvalidRxLength = 0x0B,              // 无效的接收长度
        GeneralTxRxError = 0x0C,             // 通用传输/接收错误
        ReadDataError = 0x0D,                // 读取数据错误
        UnsupportedNumPhdrs = 0x0E,          // 不支持的程序头数量
        InvalidPhdrSize = 0x0F,              // 无效的程序头大小
        MultipleSharedSegment = 0x10,        // 多个共享段
        UninitPhdrLocation = 0x11,           // 未初始化的程序头位置
        InvalidDestAddress = 0x12,           // 无效的目标地址
        InvalidImgHdrDataSize = 0x13,        // 无效的镜像头数据大小
        InvalidElfHeader = 0x14,             // 无效的 ELF 头
        UnknownHostError = 0x15,             // 未知的主机错误
        TimeoutRx = 0x16,                    // 接收超时
        TimeoutTx = 0x17,                    // 发送超时
        InvalidHostMode = 0x18,              // 无效的主机模式
        InvalidMemoryRead = 0x19,            // 无效的内存读取
        InvalidDataSizeRequest = 0x1A,       // 无效的数据大小请求
        MemoryDebugNotSupported = 0x1B,      // 不支持内存调试
        InvalidModeSwitch = 0x1C,            // 无效的模式切换
        CommandExecFailure = 0x1D,           // 命令执行失败
        ExecCmdInvalidParam = 0x1E,          // 无效的命令参数
        ExecCmdUnsupported = 0x1F,           // 不支持的命令
        ExecDataInvalidClientCmd = 0x20,     // 无效的客户端命令
        HashTableAuthFailure = 0x21,         // 哈希表认证失败
        HashVerificationFailure = 0x22,      // 哈希验证失败
        HashTableNotFound = 0x23             // 未找到哈希表
    }

    /// <summary>
    /// Sahara 镜像 ID
    /// </summary>
    public enum SaharaImageId : uint
    {
        None = 0x00,
        OemSbl = 0x01,
        Amss = 0x02,
        Ocbl = 0x03,
        Hash = 0x04,
        Appbl = 0x05,
        Apps = 0x06,
        HostDl = 0x07,
        Dsp1 = 0x08,
        Fsbl = 0x09,
        Dbl = 0x0A,
        Osbl = 0x0B,
        Dsp2 = 0x0C,
        Ehostdl = 0x0D,
        Firehose = 0x0E,  // Firehose Programmer
        Norprg = 0x0F,
        Ramfs1 = 0x10,
        Ramfs2 = 0x11,
        AdspQ5 = 0x12,
        AppsKernel = 0x13,
        BackupRamfs = 0x14,
        Sbl1 = 0x15,
        Sbl2 = 0x16,
        Rpm = 0x17,
        Sbl3 = 0x18,
        Tz = 0x19,
        SsdKeys = 0x1A,
        Gen = 0x1B,
        Dsp3 = 0x1C,
        Acdb = 0x1D,
        Wdt = 0x1E,
        Mba = 0x1F
    }

    #endregion

    #region 数据结构

    /// <summary>
    /// Sahara 协议头 (8 字节)
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaHeader
    {
        public SaharaCommandId Command;
        public uint Length;
    }

    /// <summary>
    /// Hello 请求包 (48 字节) - 设备发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaHelloRequest
    {
        public SaharaHeader Header;
        public uint Version;
        public uint MinVersion;
        public uint MaxCommandPacketSize;
        public SaharaMode Mode;
        public uint Reserved1;
        public uint Reserved2;
        public uint Reserved3;
        public uint Reserved4;
        public uint Reserved5;
        public uint Reserved6;
    }

    /// <summary>
    /// Hello 响应包 (48 字节) - 主机发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaHelloResp
    {
        public SaharaHeader Header;
        public uint Version;
        public uint VersionSupported;
        public uint Status;
        public SaharaMode Mode;
        public uint Reserved0;
        public uint Reserved1;
        public uint Reserved2;
        public uint Reserved3;
        public uint Reserved4;
        public uint Reserved5;
    }

    /// <summary>
    /// 读取数据请求 (32位) - 设备发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaReadDataRequest
    {
        public SaharaHeader Header;
        public SaharaImageId ImageId;
        public uint Offset;
        public uint Length;
    }

    /// <summary>
    /// 读取数据请求 (64位) - 设备发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaReadDataRequest64
    {
        public SaharaHeader Header;
        public SaharaImageId ImageId;
        public long Offset;
        public long Length;
    }

    /// <summary>
    /// 镜像传输结束包 - 设备发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaEndImageTransfer
    {
        public SaharaHeader Header;
        public uint ImageId;
        public SaharaStatus Status;
    }

    /// <summary>
    /// 镜像完成请求包 - 主机发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaImageDoneRequest
    {
        public SaharaHeader Header;
    }

    /// <summary>
    /// 镜像完成响应包 - 设备发送
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaImageDoneResponse
    {
        public SaharaHeader Header;
        public SaharaStatus Status;
    }

    /// <summary>
    /// 切换模式包
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaSwitchMode
    {
        public SaharaHeader Header;
        public SaharaMode Mode;
    }

    /// <summary>
    /// 执行命令请求包
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaCmdExec
    {
        public SaharaHeader Header;
        public SaharaExecCmdId ClientCommand;
    }

    /// <summary>
    /// 执行命令响应包
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct SaharaCmdExecResponse
    {
        public SaharaHeader Header;
        public SaharaExecCmdId Command;
        public uint DataLength;
    }

    /// <summary>
    /// PBL 设备信息
    /// </summary>
    public class SaharaPblInfo
    {
        public string Serial { get; set; } = "";
        public string MsmId { get; set; } = "";
        public string PkHash { get; set; } = "";
        public string OemId { get; set; } = "";
        public string ModelId { get; set; } = "";
        public string ChipName { get; set; } = "";
        public int PblSw { get; set; }
        public bool Is64Bit { get; set; }
        public uint SaharaVersion { get; set; } = 2;
    }

    /// <summary>
    /// 智能握手结果
    /// </summary>
    public class SaharaHandshakeResult
    {
        /// <summary>
        /// 是否成功进入 Firehose 模式
        /// </summary>
        public bool Success { get; set; }
        
        /// <summary>
        /// 设备信息已读取
        /// </summary>
        public bool DeviceInfoRead { get; set; }
        
        /// <summary>
        /// PBL 设备信息
        /// </summary>
        public SaharaPblInfo PblInfo { get; set; }
        
        /// <summary>
        /// 需要用户操作
        /// </summary>
        public bool RequiresUserAction { get; set; }
        
        /// <summary>
        /// 用户操作指引
        /// </summary>
        public string UserGuidance { get; set; } = "";
        
        /// <summary>
        /// 推荐的 Loader 文件名模式
        /// </summary>
        public string RecommendedLoaderPattern { get; set; } = "";
        
        /// <summary>
        /// 错误信息
        /// </summary>
        public string ErrorMessage { get; set; } = "";
        
        /// <summary>
        /// 设备当前状态
        /// </summary>
        public SaharaDeviceState DeviceState { get; set; } = SaharaDeviceState.Unknown;
    }

    /// <summary>
    /// Sahara 设备状态
    /// </summary>
    public enum SaharaDeviceState
    {
        Unknown,            // 未知
        WaitingForLoader,   // 等待 Loader
        LoaderUploaded,     // Loader 已上传
        FirehoseReady,      // Firehose 就绪
        CommandMode,        // 命令模式
        Disconnected,       // 已断开
        Error               // 错误
    }

    #endregion

    /// <summary>
    /// Sahara 协议客户端 (基于 UnlockTool 重写)
    /// </summary>
    public class SaharaClient
    {
        private SerialPort _port;
        private Action<string> _log;
        private SaharaPblInfo _pblInfo = new SaharaPblInfo();
        private bool _is64Bit = false;
        private SaharaMode _currentMode = SaharaMode.ImageTxPending;

        // 进度回调
        public Action<long, long> OnProgress { get; set; }

        /// <summary>
        /// 获取 PBL 信息
        /// </summary>
        public SaharaPblInfo PblInfo => _pblInfo;

        /// <summary>
        /// 是否为 64 位模式
        /// </summary>
        public bool Is64Bit => _is64Bit;

        public SaharaClient(SerialPort port, Action<string> logger = null)
        {
            _port = port;
            _log = logger ?? Console.WriteLine;
        }

        #region 自动加载器匹配与设备识别

        // 设备识别器实例
        private DeviceIdentifier _deviceIdentifier;
        
        // 最后一次识别结果
        private DeviceIdentifyResult _lastIdentifyResult;
        
        /// <summary>
        /// 获取最后一次设备识别结果
        /// </summary>
        public DeviceIdentifyResult LastIdentifyResult => _lastIdentifyResult;

        /// <summary>
        /// 设置云端 API 端点 (用于设备识别)
        /// </summary>
        public void SetCloudApiEndpoint(string endpoint)
        {
            if (_deviceIdentifier == null)
                _deviceIdentifier = new DeviceIdentifier(_log);
            _deviceIdentifier.CloudApiEndpoint = endpoint;
        }

        /// <summary>
        /// 自动识别设备并加载匹配的 Firehose Programmer
        /// 支持本地数据库和云端查询
        /// </summary>
        /// <param name="loaderDirectory">加载器目录</param>
        /// <returns>成功返回 true</returns>
        public async System.Threading.Tasks.Task<bool> AutoIdentifyAndLoadAsync(string loaderDirectory)
        {
            if (!Directory.Exists(loaderDirectory))
            {
                _log($"[Sahara] 错误: 加载器目录不存在: {loaderDirectory}");
                return false;
            }

            if (_deviceIdentifier == null)
                _deviceIdentifier = new DeviceIdentifier(_log);

            try
            {
                // 第一步：读取设备 PBL 信息
                _log("[Sahara] 正在读取设备信息...");
                var deviceInfo = ReadDeviceInfo();
                
                if (deviceInfo.Count == 0)
                {
                    _log("[Sahara] 无法获取设备信息，尝试直接加载...");
                    return TryLoadFirstAvailable(loaderDirectory);
                }

                // 第二步：智能识别设备
                _log("[Sahara] 正在识别设备...");
                _lastIdentifyResult = await _deviceIdentifier.IdentifyDeviceAsync(_pblInfo);
                
                _log($"[识别结果] {_lastIdentifyResult}");
                _log($"[芯片] {_lastIdentifyResult.ChipName} | 存储: {_lastIdentifyResult.StorageType} | Sahara V{_lastIdentifyResult.SaharaVersion}");
                
                if (_lastIdentifyResult.RequiresAuth)
                {
                    _log($"[认证] 需要 {_lastIdentifyResult.AuthType} 认证");
                }

                // 第三步：查找或下载加载器
                string loaderPath = _deviceIdentifier.FindLoader(_lastIdentifyResult, loaderDirectory);
                
                if (string.IsNullOrEmpty(loaderPath) && !string.IsNullOrEmpty(_lastIdentifyResult.LoaderUrl))
                {
                    _log("[Sahara] 尝试从云端下载加载器...");
                    loaderPath = await _deviceIdentifier.DownloadLoaderAsync(_lastIdentifyResult, loaderDirectory);
                }

                if (!string.IsNullOrEmpty(loaderPath))
                {
                    _log($"[Sahara] 使用加载器: {Path.GetFileName(loaderPath)}");
                    
                    // 等待设备准备就绪
                    Thread.Sleep(500);
                    
                    return HandshakeAndLoad(loaderPath);
                }
                else
                {
                    _log("[Sahara] 未找到匹配的加载器，尝试通用加载器...");
                    return TryLoadFirstAvailable(loaderDirectory);
                }
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 自动识别加载失败: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 同步版本的自动识别并加载
        /// </summary>
        public bool AutoHandshakeAndLoad(string loaderDirectory)
        {
            return AutoIdentifyAndLoadAsync(loaderDirectory).GetAwaiter().GetResult();
        }

        /// <summary>
        /// 仅识别设备（不加载 Firehose）
        /// </summary>
        public async System.Threading.Tasks.Task<DeviceIdentifyResult> IdentifyDeviceOnlyAsync()
        {
            if (_deviceIdentifier == null)
                _deviceIdentifier = new DeviceIdentifier(_log);

            try
            {
                _log("[Sahara] 正在识别设备...");
                var deviceInfo = ReadDeviceInfo();
                
                if (deviceInfo.Count == 0)
                {
                    _log("[Sahara] 无法获取设备信息");
                    return new DeviceIdentifyResult();
                }

                _lastIdentifyResult = await _deviceIdentifier.IdentifyDeviceAsync(_pblInfo);
                
                // 打印详细信息
                _log("═══════════════════════════════════════════");
                _log($"  芯片型号: {_lastIdentifyResult.ChipName}");
                _log($"  设备厂商: {_lastIdentifyResult.Vendor}");
                _log($"  设备型号: {_lastIdentifyResult.Model}");
                _log($"  存储类型: {_lastIdentifyResult.StorageType.ToUpper()}");
                _log($"  Sahara版本: V{_lastIdentifyResult.SaharaVersion}");
                _log($"  推荐策略: {_lastIdentifyResult.RecommendedStrategy}");
                if (_lastIdentifyResult.RequiresAuth)
                    _log($"  认证类型: {_lastIdentifyResult.AuthType}");
                _log("═══════════════════════════════════════════");

                return _lastIdentifyResult;
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 设备识别失败: {ex.Message}");
                return new DeviceIdentifyResult();
            }
        }

        /// <summary>
        /// 上报未知设备到云端
        /// </summary>
        public async System.Threading.Tasks.Task ReportUnknownDeviceAsync(string userNote = "")
        {
            if (_deviceIdentifier == null || _pblInfo == null)
                return;

            await _deviceIdentifier.ReportUnknownDeviceAsync(_pblInfo, userNote);
        }

        /// <summary>
        /// 尝试加载目录中第一个可用的加载器
        /// </summary>
        private bool TryLoadFirstAvailable(string loaderDirectory)
        {
            var patterns = new[] { "*.mbn", "*.elf", "*.bin" };
            
            foreach (var pattern in patterns)
            {
                var files = Directory.GetFiles(loaderDirectory, pattern, SearchOption.AllDirectories)
                    .Where(f => f.ToLower().Contains("prog") || 
                               f.ToLower().Contains("firehose") || 
                               f.ToLower().Contains("xbl") ||
                               f.ToLower().Contains("devprg"))
                    .OrderByDescending(f => new FileInfo(f).Length)
                    .ToArray();

                foreach (var file in files)
                {
                    _log($"[Sahara] 尝试加载: {Path.GetFileName(file)}");
                    
                    if (HandshakeAndLoad(file))
                    {
                        return true;
                    }
                    
                    Thread.Sleep(500); // 等待设备复位
                }
            }

            _log("[Sahara] 所有加载器尝试均失败");
            return false;
        }

        /// <summary>
        /// 获取设备推荐的策略类型
        /// </summary>
        public string GetRecommendedStrategy()
        {
            // 优先使用识别结果
            if (_lastIdentifyResult != null && !string.IsNullOrEmpty(_lastIdentifyResult.RecommendedStrategy))
                return _lastIdentifyResult.RecommendedStrategy;

            if (_pblInfo == null || string.IsNullOrEmpty(_pblInfo.PkHash))
                return "Standard";

            byte[] pkHash = HexStringToBytes(_pblInfo.PkHash);
            
            if (QualcommDatabase.IsVipDevice(pkHash))
                return "OppoVip";
            
            if (QualcommDatabase.IsXiaomiDevice(pkHash))
                return "Xiaomi";

            return "Standard";
        }

        /// <summary>
        /// 获取设备详细信息摘要
        /// </summary>
        public string GetDeviceSummary()
        {
            // 优先使用识别结果
            if (_lastIdentifyResult != null && _lastIdentifyResult.Vendor != "Unknown")
            {
                return $"{_lastIdentifyResult.Vendor} {_lastIdentifyResult.Model} ({_lastIdentifyResult.ChipName}) - {_lastIdentifyResult.RecommendedStrategy}";
            }

            if (_pblInfo == null)
                return "未知设备";

            var parts = new List<string>();
            
            if (!string.IsNullOrEmpty(_pblInfo.ChipName))
                parts.Add($"芯片: {_pblInfo.ChipName}");
            
            if (!string.IsNullOrEmpty(_pblInfo.Serial))
                parts.Add($"序列号: {_pblInfo.Serial}");
            
            if (!string.IsNullOrEmpty(_pblInfo.PkHash))
            {
                byte[] pkHash = HexStringToBytes(_pblInfo.PkHash);
                var (vendor, model, _) = QualcommDatabase.GetDeviceByPkHash(pkHash);
                if (vendor != "Unknown")
                    parts.Add($"厂商: {vendor}");
            }

            parts.Add($"Sahara V{_pblInfo.SaharaVersion}");
            parts.Add(_is64Bit ? "64位" : "32位");

            return string.Join(" | ", parts);
        }

        private byte[] HexStringToBytes(string hex)
        {
            if (string.IsNullOrEmpty(hex)) return new byte[0];
            hex = hex.Replace(" ", "").Replace("-", "").Replace("0x", "");
            if (hex.Length % 2 != 0) return new byte[0];
            
            byte[] bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                try { bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16); }
                catch { return new byte[0]; }
            }
            return bytes;
        }

        #endregion

        #region 智能握手流程 (改进版)

        /// <summary>
        /// 智能握手流程 - 根据用户是否选择 Loader 自动决定行为
        /// 
        /// 流程:
        /// 1. 如果用户选择了 Loader → 读取设备信息 → 自动上传 Loader
        /// 2. 如果没有选择 Loader → 读取设备信息 → 显示信息并指导用户选择
        /// 3. 避免卡死在 Sahara 模式，始终返回清晰的结果
        /// </summary>
        /// <param name="userSelectedLoader">用户选择的 Loader 路径 (可为 null)</param>
        /// <param name="loaderDirectory">Loader 搜索目录 (用于自动匹配)</param>
        /// <param name="enableCloudLookup">是否启用云端查询</param>
        /// <returns>握手结果</returns>
        public async System.Threading.Tasks.Task<SaharaHandshakeResult> SmartHandshakeAsync(
            string userSelectedLoader = null,
            string loaderDirectory = null,
            bool enableCloudLookup = false)
        {
            var result = new SaharaHandshakeResult
            {
                PblInfo = _pblInfo,
                DeviceState = SaharaDeviceState.Unknown
            };

            try
            {
                _log("═══════════════════════════════════════════");
                _log("  Sahara 智能握手流程");
                _log("═══════════════════════════════════════════");

                // ========== 步骤 1: 等待设备 Hello ==========
                _log("[Sahara] 等待设备连接...");
                
                if (!WaitForHello(out uint version))
                {
                    result.ErrorMessage = "未检测到设备 Hello 包";
                    result.DeviceState = SaharaDeviceState.Disconnected;
                    result.UserGuidance = "请确保设备已进入 EDL 模式 (9008)";
                    return result;
                }

                _pblInfo.SaharaVersion = version;
                _log($"[Sahara] 检测到协议版本: V{version}");

                // ========== 步骤 2: 读取设备信息 ==========
                _log("[Sahara] 正在读取设备信息...");
                
                bool infoRead = await ReadDeviceInfoInternalAsync(version);
                result.DeviceInfoRead = infoRead;
                result.PblInfo = _pblInfo;

                // 打印设备信息
                PrintDeviceInfo();

                // ========== 步骤 3: 决定下一步操作 ==========
                
                // 3a. 用户已选择 Loader → 直接使用
                if (!string.IsNullOrEmpty(userSelectedLoader) && File.Exists(userSelectedLoader))
                {
                    _log($"[Sahara] 使用用户指定的 Loader: {Path.GetFileName(userSelectedLoader)}");
                    
                    // 确保回到镜像传输模式
                    await EnsureImageTransferModeAsync(version);
                    
                    bool loadSuccess = HandshakeAndLoad(userSelectedLoader);
                    
                    result.Success = loadSuccess;
                    result.DeviceState = loadSuccess ? SaharaDeviceState.FirehoseReady : SaharaDeviceState.Error;
                    
                    if (!loadSuccess)
                    {
                        result.ErrorMessage = "Loader 上传失败，可能不匹配当前设备";
                        result.UserGuidance = $"请为 {_pblInfo.ChipName} 选择正确的 Firehose Loader";
                    }
                    
                    return result;
                }

                // 3b. 尝试自动匹配 Loader
                string matchedLoader = null;
                
                if (!string.IsNullOrEmpty(loaderDirectory) && Directory.Exists(loaderDirectory))
                {
                    matchedLoader = TryMatchLoader(loaderDirectory);
                }

                // 3c. 找到匹配的 Loader → 自动使用
                if (!string.IsNullOrEmpty(matchedLoader))
                {
                    _log($"[Sahara] 自动匹配 Loader: {Path.GetFileName(matchedLoader)}");
                    
                    await EnsureImageTransferModeAsync(version);
                    
                    bool loadSuccess = HandshakeAndLoad(matchedLoader);
                    
                    result.Success = loadSuccess;
                    result.DeviceState = loadSuccess ? SaharaDeviceState.FirehoseReady : SaharaDeviceState.Error;
                    
                    if (!loadSuccess)
                    {
                        result.ErrorMessage = "自动匹配的 Loader 上传失败";
                        result.RequiresUserAction = true;
                        result.UserGuidance = "请手动选择正确的 Loader 文件";
                    }
                    
                    return result;
                }

                // 3d. 没有找到 Loader → 停止并指导用户
                _log("");
                _log("╔═══════════════════════════════════════════════════════════╗");
                _log("║  ⚠️  未找到匹配的 Firehose Loader                          ║");
                _log("╠═══════════════════════════════════════════════════════════╣");
                _log($"║  芯片: {_pblInfo.ChipName,-20} Sahara: V{version}          ║");
                _log("╠═══════════════════════════════════════════════════════════╣");
                _log("║  请手动选择 Loader 文件:                                   ║");
                _log($"║  推荐文件名: prog_firehose_{_pblInfo.ChipName.ToLower()}*.mbn    ║");
                _log("║                                                           ║");
                _log("║  提示: Loader 通常在固件包的以下位置:                       ║");
                _log("║  - images/ 目录                                           ║");
                _log("║  - BTFM.bin 同目录                                        ║");
                _log("╚═══════════════════════════════════════════════════════════╝");
                _log("");

                // 安全退出 Sahara 模式，避免卡死
                await SafeExitSaharaModeAsync(version);

                result.Success = false;
                result.RequiresUserAction = true;
                result.DeviceState = SaharaDeviceState.WaitingForLoader;
                result.RecommendedLoaderPattern = $"prog_firehose_{_pblInfo.ChipName.ToLower()}";
                result.UserGuidance = GenerateUserGuidance();
                
                return result;
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 智能握手异常: {ex.Message}");
                
                // 尝试安全退出
                try { SendReset(); } catch { }
                
                result.Success = false;
                result.ErrorMessage = ex.Message;
                result.DeviceState = SaharaDeviceState.Error;
                result.UserGuidance = "发生异常，请重新连接设备";
                
                return result;
            }
        }

        /// <summary>
        /// 同步版本的智能握手
        /// </summary>
        public SaharaHandshakeResult SmartHandshake(
            string userSelectedLoader = null,
            string loaderDirectory = null,
            bool enableCloudLookup = false)
        {
            return SmartHandshakeAsync(userSelectedLoader, loaderDirectory, enableCloudLookup)
                .GetAwaiter().GetResult();
        }

        /// <summary>
        /// 内部方法: 读取设备信息 (不改变模式)
        /// </summary>
        private async System.Threading.Tasks.Task<bool> ReadDeviceInfoInternalAsync(uint version)
        {
            try
            {
                // 尝试进入命令模式读取信息
                if (!EnterCommandMode(version))
                {
                    if (version >= 3)
                    {
                        _log("[Sahara V3] 设备拒绝命令模式 (V3 协议预期行为)");
                        _log("[Sahara V3] 无法读取完整设备信息");
                    }
                    return false;
                }

                // 读取序列号
                ReadSerialNumber();
                
                // 读取 MSM 硬件 ID
                ReadMsmHwId();
                
                // 读取 PK Hash (V3 可能失败)
                ReadPkHash();

                return true;
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 读取设备信息异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 打印设备信息
        /// </summary>
        private void PrintDeviceInfo()
        {
            _log("");
            _log("┌─────────────────────────────────────────┐");
            _log("│           设备信息                       │");
            _log("├─────────────────────────────────────────┤");
            
            if (!string.IsNullOrEmpty(_pblInfo.ChipName))
                _log($"│  芯片型号: {_pblInfo.ChipName,-27} │");
            
            if (!string.IsNullOrEmpty(_pblInfo.Serial))
                _log($"│  序列号:   {_pblInfo.Serial,-27} │");
            
            if (!string.IsNullOrEmpty(_pblInfo.MsmId))
                _log($"│  MSM ID:   0x{_pblInfo.MsmId,-25} │");
            
            if (!string.IsNullOrEmpty(_pblInfo.OemId))
                _log($"│  OEM ID:   0x{_pblInfo.OemId,-25} │");
            
            if (!string.IsNullOrEmpty(_pblInfo.ModelId))
                _log($"│  Model ID: 0x{_pblInfo.ModelId,-25} │");
            
            if (!string.IsNullOrEmpty(_pblInfo.PkHash))
            {
                string shortHash = _pblInfo.PkHash.Length > 16 
                    ? _pblInfo.PkHash.Substring(0, 16) + "..." 
                    : _pblInfo.PkHash;
                _log($"│  PK Hash:  {shortHash,-27} │");
            }
            
            _log($"│  Sahara:   V{_pblInfo.SaharaVersion,-26} │");
            _log($"│  模式:     {(_pblInfo.Is64Bit ? "64位" : "32位"),-27} │");
            _log("└─────────────────────────────────────────┘");
            _log("");
        }

        /// <summary>
        /// 尝试匹配本地 Loader
        /// </summary>
        private string TryMatchLoader(string loaderDirectory)
        {
            if (string.IsNullOrEmpty(_pblInfo.ChipName))
                return null;

            string chipLower = _pblInfo.ChipName.ToLower();
            
            // 匹配模式优先级
            var patterns = new[]
            {
                $"prog_firehose_{chipLower}*.mbn",
                $"prog_firehose_{chipLower}*.elf",
                $"*{chipLower}*firehose*.mbn",
                $"*{chipLower}*firehose*.elf",
                $"prog_ufs_firehose*.mbn",
                $"prog_emmc_firehose*.mbn"
            };

            foreach (var pattern in patterns)
            {
                try
                {
                    var files = Directory.GetFiles(loaderDirectory, pattern, SearchOption.AllDirectories);
                    if (files.Length > 0)
                    {
                        // 优先选择最大的文件 (通常是完整版)
                        return files.OrderByDescending(f => new FileInfo(f).Length).First();
                    }
                }
                catch { }
            }

            return null;
        }

        /// <summary>
        /// 确保设备处于镜像传输模式
        /// </summary>
        private async System.Threading.Tasks.Task EnsureImageTransferModeAsync(uint version)
        {
            try
            {
                SwitchMode(SaharaMode.ImageTxPending);
                await System.Threading.Tasks.Task.Delay(200);
                
                // 等待设备响应
                if (!WaitForHello(out _))
                {
                    _log("[Sahara] 设备可能已切换模式，继续...");
                }
            }
            catch { }
        }

        /// <summary>
        /// 安全退出 Sahara 模式
        /// </summary>
        private async System.Threading.Tasks.Task SafeExitSaharaModeAsync(uint version)
        {
            try
            {
                _log("[Sahara] 安全退出 Sahara 模式...");
                
                // 切换到镜像传输待机模式
                SwitchMode(SaharaMode.ImageTxPending);
                await System.Threading.Tasks.Task.Delay(100);
                
                // 发送重置命令让设备重新等待
                // 注意: 不发送 Reset，因为那会导致设备完全复位
                // 我们只是让设备回到等待 Loader 的状态
                
                _log("[Sahara] 设备现在等待 Loader 上传");
                _log("[Sahara] 请选择 Loader 后点击「连接设备」继续");
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 安全退出异常: {ex.Message}");
            }
        }

        /// <summary>
        /// 生成用户操作指引
        /// </summary>
        private string GenerateUserGuidance()
        {
            var sb = new StringBuilder();
            
            sb.AppendLine("请按以下步骤操作:");
            sb.AppendLine();
            sb.AppendLine($"1. 为 {_pblInfo.ChipName} 芯片找到正确的 Firehose Loader");
            sb.AppendLine($"   推荐文件名: prog_firehose_{_pblInfo.ChipName.ToLower()}_*.mbn");
            sb.AppendLine();
            sb.AppendLine("2. Loader 通常在以下位置:");
            sb.AppendLine("   - 官方固件包的 images/ 目录");
            sb.AppendLine("   - 第三方工具的 Loaders/ 目录");
            sb.AppendLine();
            sb.AppendLine("3. 选择 Loader 后点击「连接设备」");
            sb.AppendLine();
            
            if (_pblInfo.SaharaVersion >= 3)
            {
                sb.AppendLine("⚠️ 注意: 您的设备使用 Sahara V3 协议");
                sb.AppendLine("   V3 设备必须使用匹配的签名 Loader");
            }

            return sb.ToString();
        }

        #endregion

        #region 主要功能方法

        /// <summary>
        /// 功能 1: 握手并上传引导程序 (Firehose Programmer)
        /// 基于 UnlockTool 的 sendFlashLoader/sendFlashLoader64
        /// </summary>
        public bool HandshakeAndLoad(string programmerPath)
        {
            if (!File.Exists(programmerPath))
            {
                _log($"[Sahara] 错误: 引导文件不存在: {programmerPath}");
                            return false;
            }

            byte[] loaderBytes = File.ReadAllBytes(programmerPath);
            
            // 验证 ELF 头
            string header = Encoding.UTF8.GetString(loaderBytes.Take(20).ToArray());
            if (!header.ToUpper().Contains("ELF"))
            {
                _log("[Sahara] 警告: 文件可能不是有效的 ELF 格式");
            }

            Stopwatch sw = new Stopwatch();
            sw.Start();

            try
            {
                // 等待 Hello 包
                if (!WaitForHello(out uint version))
                    {
                    _log("[Sahara] 错误: 未检测到设备 Hello");
                        return false;
                    }

                _pblInfo.SaharaVersion = version;
                _log($"[Sahara] 检测到协议版本: V{version}");

                // 发送 Hello 响应，进入镜像传输模式
                SendHelloResponse(SaharaMode.ImageTxPending, version);

                // 等待 ReadData 请求并上传 Loader
                return _is64Bit 
                    ? SendFlashLoader64(loaderBytes, sw) 
                    : SendFlashLoader32(loaderBytes, sw);
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 通信异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 功能 2: 读取设备信息 (无需引导文件)
        /// 基于 UnlockTool 的 dumpDeviceInfo
        /// </summary>
        public Dictionary<string, string> ReadDeviceInfo()
        {
            var info = new Dictionary<string, string>();
            
            try
            {
                // 等待 Hello 包
                if (!WaitForHello(out uint version))
                {
                    _log("[Sahara] 未检测到设备");
                    return info;
                }

                _pblInfo.SaharaVersion = version;
                info["SaharaVersion"] = version.ToString();
                _log($"[Sahara] 检测到协议版本: V{version}");

                // 尝试进入命令模式
                if (!EnterCommandMode(version))
                {
                    if (version >= 3)
                    {
                        _log("[Sahara V3] 设备拒绝命令模式，这是 V3 协议的预期行为");
                        _log("[Sahara V3] 请手动指定引导文件 (Loader)");
                        info["IsV3"] = "true";
                    }
                    else
                    {
                        _log("[Sahara] 设备拒绝命令模式，尝试复位");
                    }
                    SendReset();
                    return info;
                }

                _log("[Sahara] 正在读取 PBL 信息...");

                // 读取序列号
                ReadSerialNumber();
                if (!string.IsNullOrEmpty(_pblInfo.Serial))
                {
                    info["SerialNumber"] = _pblInfo.Serial;
                    _log($"[Sahara] 序列号: {_pblInfo.Serial}");
                }

                // 读取 MSM 硬件 ID
                ReadMsmHwId();
                if (!string.IsNullOrEmpty(_pblInfo.MsmId))
                {
                    info["MSM_HWID"] = _pblInfo.MsmId;
                    info["OEM_ID"] = _pblInfo.OemId;
                    info["Model_ID"] = _pblInfo.ModelId;
                    info["ChipName"] = _pblInfo.ChipName;
                    
                    _log($"[Sahara] SoC: {_pblInfo.ChipName} [0x{_pblInfo.MsmId}]");
                    _log($"[Sahara] OEM: {_pblInfo.OemId}, Model: {_pblInfo.ModelId}");

                    // 检测 Sahara V3
                    int saharaVer = QualcommDatabase.GetSaharaVersion(_pblInfo.ChipName);
                            if (saharaVer >= 3)
                            {
                                info["IsV3"] = "true";
                        _log($"[Sahara] 芯片 {_pblInfo.ChipName} 使用 V{saharaVer} 协议");
                    }
                }

                // 读取 PK Hash
                ReadPkHash();
                if (!string.IsNullOrEmpty(_pblInfo.PkHash))
                {
                    info["PK_HASH"] = _pblInfo.PkHash;
                    _log($"[Sahara] PK_HASH[0]: {_pblInfo.PkHash.Substring(0, Math.Min(32, _pblInfo.PkHash.Length))}");
                    if (_pblInfo.PkHash.Length > 32)
                    {
                        _log($"[Sahara] PK_HASH[1]: {_pblInfo.PkHash.Substring(_pblInfo.PkHash.Length - 32)}");
                    }
                }
                else if (version >= 3)
                {
                    _log("[Sahara V3] 无法读取 PK Hash，V3 设备不支持此功能");
                }

                // 切回镜像传输模式
                SwitchMode(SaharaMode.ImageTxPending);
                _log("[Sahara] 设备信息读取完成");
            }
            catch (Exception ex)
            {
                _log($"[Sahara] 读取信息失败: {ex.Message}");
            }

            return info;
        }

        /// <summary>
        /// 功能 3: 尝试切换到 Firehose 模式 (Hang Hack)
        /// 基于 UnlockTool 的 hangHack
        /// </summary>
        public bool TrySwitchToFirehose()
        {
            _log("[Sahara] 尝试切换到 Firehose 模式...");

            var switchPkt = new SaharaSwitchMode
            {
                Header = new SaharaHeader
                {
                    Command = SaharaCommandId.CmdSwitchMode,
                    Length = (uint)Marshal.SizeOf<SaharaSwitchMode>()
                },
                Mode = SaharaMode.ImageTxPending
            };

            WriteStruct(switchPkt);

            for (int retry = 0; retry < 3; retry++)
            {
                Thread.Sleep(500);
                byte[] response = PortRead(100);

                if (response.Length > 0 && Encoding.UTF8.GetString(response).Contains("xml"))
                {
                    _log("[Sahara] 成功切换到 Firehose 模式");
                    _currentMode = SaharaMode.ImageTxComplete;
                    return true;
                }
            }

            _log("[Sahara] 切换失败: 设备未进入 Firehose 模式");
            return false;
        }

        #endregion

        #region 32位 Loader 上传 (基于 UnlockTool sendFlashLoader)

        private bool SendFlashLoader32(byte[] loaderBytes, Stopwatch sw)
        {
            int bytesSent = 0;
            int retryCount = 0;
            const int maxRetries = 25;

            _log("[Sahara] 正在上传引导程序 (32位模式)...");

            while (true)
            {
                byte[] response = PortRead(0);
                
                if (response.Length == 0)
                {
                    response = RetryRead(ref retryCount, maxRetries);
                    if (response == null) return false;
                }

                // 解析响应
                if (response.Length < 8) continue;

                SaharaHeader header = RawDeserialize<SaharaHeader>(response);

                switch (header.Command)
                {
                    case SaharaCommandId.ReadData:
                        if (response.Length >= 20)
                        {
                            var readReq = RawDeserialize<SaharaReadDataRequest>(response);
                            int offset = (int)readReq.Offset;
                            int length = (int)readReq.Length;

                            if (offset + length > loaderBytes.Length)
                            {
                                _log($"[Sahara] 错误: 请求越界 Offset={offset}, Len={length}, FileSize={loaderBytes.Length}");
                                return false;
                            }

                            _port.Write(loaderBytes, offset, length);
                            bytesSent += length;
                            OnProgress?.Invoke(bytesSent, loaderBytes.Length);
                            retryCount = 0;
                        }
                        break;

                    case SaharaCommandId.EndImageTx:
                        // 发送 Done 请求
                        var doneReq = new SaharaImageDoneRequest
                        {
                            Header = new SaharaHeader
                            {
                                Command = SaharaCommandId.Done,
                                Length = (uint)Marshal.SizeOf<SaharaImageDoneRequest>()
                            }
                        };
                        WriteStruct(doneReq);

                        // 等待 Done 响应
                        byte[] doneResp = PortRead(100);
                        if (doneResp.Length >= 12)
                        {
                            var doneResponse = RawDeserialize<SaharaImageDoneResponse>(doneResp);
                            if (doneResponse.Status == SaharaStatus.Success32)
                            {
                                sw.Stop();
                                _log($"[Sahara] 引导上传成功 ({FormatSize(bytesSent)}, {sw.ElapsedMilliseconds}ms)");
                                _currentMode = SaharaMode.ImageTxComplete;
                                return true;
                            }
                        }
                        _log("[Sahara] 引导验证失败: Firehose 不匹配此设备");
                        return false;

                    case SaharaCommandId.Reset:
                        _log("[Sahara] 设备请求复位");
                        return false;

                    default:
                        // 检查是否为 64 位模式
                        if (response.Length == 32)
                        {
                            _is64Bit = true;
                            _pblInfo.Is64Bit = true;
                            _log("[Sahara] 检测到 64 位模式，切换...");
                            return SendFlashLoader64(loaderBytes, sw);
                        }
                        break;
                }
            }
        }

        #endregion

        #region 64位 Loader 上传

        private bool SendFlashLoader64(byte[] loaderBytes, Stopwatch sw)
        {
            int bytesSent = 0;
            int retryCount = 0;
            const int maxRetries = 25;

            _log("[Sahara] 正在上传引导程序 (64位模式)...");

            while (true)
            {
                byte[] response = PortRead(0);

                if (response.Length == 0)
                {
                    response = RetryRead(ref retryCount, maxRetries);
                    if (response == null) return false;
                }

                if (response.Length < 8) continue;

                SaharaHeader header = RawDeserialize<SaharaHeader>(response);

                switch (header.Command)
                {
                    case SaharaCommandId.ReadData64:
                        if (response.Length >= 32)
                        {
                            var readReq = RawDeserialize<SaharaReadDataRequest64>(response);
                            long offset = readReq.Offset;
                            long length = readReq.Length;

                            if (offset + length > loaderBytes.Length)
                            {
                                _log($"[Sahara] 错误: 请求越界 Offset={offset}, Len={length}, FileSize={loaderBytes.Length}");
                                return false;
                            }

                            _port.Write(loaderBytes, (int)offset, (int)length);
                            bytesSent += (int)length;
                            OnProgress?.Invoke(bytesSent, loaderBytes.Length);
                            retryCount = 0;
                        }
                        break;

                    case SaharaCommandId.ReadData:
                        // 回退到 32 位模式
                        _is64Bit = false;
                        _pblInfo.Is64Bit = false;
                        return SendFlashLoader32(loaderBytes, sw);

                    case SaharaCommandId.EndImageTx:
                        var doneReq = new SaharaImageDoneRequest
                        {
                            Header = new SaharaHeader
                            {
                                Command = SaharaCommandId.Done,
                                Length = (uint)Marshal.SizeOf<SaharaImageDoneRequest>()
                            }
                        };
                        WriteStruct(doneReq);

                        byte[] doneResp = PortRead(100);
                        if (doneResp.Length >= 12)
                        {
                            var doneResponse = RawDeserialize<SaharaImageDoneResponse>(doneResp);
                            if (doneResponse.Status == SaharaStatus.Success64)
                            {
                                sw.Stop();
                                _log($"[Sahara] 引导上传成功 ({FormatSize(bytesSent)}, {sw.ElapsedMilliseconds}ms)");
                                _currentMode = SaharaMode.ImageTxComplete;
                                return true;
                            }
                        }
                        _log("[Sahara] 引导验证失败: Firehose 不匹配此设备");
                        return false;

                    case SaharaCommandId.Reset:
                        _log("[Sahara] 设备请求复位");
                        return false;
                }
            }
        }

        #endregion

        #region 命令执行方法 (基于 UnlockTool ReadData)

        private void ReadSerialNumber()
        {
            byte[] data = ExecuteCommand(SaharaExecCmdId.SerialNumRead);
            if (data != null && data.Length >= 4)
            {
                _pblInfo.Serial = HexToDecimal(BitConverter.ToString(data).Replace("-", ""));
            }
        }

        private void ReadMsmHwId()
        {
            byte[] data = ExecuteCommand(SaharaExecCmdId.MsmHwIdRead);
            if (data != null && data.Length > 0)
            {
                Array.Reverse(data);
                string hwid = BitConverter.ToString(data).Replace("-", "");

                if (hwid.Length >= 8)
                {
                    _pblInfo.MsmId = hwid.Substring(0, 8);
                    _pblInfo.ChipName = QualcommDatabase.GetChipName(
                        Convert.ToUInt32(_pblInfo.MsmId, 16));

                    if (hwid.Length >= 12)
                    {
                        _pblInfo.OemId = hwid.Substring(hwid.Length - 8, 4);
                        _pblInfo.ModelId = hwid.Substring(hwid.Length - 4, 4);
                    }
                }
            }
        }

        private void ReadPkHash()
        {
            byte[] data = ExecuteCommand(SaharaExecCmdId.OemPkHashRead);
            if (data != null)
            {
                _pblInfo.PkHash = BitConverter.ToString(data).Replace("-", "").ToLower();
            }
        }

        private byte[] ExecuteCommand(SaharaExecCmdId cmd)
        {
            // 发送执行命令请求
            var execReq = new SaharaCmdExec
            {
                Header = new SaharaHeader
                {
                    Command = SaharaCommandId.CmdExec,
                    Length = (uint)Marshal.SizeOf<SaharaCmdExec>()
                },
                ClientCommand = cmd
            };
            WriteStruct(execReq);
            Thread.Sleep(80);

            // 读取响应
            byte[] response = PortRead(10);
            if (response.Length < 16) return null;

            SaharaHeader header = RawDeserialize<SaharaHeader>(response);
            if (header.Command != SaharaCommandId.CmdExecResp) return null;

            var execResp = RawDeserialize<SaharaCmdExecResponse>(response);
            uint dataLen = execResp.DataLength;

            // 发送数据请求
            var dataReq = new SaharaCmdExec
            {
                Header = new SaharaHeader
                {
                    Command = SaharaCommandId.CmdExecData,
                    Length = (uint)Marshal.SizeOf<SaharaCmdExec>()
                },
                ClientCommand = cmd
            };
            WriteStruct(dataReq);

            // 读取数据
            return PortRead(10);
        }

        #endregion

        #region 辅助方法

        private bool WaitForHello(out uint version)
        {
            version = 2;
            int oldTimeout = _port.ReadTimeout;
            _port.ReadTimeout = 2000;

            try
            {
                for (int i = 0; i < 5; i++)
                {
                    try 
                    {
                        byte[] head = new byte[8];
                        int read = _port.Read(head, 0, 8);
                        if (read < 8) continue;

                        SaharaHeader header = RawDeserialize<SaharaHeader>(head);
                        if (header.Command == SaharaCommandId.Hello && header.Length >= 48)
                        {
                            byte[] body = ReadBytes((int)header.Length - 8);
                            version = BitConverter.ToUInt32(body, 0);

                            // 检测 32/64 位模式
                            if (body.Length >= 16)
                            {
                                uint mode = BitConverter.ToUInt32(body, 12);
                            }

                            _log("[Sahara] 检测到设备 (Hello)");
                            return true;
                        }
                        else if (header.Length > 8)
                        {
                            ReadBytes((int)header.Length - 8);
                        }
                    }
                    catch (TimeoutException) { }
                }
            }
            finally
            {
                _port.ReadTimeout = oldTimeout;
            }
            return false;
        }

        private bool EnterCommandMode(uint version)
        {
            SendHelloResponse(SaharaMode.Command, version);

            try
            {
                byte[] response = ReadBytes(8);
                SaharaHeader header = RawDeserialize<SaharaHeader>(response);

                if (header.Length > 8)
                    ReadBytes((int)header.Length - 8);

                return header.Command == SaharaCommandId.CmdReady;
            }
            catch 
            {
                return false;
            }
        }

        private void SendHelloResponse(SaharaMode mode, uint version)
        {
            var resp = new SaharaHelloResp
            {
                Header = new SaharaHeader
                {
                    Command = SaharaCommandId.HelloResp,
                    Length = 48
                },
                Version = version,
                VersionSupported = 1,
                Status = 0, // Success
                Mode = mode,
                Reserved0 = 0,
                Reserved1 = 0,
                Reserved2 = 0,
                Reserved3 = 0,
                Reserved4 = 0,
                Reserved5 = 0
            };
            WriteStruct(resp);
        }

        private void SwitchMode(SaharaMode mode)
        {
            var cmd = new SaharaSwitchMode
            {
                Header = new SaharaHeader
                {
                    Command = SaharaCommandId.CmdSwitchMode,
                    Length = (uint)Marshal.SizeOf<SaharaSwitchMode>()
                },
                Mode = mode
            };
            WriteStruct(cmd);
        }

        private void SendReset()
        {
            var reset = new SaharaHeader
            {
                Command = SaharaCommandId.Reset,
                Length = 8
            };
            WriteStruct(reset);
        }

        private byte[] RetryRead(ref int retryCount, int maxRetries)
        {
            while (retryCount < maxRetries)
            {
                byte[] response = PortRead(10);
                if (response.Length > 0)
                {
                    retryCount = 0;
                    return response;
                }
                retryCount++;
            }
            
            _log("[Sahara] 读取超时: 设备无响应");
            return null;
        }

        #endregion

        #region 底层 IO 方法

        private byte[] PortRead(int timeSpan)
        {
            Thread.Sleep(timeSpan);
            int numBytes = _port.BytesToRead;
            byte[] buffer = new byte[numBytes];
            if (numBytes > 0)
                _port.Read(buffer, 0, numBytes);
            return buffer;
        }

        private byte[] ReadBytes(int count)
        {
            byte[] buffer = new byte[count];
            int offset = 0;
            int retries = 0;
            
            while (offset < count)
            {
                int read = _port.Read(buffer, offset, count - offset);
                if (read == 0)
                {
                    if (retries++ > 3)
                        throw new TimeoutException("Sahara 读取超时");
                    Thread.Sleep(50);
                    continue;
                }
                offset += read;
            }
            return buffer;
        }

        private void WriteStruct<T>(T structure) where T : struct
        {
            byte[] data = SerializeStruct(structure);
            _port.Write(data, 0, data.Length);
        }

        private static byte[] SerializeStruct<T>(T structure) where T : struct
        {
            int size = Marshal.SizeOf<T>();
            byte[] buffer = new byte[size];
            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(structure, ptr, true);
                Marshal.Copy(ptr, buffer, 0, size);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
            return buffer;
        }

        private static T RawDeserialize<T>(byte[] data) where T : struct
        {
            int size = Marshal.SizeOf<T>();
            if (data.Length < size)
                return default;

            IntPtr ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.Copy(data, 0, ptr, size);
                return Marshal.PtrToStructure<T>(ptr);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        #endregion

        #region 工具方法

        private static string HexToDecimal(string hex)
        {
            try
            {
                return Convert.ToUInt64(hex, 16).ToString();
            }
            catch
            {
                return hex;
            }
        }

        private static string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1024 * 1024) return $"{bytes / 1024.0:F1} KB";
            return $"{bytes / 1024.0 / 1024.0:F1} MB";
        }

        private static string FormatSpeed(double bytesPerSec)
        {
            if (bytesPerSec < 1024) return $"{bytesPerSec:F1} B/s";
            if (bytesPerSec < 1024 * 1024) return $"{bytesPerSec / 1024:F1} KB/s";
            return $"{bytesPerSec / 1024 / 1024:F1} MB/s";
        }

        #endregion
    }
}
