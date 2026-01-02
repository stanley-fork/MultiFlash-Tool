using System;
using System.IO;
using System.IO.Ports;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using OPFlashTool.Authentication;
using OPFlashTool.Strategies;
using OPFlashTool.Services;

namespace OPFlashTool.Qualcomm
{
    // 定义认证类型枚举，方便 UI 传参
    public enum AuthType
    {
        Standard, // 标准 (无验证)
        Vip,      // Oppo/Realme VIP
        Xiaomi    // 小米免授权
    }

    public class AutoFlasher
    {
        private Action<string> _log;
        private string _baseDir;    // bin 目录
        private string _loaderDir;  // bin/Loaders 目录

        public AutoFlasher(string binDirectory, Action<string> logCallback)
        {
            _baseDir = binDirectory;
            _loaderDir = Path.Combine(binDirectory, "Loaders");
            
            // 自动创建目录，方便用户放文件
            if (!Directory.Exists(_loaderDir)) Directory.CreateDirectory(_loaderDir);
            
            _log = logCallback;
        }

        // 工厂方法：根据枚举获取策略
        private IDeviceStrategy GetDeviceStrategy(AuthType type)
        {
            switch (type)
            {
                case AuthType.Vip: return new OppoVipDeviceStrategy();
                case AuthType.Xiaomi: return new XiaomiDeviceStrategy();
                case AuthType.Standard: 
                default: return new StandardDeviceStrategy();
            }
        }

        /// <summary>
        /// 智能刷机主流程 (带回调)
        /// </summary>
        public async Task<bool> RunFlashActionAsync(
            string portName, 
            string userProgPath, 
            AuthType authType, 
            bool skipLoader,
            string userDigestPath, 
            string userSignPath,
            Func<FlashTaskExecutor, Task> flashAction,
            CancellationToken ct = default,
            Func<string, string> inputRequestCallback = null,
            string preferredStorageType = "Auto",
            ProgressContext progressContext = null)
        {
            // 1. 获取策略对象
            IDeviceStrategy strategy = GetDeviceStrategy(authType);

            if (string.IsNullOrEmpty(portName))
            {
                _log("错误: 端口名不能为空");
                return false;
            }

            return await Task.Run(async () =>
            {
                SerialPort port = null;
                CancellationTokenRegistration ctr = default;

                try
                {
                    if (ct.IsCancellationRequested) throw new OperationCanceledException();

                    // [优化] 显式创建，便于在 finally 中控制
                    port = new SerialPort(portName, 115200);
                    port.ReadTimeout = 5000;
                    port.WriteTimeout = 5000;
                    
                    // [热插拔优化] 重试打开端口，处理突然断开后的重连
                    int openRetries = 3;
                    while (openRetries-- > 0)
                    {
                        try 
                        {
                            port.Open();
                            break; // 成功打开
                        }
                        catch (UnauthorizedAccessException) when (openRetries > 0)
                        {
                            // 端口被占用，可能是之前的连接未完全释放
                            _log($"[重试] 端口 {portName} 被占用，等待释放... ({3 - openRetries}/3)");
                            
                            // 强制 GC 回收可能残留的 SerialPort 对象
                            GC.Collect();
                            GC.WaitForPendingFinalizers();
                            
                            Thread.Sleep(1000); // 等待系统释放端口
                        }
                        catch (Exception ex)
                        {
                            _log($"[错误] 无法打开端口 {portName}: {ex.Message}");
                            return false;
                        }
                    }
                    
                    if (!port.IsOpen)
                    {
                        _log($"[错误] 重试后仍无法打开端口 {portName}");
                        return false;
                    }

                    _log($"[连接] 打开端口 {portName} | 认证模式: {strategy.Name}");

                    // [新增] 注册取消回调，强制关闭端口以中断阻塞操作
                    ctr = ct.Register(() => 
                    {
                        try 
                        { 
                            if (port != null && port.IsOpen) 
                            {
                                _log("[停止] 用户强制停止，正在关闭端口...");
                                port.Close(); 
                            }
                        } 
                        catch {}
                    });

                    if (ct.IsCancellationRequested) throw new OperationCanceledException();

                    uint? detectedHwId = null;

                    // --- START 核心逻辑 ---
                    // 1. Sahara 智能引导 (改进版)
                    if (!skipLoader)
                    {
                        var sahara = new SaharaClient(port, _log);
                        
                        // 使用智能握手流程
                        var handshakeResult = await sahara.SmartHandshakeAsync(
                            userSelectedLoader: userProgPath,  // 用户选择的 Loader (可为空)
                            loaderDirectory: _loaderDir,       // 自动搜索目录
                            enableCloudLookup: false           // 暂不启用云端查询
                        );

                        // 保存设备信息用于后续操作
                        if (handshakeResult.DeviceInfoRead && handshakeResult.PblInfo != null)
                        {
                            if (!string.IsNullOrEmpty(handshakeResult.PblInfo.MsmId))
                            {
                                try
                                {
                                    detectedHwId = Convert.ToUInt32(handshakeResult.PblInfo.MsmId, 16);
                                }
                                catch { }
                            }
                        }

                        // 检查握手结果
                        if (!handshakeResult.Success)
                        {
                            // 需要用户操作
                            if (handshakeResult.RequiresUserAction)
                            {
                                _log("");
                                _log("═══════════════════════════════════════════");
                                _log("  ⚠️ 需要手动选择 Loader");
                                _log("═══════════════════════════════════════════");
                                _log(handshakeResult.UserGuidance);
                                _log("");
                                _log("[提示] 请选择正确的 Loader 后重新连接设备");
                                
                                // 返回 false 但不视为错误，只是需要用户操作
                                return false;
                            }
                            
                            // 真正的错误
                            _log($"[失败] Sahara 引导失败: {handshakeResult.ErrorMessage}");
                            return false;
                        }
                        
                        _log("[等待] Firehose 正在启动...");
                        Thread.Sleep(1500);
                    }
                    else
                    {
                        _log("[引导] 跳过引导阶段 (假设设备已在 Firehose 模式)");
                    }

                    // 2. 创建 Client
                    var firehose = new FirehoseClient(port, _log);

                    // 3. 策略认证
                    bool authResult = true;

                    // [根本性修复] 如果跳过了 Loader，说明我们正在复用一个已经建立的会话。
                    // 此时设备通常已经处于 Authenticated 状态。
                    // 如果再次发送认证指令（如小米的 <sig>），设备会报错 Failed to run the last command。
                    // 因此，当 skipLoader=true 时，我们应当跳过认证步骤。
                    if (!skipLoader)
                    {
                        // ============== 新增：标准模式自动检测认证 ==============
                        // 如果用户选择的是标准模式，先检测设备是否需要认证
                        if (authType == AuthType.Standard)
                        {
                            var autoDetectedStrategy = await AutoDetectAuthStrategyAsync(firehose, userDigestPath, userSignPath, ct);
                            if (autoDetectedStrategy != null)
                            {
                                _log($"[自动检测] 检测到设备需要 {autoDetectedStrategy.Name} 认证");
                                strategy = autoDetectedStrategy;
                            }
                        }
                        // ============== 自动检测结束 ==============

                        authResult = await strategy.AuthenticateAsync(
                            firehose, 
                            userProgPath ?? "", 
                            _log, 
                            inputRequestCallback, 
                            userDigestPath, 
                            userSignPath
                        );
                    }
                    else
                    {
                        _log("[认证] 复用会话模式：跳过重复认证步骤");
                    }

                    if (!authResult)
                    {
                        _log($"[错误] {strategy.Name} 认证失败！中止操作。");
                        return false;
                    }

                    // 4. 配置 UFS/EMMC (智能配置)
                    string storageType = "ufs"; // 默认 UFS
                    bool isAuto = string.Equals(preferredStorageType, "Auto", StringComparison.OrdinalIgnoreCase);

                    if (!isAuto && !string.IsNullOrEmpty(preferredStorageType))
                    {
                        storageType = preferredStorageType.ToLower();
                        _log($"[配置] 强制使用存储类型: {storageType.ToUpper()}");
                    }
                    else if (detectedHwId != null)
                    {
                        // 自动识别逻辑
                        string chipNameForConfig = QualcommDatabase.GetChipName(detectedHwId.Value);
                        var memType = QualcommDatabase.GetMemoryType(chipNameForConfig);
                        
                        storageType = (memType == MemoryType.Emmc) ? "emmc" : "ufs";
                        _log($"[智能] 芯片 {chipNameForConfig} -> 推荐配置: {storageType.ToUpper()}");
                    }

                    // 针对高端芯片的特殊优化：强制 UFS，不重试 EMMC (仅在自动模式下)
                    bool isHighEndChip = isAuto && detectedHwId != null && 
                                        (QualcommDatabase.GetChipName(detectedHwId.Value).StartsWith("SM8") || 
                                         QualcommDatabase.GetChipName(detectedHwId.Value).Contains("Snapdragon 8"));

                    if (isHighEndChip)
                    {
                        // 高端旗舰芯片，强制 UFS
                        if (!firehose.Configure("ufs")) 
                        {
                            _log($"[错误] UFS 配置失败 (芯片不支持 EMMC 降级重试)");
                            return false;
                        }
                    }
                    else
                    {
                        // 普通芯片或强制模式
                        if (!firehose.Configure(storageType))
                        {
                            if (isAuto)
                            {
                                _log($"[重试] 配置 {storageType} 失败，尝试切换模式...");
                                string retryType = (storageType == "ufs") ? "emmc" : "ufs";
                                if (!firehose.Configure(retryType))
                                {
                                    _log("[失败] 存储配置完全失败。");
                                    return false;
                                }
                                storageType = retryType;
                            }
                            else
                            {
                                _log($"[失败] 强制配置 {storageType} 失败。");
                                return false;
                            }
                        }
                    }

                    _log($"[就绪] 设备连接成功 ({storageType.ToUpper()})");

                    // 5. 执行任务
                    if (flashAction != null)
                    {
                        // 创建执行器，传入进度上下文
                        var executor = new FlashTaskExecutor(firehose, strategy, _log, firehose.SectorSize, progressContext);

                        // 执行具体的刷写逻辑
                        await flashAction(executor);
                    }
                    // --- END 核心逻辑 ---

                    return true;
                }
                catch (OperationCanceledException)
                {
                    _log("[信息] 操作已取消，正在断开连接...");
                    return false;
                }
                catch (Exception ex)
                {
                    if (ct.IsCancellationRequested)
                    {
                        _log("[信息] 操作已强制终止");
                        return false;
                    }
                    _log($"[异常] {ex.Message}");
                    return false;
                }
                finally
                {
                    ctr.Dispose();

                    // [热插拔优化] 强制彻底释放端口资源
                    ForceReleasePort(ref port, portName, _log);
                }
            }, ct);
        }

        /// <summary>
        /// 自动检测设备是否需要认证，并返回合适的认证策略
        /// 如果不需要认证或检测失败，返回 null (继续使用标准模式)
        /// </summary>
        private async Task<IDeviceStrategy> AutoDetectAuthStrategyAsync(
            FirehoseClient firehose,
            string userDigestPath,
            string userSignPath,
            CancellationToken ct)
        {
            try
            {
                _log("[自动检测] 正在检测设备认证需求...");
                
                // 创建增强 Firehose 客户端用于功能检测
                var enhanced = new FirehoseEnhanced(firehose, _log);
                
                // 1. 尝试获取设备支持的功能列表
                _log("[自动检测] 发送 NOP 命令检测支持的功能...");
                var supportedFunctions = await enhanced.GetSupportedFunctionsAsync();
                
                if (supportedFunctions == null || supportedFunctions.Count == 0)
                {
                    _log("[自动检测] 无法获取功能列表，尝试直接配置测试...");
                    
                    // 2. 尝试直接配置，如果失败则可能需要认证
                    bool configOk = await TryConfigureWithoutAuthAsync(firehose);
                    if (configOk)
                    {
                        _log("[自动检测] 配置成功，设备不需要认证");
                        return null;
                    }
                    
                    _log("[自动检测] 配置失败，尝试检测认证类型...");
                }
                else
                {
                    _log($"[自动检测] 检测到 {supportedFunctions.Count} 个支持的功能");
                    foreach (var func in supportedFunctions)
                    {
                        _log($"  - {func}");
                    }
                }

                // 3. 根据功能列表检测认证类型
                // 检测小米认证
                bool hasDemacia = supportedFunctions?.Contains("demacia") == true;
                bool hasSetProjModel = supportedFunctions?.Contains("setprojmodel") == true;
                bool hasSetSwProjModel = supportedFunctions?.Contains("setswprojmodel") == true;
                bool hasGetToken = supportedFunctions?.Contains("gettoken") == true;
                
                // 检测 Nothing 认证
                bool hasNtFeature = supportedFunctions?.Contains("checkntfeature") == true;
                
                // 检测小米设备 (任一小米特有功能)
                if (hasDemacia || hasSetProjModel || hasSetSwProjModel || hasGetToken)
                {
                    _log("[自动检测] ★ 检测到小米/红米设备特征");
                    if (hasDemacia) _log("  - demacia: 需要 Mi-Demacia 认证");
                    if (hasSetProjModel) _log("  - setprojmodel: 需要 Project 认证");
                    if (hasSetSwProjModel) _log("  - setswprojmodel: 需要 SW-Project 认证");
                    
                    return new XiaomiDeviceStrategy();
                }
                
                // 检测 Nothing Phone
                if (hasNtFeature)
                {
                    _log("[自动检测] ★ 检测到 Nothing Phone 设备特征");
                    _log("  - checkntfeature: 需要 Nothing 认证");
                    
                    // Nothing 认证暂时由 XiaomiDeviceStrategy 处理
                    // TODO: 未来可以创建独立的 NothingDeviceStrategy
                    return new XiaomiDeviceStrategy();
                }
                
                // 注意: OPPO VIP 模式不进行自动检测
                // 必须由用户手动选择 VIP 模式才会启用
                // 这是为了避免误触发 VIP 认证流程
                
                // 4. 无需认证
                _log("[自动检测] 未检测到特殊认证需求，使用标准模式");
                return null;
            }
            catch (Exception ex)
            {
                _log($"[自动检测] 检测过程出错: {ex.Message}");
                _log("[自动检测] 回退到标准模式");
                return null;
            }
        }

        /// <summary>
        /// 尝试不经认证直接配置 Firehose
        /// </summary>
        private async Task<bool> TryConfigureWithoutAuthAsync(FirehoseClient firehose)
        {
            try
            {
                // 尝试 UFS 配置
                _log("[自动检测] 尝试 UFS 配置...");
                bool ufsOk = firehose.Configure("ufs");
                if (ufsOk && firehose.IsConfigured)
                {
                    _log("[自动检测] UFS 配置成功");
                    return true;
                }
                
                // 尝试 eMMC 配置
                _log("[自动检测] 尝试 eMMC 配置...");
                bool emmcOk = firehose.Configure("emmc");
                if (emmcOk && firehose.IsConfigured)
                {
                    _log("[自动检测] eMMC 配置成功");
                    return true;
                }
                
                // 都失败了，可能需要认证
                _log("[自动检测] 配置失败，设备可能需要认证");
                return false;
            }
            catch (Exception ex)
            {
                _log($"[自动检测] 配置尝试异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 强制释放端口资源
        /// </summary>
        private void ForceReleasePort(ref SerialPort port, string portName, Action<string> logCallback)
        {
            if (port != null)
            {
                if (port.IsOpen)
                {
                    try 
                    {
                        // 尝试清理缓冲区，虽然可能抛异常
                        port.DiscardInBuffer();
                        port.DiscardOutBuffer();
                    } catch {}

                    try
                    {
                        port.Close(); // 关闭连接
                    }
                    catch (Exception ex)
                    {
                        logCallback($"[警告] 关闭端口时出错: {ex.Message}");
                    }
                }
                
                port.Dispose(); // 释放资源
                port = null;    // 解除引用
                
                logCallback($"[断开] 端口 {portName} 已释放");
            }
        }

        /// <summary>
        /// 智能刷机主流程 (旧版兼容)
        /// </summary>
        public async Task<bool> RunAutoProcess(
            string portName, 
            string userProgPath, 
            bool enableVip, 
            string userDigestPath, 
            string userSignPath)
        {
            AuthType auth = enableVip ? AuthType.Vip : AuthType.Standard;
            return await RunFlashActionAsync(portName, userProgPath, auth, false, userDigestPath, userSignPath, null);
        }
    }
}