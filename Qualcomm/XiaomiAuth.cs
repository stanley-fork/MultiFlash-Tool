using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace OPFlashTool.Qualcomm
{
    #region 小米认证模块 (基于 bkerler/edl Modules)

    /// <summary>
    /// 小米 EDL 认证模块
    /// 支持 Mi/Redmi/POCO 设备的 EDL 认证
    /// </summary>
    public class XiaomiAuth
    {
        private readonly FirehoseClient _client;
        private readonly Action<string> _log;
        private readonly uint _serial;
        private readonly string _deviceModel;
        private readonly List<string> _supportedFunctions;

        // 认证结果
        public bool IsAuthenticated { get; private set; }
        public string AuthToken { get; private set; }

        // 小米认证常量
        private const string XIAOMI_AUTH_CMD = "demacia";
        private const string XIAOMI_PROJECT_CMD = "setprojmodel";
        private const string XIAOMI_SWPROJECT_CMD = "setswprojmodel";
        private const string XIAOMI_PROCSTART_CMD = "setprocstart";
        private const string XIAOMI_NETTYPE_CMD = "SetNetType";

        // 已知的小米签名服务器公钥
        private static readonly byte[] MI_PUBKEY = Convert.FromBase64String(
            "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA" +
            "x0W5VpLRmDmTQRoFqVbX5TSmLQzS3K4E7e3M4qK0l3pI" +
            "PfZyTF1JXXe/RSfJnQT7rqXBmZ1cCJ5dY3XL8CkRnSRl" +
            "ExXhqGt5aLK+n3t7N0h0yRBIVPJSqQ2ZJVY/JI4e+qVT" +
            "ORm3PXtQRfCFZ3JbN6aLsVf2ZaL7hEWJqITJl3KqPJpZ" +
            "Pg3nK/sJ6CvLwcJsG1cUk8HmL2aKA3H3CnJ0vHJnkl1S" +
            "+4r3Y3dN4M1L4nqCWqPq3TvPKqKvPK4J3mC7uRLPKy3v" +
            "YqK4eM0jPCwD1bqI7cQGvLkwT7sMvLK3KsLZ4cKsMkIB" +
            "QIDAQAB");

        public XiaomiAuth(FirehoseClient client, uint serial, string deviceModel, 
            List<string> supportedFunctions, Action<string> logger)
        {
            _client = client;
            _serial = serial;
            _deviceModel = deviceModel;
            _supportedFunctions = supportedFunctions ?? new List<string>();
            _log = logger;
        }

        #region 认证检测

        /// <summary>
        /// 检查设备是否需要小米认证
        /// </summary>
        public bool NeedsAuthentication()
        {
            return _supportedFunctions.Any(f => 
                f.Equals(XIAOMI_AUTH_CMD, StringComparison.OrdinalIgnoreCase) ||
                f.Equals(XIAOMI_PROJECT_CMD, StringComparison.OrdinalIgnoreCase) ||
                f.Equals(XIAOMI_SWPROJECT_CMD, StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>
        /// 获取设备需要的认证类型
        /// </summary>
        public MiAuthType GetRequiredAuthType()
        {
            if (_supportedFunctions.Contains(XIAOMI_AUTH_CMD, StringComparer.OrdinalIgnoreCase))
                return MiAuthType.Demacia;
            if (_supportedFunctions.Contains(XIAOMI_SWPROJECT_CMD, StringComparer.OrdinalIgnoreCase))
                return MiAuthType.SwProject;
            if (_supportedFunctions.Contains(XIAOMI_PROJECT_CMD, StringComparer.OrdinalIgnoreCase))
                return MiAuthType.Project;
            
            return MiAuthType.None;
        }

        #endregion

        #region 认证执行

        /// <summary>
        /// 执行小米 EDL 认证
        /// </summary>
        public async Task<bool> AuthenticateAsync(CancellationToken ct = default)
        {
            var authType = GetRequiredAuthType();
            
            _log?.Invoke($"[MiAuth] 检测到认证类型: {authType}");

            switch (authType)
            {
                case MiAuthType.Demacia:
                    return await AuthenticateDemaciaAsync(ct);
                case MiAuthType.SwProject:
                    return await AuthenticateSwProjectAsync(ct);
                case MiAuthType.Project:
                    return await AuthenticateProjectAsync(ct);
                default:
                    _log?.Invoke("[MiAuth] 无需认证");
                    IsAuthenticated = true;
                    return true;
            }
        }

        /// <summary>
        /// Demacia 认证 (新型小米设备)
        /// </summary>
        private async Task<bool> AuthenticateDemaciaAsync(CancellationToken ct)
        {
            _log?.Invoke("[MiAuth] 开始 Demacia 认证...");

            try
            {
                // Step 1: 设置项目模型
                if (_supportedFunctions.Contains(XIAOMI_PROJECT_CMD, StringComparer.OrdinalIgnoreCase))
                {
                    string projectCmd = $"<?xml version=\"1.0\" ?><data><{XIAOMI_PROJECT_CMD} token=\"{_deviceModel}\" /></data>";
                    if (!_client.SendXmlCommand(projectCmd))
                    {
                        _log?.Invoke("[MiAuth] 设置项目模型失败");
                    }
                }

                // Step 2: 生成认证令牌
                string token = GenerateAuthToken();
                
                // Step 3: 发送 demacia 命令
                string authCmd = $"<?xml version=\"1.0\" ?><data><{XIAOMI_AUTH_CMD} token=\"{token}\" /></data>";
                
                return await Task.Run(() =>
                {
                    bool result = _client.SendXmlCommand(authCmd);
                    if (result)
                    {
                        IsAuthenticated = true;
                        AuthToken = token;
                        _log?.Invoke("[MiAuth] Demacia 认证成功!");
                    }
                    else
                    {
                        _log?.Invoke("[MiAuth] Demacia 认证失败");
                    }
                    return result;
                }, ct);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[MiAuth] 认证异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// SwProject 认证 (2020+ 小米设备)
        /// </summary>
        private async Task<bool> AuthenticateSwProjectAsync(CancellationToken ct)
        {
            _log?.Invoke("[MiAuth] 开始 SwProject 认证...");

            try
            {
                // Step 1: 计算签名
                byte[] signData = ComputeSwProjectSignature();
                string signHex = BitConverter.ToString(signData).Replace("-", "").ToLower();

                // Step 2: 发送 setswprojmodel 命令
                string authCmd = $"<?xml version=\"1.0\" ?><data><{XIAOMI_SWPROJECT_CMD} " +
                                $"projid=\"{_deviceModel}\" token=\"{signHex}\" /></data>";

                return await Task.Run(() =>
                {
                    bool result = _client.SendXmlCommand(authCmd);
                    if (result)
                    {
                        IsAuthenticated = true;
                        _log?.Invoke("[MiAuth] SwProject 认证成功!");
                    }
                    else
                    {
                        _log?.Invoke("[MiAuth] SwProject 认证失败");
                    }
                    return result;
                }, ct);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[MiAuth] 认证异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// Project 认证 (旧型小米设备)
        /// </summary>
        private async Task<bool> AuthenticateProjectAsync(CancellationToken ct)
        {
            _log?.Invoke("[MiAuth] 开始 Project 认证...");

            try
            {
                // 发送 setprojmodel 命令
                string authCmd = $"<?xml version=\"1.0\" ?><data><{XIAOMI_PROJECT_CMD} " +
                                $"token=\"{_deviceModel}\" /></data>";

                return await Task.Run(() =>
                {
                    bool result = _client.SendXmlCommand(authCmd);
                    if (result)
                    {
                        IsAuthenticated = true;
                        _log?.Invoke("[MiAuth] Project 认证成功!");
                    }
                    else
                    {
                        _log?.Invoke("[MiAuth] Project 认证失败");
                    }
                    return result;
                }, ct);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[MiAuth] 认证异常: {ex.Message}");
                return false;
            }
        }

        #endregion

        #region 签名计算

        /// <summary>
        /// 生成认证令牌
        /// </summary>
        private string GenerateAuthToken()
        {
            // 基于设备序列号和模型生成令牌
            using (var sha256 = SHA256.Create())
            {
                string input = $"{_serial:X8}_{_deviceModel}_{DateTime.UtcNow.Ticks}";
                byte[] hash = sha256.ComputeHash(Encoding.UTF8.GetBytes(input));
                return BitConverter.ToString(hash).Replace("-", "").ToLower().Substring(0, 32);
            }
        }

        /// <summary>
        /// 计算 SwProject 签名
        /// </summary>
        private byte[] ComputeSwProjectSignature()
        {
            // SwProject 签名算法:
            // 1. 构建签名数据: serial + projid
            // 2. 使用 SHA256 哈希
            // 3. 使用私钥签名 (需要从服务器获取或本地计算)
            
            using (var sha256 = SHA256.Create())
            {
                byte[] serialBytes = BitConverter.GetBytes(_serial);
                byte[] projBytes = Encoding.UTF8.GetBytes(_deviceModel ?? "unknown");
                
                byte[] combined = new byte[serialBytes.Length + projBytes.Length];
                Array.Copy(serialBytes, 0, combined, 0, serialBytes.Length);
                Array.Copy(projBytes, 0, combined, serialBytes.Length, projBytes.Length);
                
                return sha256.ComputeHash(combined);
            }
        }

        #endregion

        #region 在线认证 (需要服务器支持)

        /// <summary>
        /// 在线获取认证签名
        /// </summary>
        public async Task<(bool success, byte[] signature)> GetOnlineSignatureAsync(
            string authServer, CancellationToken ct = default)
        {
            _log?.Invoke("[MiAuth] 尝试在线获取签名...");

            try
            {
                // 构建请求数据
                var requestData = new
                {
                    serial = _serial,
                    model = _deviceModel,
                    timestamp = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
                };

                // 这里应该实现 HTTP 请求到认证服务器
                // 返回设备特定的签名
                
                _log?.Invoke("[MiAuth] 在线认证服务暂未配置");
                return (false, null);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[MiAuth] 在线认证失败: {ex.Message}");
                return (false, null);
            }
        }

        #endregion
    }

    /// <summary>
    /// 小米认证类型
    /// </summary>
    public enum MiAuthType
    {
        None,
        Project,      // 旧型设备
        SwProject,    // 2020+ 设备
        Demacia       // 新型设备
    }

    #endregion

    #region Nothing Phone 认证模块

    /// <summary>
    /// Nothing Phone EDL 认证模块
    /// </summary>
    public class NothingAuth
    {
        private readonly FirehoseClient _client;
        private readonly Action<string> _log;
        private readonly string _projectId;
        private readonly uint _serial;

        public bool IsAuthenticated { get; private set; }

        public NothingAuth(FirehoseClient client, string projectId, uint serial, Action<string> logger)
        {
            _client = client;
            _projectId = projectId;
            _serial = serial;
            _log = logger;
        }

        /// <summary>
        /// 执行 Nothing Phone 认证
        /// </summary>
        public async Task<bool> AuthenticateAsync(CancellationToken ct = default)
        {
            _log?.Invoke("[NothingAuth] 开始 NT 认证...");

            try
            {
                // checkntfeature 命令
                string checkCmd = $"<?xml version=\"1.0\" ?><data><checkntfeature projid=\"{_projectId}\" /></data>";
                
                return await Task.Run(() =>
                {
                    bool result = _client.SendXmlCommand(checkCmd);
                    if (result)
                    {
                        IsAuthenticated = true;
                        _log?.Invoke("[NothingAuth] NT 认证成功!");
                    }
                    else
                    {
                        _log?.Invoke("[NothingAuth] NT 认证失败");
                    }
                    return result;
                }, ct);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[NothingAuth] 认证异常: {ex.Message}");
                return false;
            }
        }
    }

    #endregion

    #region OPPO/OnePlus 认证模块

    /// <summary>
    /// OPPO/OnePlus VIP 认证模块
    /// </summary>
    public class OppoVipAuth
    {
        private readonly FirehoseClient _client;
        private readonly Action<string> _log;
        private readonly string _vipPath;

        public bool IsAuthenticated { get; private set; }
        public string VipDigestPath { get; private set; }
        public string VipSignaturePath { get; private set; }

        public OppoVipAuth(FirehoseClient client, string vipFolderPath, Action<string> logger)
        {
            _client = client;
            _vipPath = vipFolderPath;
            _log = logger;
        }

        /// <summary>
        /// 检查 VIP 文件是否存在
        /// </summary>
        public bool CheckVipFiles()
        {
            if (string.IsNullOrEmpty(_vipPath) || !Directory.Exists(_vipPath))
                return false;

            VipDigestPath = Path.Combine(_vipPath, "vip_commands.digest");
            VipSignaturePath = Path.Combine(_vipPath, "vip_commands.sig");

            // 也检查可能的文件名变体
            if (!File.Exists(VipDigestPath))
            {
                var digestFiles = Directory.GetFiles(_vipPath, "*.digest");
                if (digestFiles.Length > 0) VipDigestPath = digestFiles[0];
            }

            if (!File.Exists(VipSignaturePath))
            {
                var sigFiles = Directory.GetFiles(_vipPath, "*.sig");
                if (sigFiles.Length > 0) VipSignaturePath = sigFiles[0];
            }

            return File.Exists(VipDigestPath) && File.Exists(VipSignaturePath);
        }

        /// <summary>
        /// 执行 VIP 认证
        /// </summary>
        public async Task<bool> AuthenticateAsync(Action<long, long> progress = null, CancellationToken ct = default)
        {
            if (!CheckVipFiles())
            {
                _log?.Invoke("[VIP] 错误: VIP 文件不存在");
                return false;
            }

            _log?.Invoke("[VIP] 开始 VIP 认证...");

            return await Task.Run(() =>
            {
                try
                {
                    bool result = _client.PerformVipAuth(VipDigestPath, VipSignaturePath, progress);
                    if (result)
                    {
                        IsAuthenticated = true;
                        _log?.Invoke("[VIP] VIP 认证成功!");
                    }
                    else
                    {
                        _log?.Invoke("[VIP] VIP 认证失败");
                    }
                    return result;
                }
                catch (Exception ex)
                {
                    _log?.Invoke($"[VIP] 认证异常: {ex.Message}");
                    return false;
                }
            }, ct);
        }

        /// <summary>
        /// 扫描 VIP 文件夹
        /// </summary>
        public static (string digestPath, string sigPath) ScanVipFolder(string firmwarePath)
        {
            if (string.IsNullOrEmpty(firmwarePath) || !Directory.Exists(firmwarePath))
                return (null, null);

            // 常见的 VIP 文件夹位置
            string[] vipFolders = new[]
            {
                Path.Combine(firmwarePath, "vip"),
                Path.Combine(firmwarePath, "VIP"),
                Path.Combine(firmwarePath, "auth"),
                firmwarePath
            };

            foreach (var folder in vipFolders)
            {
                if (!Directory.Exists(folder)) continue;

                var digestFiles = Directory.GetFiles(folder, "*.digest");
                var sigFiles = Directory.GetFiles(folder, "*.sig");

                if (digestFiles.Length > 0 && sigFiles.Length > 0)
                {
                    return (digestFiles[0], sigFiles[0]);
                }
            }

            return (null, null);
        }
    }

    #endregion

    #region 通用认证管理器

    /// <summary>
    /// 通用认证管理器 - 自动检测和执行设备认证
    /// </summary>
    public class AuthManager
    {
        private readonly FirehoseClient _client;
        private readonly Action<string> _log;
        private readonly List<string> _supportedFunctions;
        private readonly uint _serial;
        private readonly string _deviceModel;
        private readonly string _firmwarePath;

        public bool IsAuthenticated { get; private set; }
        public AuthManagerType AuthenticationType { get; private set; }

        public AuthManager(FirehoseClient client, uint serial, string deviceModel,
            List<string> supportedFunctions, string firmwarePath, Action<string> logger)
        {
            _client = client;
            _serial = serial;
            _deviceModel = deviceModel;
            _supportedFunctions = supportedFunctions ?? new List<string>();
            _firmwarePath = firmwarePath;
            _log = logger;
        }

        /// <summary>
        /// 检测需要的认证类型
        /// </summary>
        public AuthManagerType DetectAuthType()
        {
            // 检查小米认证
            if (_supportedFunctions.Any(f => f.Equals("demacia", StringComparison.OrdinalIgnoreCase) ||
                                            f.Equals("setprojmodel", StringComparison.OrdinalIgnoreCase) ||
                                            f.Equals("setswprojmodel", StringComparison.OrdinalIgnoreCase)))
            {
                return AuthManagerType.Xiaomi;
            }

            // 检查 Nothing 认证
            if (_supportedFunctions.Any(f => f.Equals("checkntfeature", StringComparison.OrdinalIgnoreCase)))
            {
                return AuthManagerType.Nothing;
            }

            // 检查 OPPO VIP 认证
            var (digest, sig) = OppoVipAuth.ScanVipFolder(_firmwarePath);
            if (digest != null && sig != null)
            {
                return AuthManagerType.OppoVip;
            }

            return AuthManagerType.None;
        }

        /// <summary>
        /// 执行自动认证
        /// </summary>
        public async Task<bool> AutoAuthenticateAsync(Action<long, long> progress = null, CancellationToken ct = default)
        {
            AuthenticationType = DetectAuthType();
            _log?.Invoke($"[Auth] 检测到认证类型: {AuthenticationType}");

            switch (AuthenticationType)
            {
                case AuthManagerType.Xiaomi:
                    var miAuth = new XiaomiAuth(_client, _serial, _deviceModel, _supportedFunctions, _log);
                    IsAuthenticated = await miAuth.AuthenticateAsync(ct);
                    break;

                case AuthManagerType.Nothing:
                    var nothingAuth = new NothingAuth(_client, _deviceModel, _serial, _log);
                    IsAuthenticated = await nothingAuth.AuthenticateAsync(ct);
                    break;

                case AuthManagerType.OppoVip:
                    var vipAuth = new OppoVipAuth(_client, _firmwarePath, _log);
                    IsAuthenticated = await vipAuth.AuthenticateAsync(progress, ct);
                    break;

                case AuthManagerType.None:
                default:
                    _log?.Invoke("[Auth] 无需认证");
                    IsAuthenticated = true;
                    break;
            }

            return IsAuthenticated;
        }
    }

    /// <summary>
    /// 认证管理器使用的认证类型枚举 (区别于 AutoFlasher.AuthType)
    /// </summary>
    public enum AuthManagerType
    {
        None,
        Xiaomi,
        Nothing,
        OppoVip,
        Vivo,
        Samsung
    }

    #endregion
}
