// ============================================================================
// SakuraEDL - Qualcomm Service | 高通服务
// ============================================================================
// [ZH] 高通刷写服务 - 整合 Sahara 和 Firehose 协议的高层 API
// [EN] Qualcomm Flash Service - High-level API integrating Sahara and Firehose
// [JA] Qualcommフラッシュサービス - SaharaとFirehoseを統合した高レベルAPI
// [KO] Qualcomm 플래싱 서비스 - Sahara와 Firehose를 통합한 고수준 API
// [RU] Сервис прошивки Qualcomm - Высокоуровневый API для Sahara и Firehose
// [ES] Servicio de flasheo Qualcomm - API de alto nivel para Sahara y Firehose
// ============================================================================
// Features: Device connection, partition R/W, flash workflow management
// Copyright (c) 2025-2026 SakuraEDL | Licensed under CC BY-NC-SA 4.0
// ============================================================================

using System;
using System.Collections.Generic;
using System.Configuration;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Text.Json;
using System.Text.Json.Nodes;
using SakuraEDL.Common;
using SakuraEDL.Qualcomm.Common;
using SakuraEDL.Qualcomm.Database;
using SakuraEDL.Qualcomm.Models;
using SakuraEDL.Qualcomm.Protocol;
using SakuraEDL.Qualcomm.Authentication;
// 已合并到 SakuraEDL.Qualcomm.Common 和 SakuraEDL.Qualcomm.Protocol

namespace SakuraEDL.Qualcomm.Services
{
    /// <summary>
    /// 连接状态
    /// </summary>
    public enum QualcommConnectionState
    {
        Disconnected,
        Connecting,
        SaharaMode,
        FirehoseMode,
        Ready,
        Error
    }

    /// <summary>
    /// 高通刷写服务
    /// </summary>
    public class QualcommService : IDisposable
    {
#if !EXCLUDE_REALME_AUTH
        private sealed class RealmeDigestCandidate
        {
            public string DisplayName { get; set; }
            public string SourcePath { get; set; }
            public byte[] Data { get; set; }
            public string Sha256 { get; set; }
        }
#endif

        private SerialPortManager _portManager;
        private SaharaClient _sahara;
        private FirehoseClient _firehose;
        private readonly Action<string> _log;
        private readonly Action<string> _logDetail;  // 详细调试日志 (只写入文件)
        private readonly Action<long, long> _progress;
        private readonly OplusSuperFlashManager _oplusSuperManager;
#if !EXCLUDE_REALME_AUTH
        private readonly RealmeSignService _realmeSignService;
#endif
        private readonly DeviceInfoService _deviceInfoService;
        private bool _disposed;
#if !EXCLUDE_REALME_AUTH
        private byte[] _realmeElfHashSegment;
#endif
        
        // 看门狗机制
        private Watchdog _watchdog;

        // 状态
        public QualcommConnectionState State { get; private set; }
        public QualcommChipInfo ChipInfo { get { return _sahara != null ? _sahara.ChipInfo : null; } }
        public uint SaharaProtocolVersion { get { return _sahara != null ? _sahara.ProtocolVersion : 0; } }
        public bool IsVipDevice { get; private set; }
#if EXCLUDE_REALME_AUTH
        public bool IsRealmeAuthenticated { get { return false; } }
#else
        public bool IsRealmeAuthenticated { get; private set; }
#endif
        public string StorageType { get { return _firehose != null ? _firehose.StorageType : "ufs"; } }
        public int SectorSize { get { return _firehose != null ? _firehose.SectorSize : 4096; } }
        public string CurrentSlot { get { return _firehose != null ? _firehose.CurrentSlot : "nonexistent"; } }
        
        // 最后使用的连接参数 (用于状态显示)
        public string LastPortName { get; private set; }
        public string LastStorageType { get; private set; }

        // 分区缓存
        private Dictionary<int, List<PartitionInfo>> _partitionCache;
#if !EXCLUDE_REALME_AUTH
        private QualcommRealmeAuthOptions _realmeAuthOptions;
#endif
        
        // 新增: Diag 客户端、Loader 检测器、Motorola 支持
        private DiagClient _diagClient;
        private LoaderFeatureDetector _loaderDetector;
        private MotorolaSupport _motorolaSupport;
        private LoaderFeatures _loaderFeatures;

        /// <summary>
        /// 状态变化事件
        /// </summary>
        public event EventHandler<QualcommConnectionState> StateChanged;
        
        /// <summary>
        /// 端口断开事件 (设备自己断开时触发)
        /// </summary>
        public event EventHandler PortDisconnected;
        
        /// <summary>
        /// 小米授权令牌事件 (内置签名失败时触发，需要弹窗显示令牌)
        /// Token 格式: VQ 开头的 Base64 字符串
        /// </summary>
        public event Action<string> XiaomiAuthTokenRequired;
        
        /// <summary>
        /// 检查是否真正连接 (会验证端口状态)
        /// </summary>
        public bool IsConnected 
        { 
            get 
            { 
                if (State != QualcommConnectionState.Ready)
                    return false;
                    
                // 验证端口是否真正可用
                if (_portManager == null || !_portManager.ValidateConnection())
                {
                    // 端口已断开，更新状态
                    HandlePortDisconnected();
                    return false;
                }
                return true;
            } 
        }
        
        /// <summary>
        /// 快速检查连接状态 (不验证端口，用于UI高频显示)
        /// </summary>
        public bool IsConnectedFast
        {
            get { return State == QualcommConnectionState.Ready && _portManager != null && _portManager.IsOpen; }
        }
        
        /// <summary>
        /// 验证连接是否有效
        /// </summary>
        public bool ValidateConnection()
        {
            if (State != QualcommConnectionState.Ready)
                return false;
                
            if (_portManager == null)
                return false;
                
            // 检查端口是否在系统中
            if (!_portManager.IsPortAvailable())
            {
                _logDetail("[高通] 端口已从系统中移除");
                HandlePortDisconnected();
                return false;
            }
            
            // 验证端口连接
            if (!_portManager.ValidateConnection())
            {
                _logDetail("[高通] 端口连接验证失败");
                HandlePortDisconnected();
                return false;
            }
            
            return true;
        }
        
        /// <summary>
        /// 处理端口断开 (设备自己断开)
        /// </summary>
        private void ResetAuthState()
        {
            IsVipDevice = false;
#if !EXCLUDE_REALME_AUTH
            IsRealmeAuthenticated = false;
#endif
        }

        private void SetVipAuthState(bool enabled)
        {
            IsVipDevice = enabled;
#if !EXCLUDE_REALME_AUTH
            if (enabled)
                IsRealmeAuthenticated = false;
#endif
        }

#if !EXCLUDE_REALME_AUTH
        private void SetRealmeAuthState(bool enabled)
        {
            IsRealmeAuthenticated = enabled;
            if (enabled)
            {
                IsVipDevice = false;
                if (_firehose != null)
                    _firehose.IsRealmeMode = true;
            }
        }
#endif

        private void HandlePortDisconnected()
        {
            if (State == QualcommConnectionState.Disconnected)
                return;
                
            _log("[高通] 检测到设备断开");
            
            // 先释放依赖端口的客户端，再关闭端口，确保端口句柄被系统回收
            if (_sahara != null)
            {
                try { _sahara.Dispose(); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[QualcommService] 释放 Sahara 异常: {ex.Message}"); }
                _sahara = null;
            }
            
            if (_firehose != null)
            {
                try { _firehose.Dispose(); } 
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[QualcommService] 释放 Firehose 异常: {ex.Message}"); }
                _firehose = null;
            }
            
            if (_portManager != null)
            {
                try { _portManager.Close(); } 
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[QualcommService] 关闭端口异常: {ex.Message}"); }
                try { _portManager.Dispose(); } 
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[QualcommService] 释放端口异常: {ex.Message}"); }
                _portManager = null;
            }
            
            // 清空分区缓存 (设备断开后缓存无效)
            _partitionCache.Clear();
            ResetAuthState();
            
            SetState(QualcommConnectionState.Disconnected);
            PortDisconnected?.Invoke(this, EventArgs.Empty);
        }

        public QualcommService(Action<string> log = null, Action<long, long> progress = null, Action<string> logDetail = null)
        {
            _log = log != null ? (msg => log(LogTranslator.Translate(msg))) : (Action<string>)delegate { };
            _logDetail = logDetail ?? delegate { };
            _progress = progress;
            _oplusSuperManager = new OplusSuperFlashManager(_log);
#if !EXCLUDE_REALME_AUTH
            _realmeSignService = new RealmeSignService(_log, _logDetail);
#endif
            _deviceInfoService = new DeviceInfoService(_log, _logDetail);
            _partitionCache = new Dictionary<int, List<PartitionInfo>>();
            State = QualcommConnectionState.Disconnected;
            
            // 初始化看门狗
            _watchdog = new Watchdog("Qualcomm", WatchdogManager.DefaultTimeouts.Qualcomm, _logDetail);
            _watchdog.OnTimeout += OnWatchdogTimeout;
        }
        
        /// <summary>
        /// 看门狗超时处理
        /// </summary>
        private void OnWatchdogTimeout(object sender, WatchdogTimeoutEventArgs e)
        {
            _log($"[高通] 看门狗超时: {e.OperationName} (等待 {e.ElapsedTime.TotalSeconds:F1}秒)");
            
            // 超时次数过多时尝试重置
            if (e.TimeoutCount >= 3)
            {
                _log("[高通] 多次超时，尝试重置连接...");
                e.ShouldReset = false; // 停止看门狗
                
                // 触发端口断开事件
                HandlePortDisconnected();
            }
        }
        
        /// <summary>
        /// 喂狗 - 在长时间操作中调用以重置看门狗计时器
        /// </summary>
        public void FeedWatchdog()
        {
            _watchdog?.Feed();
        }
        
        /// <summary>
        /// 启动看门狗
        /// </summary>
        public void StartWatchdog(string operation)
        {
            _watchdog?.Start(operation);
        }
        
        /// <summary>
        /// 停止看门狗
        /// </summary>
        public void StopWatchdog()
        {
            _watchdog?.Stop();
        }

        public void ConfigureRealmeAuth(QualcommRealmeAuthOptions options)
        {
#if !EXCLUDE_REALME_AUTH
            _realmeAuthOptions = options != null ? options.Clone() : null;
#endif
        }

#if !EXCLUDE_REALME_AUTH
        private QualcommRealmeAuthOptions GetRealmeAuthOptions()
        {
            return _realmeAuthOptions != null ? _realmeAuthOptions.Clone() : new QualcommRealmeAuthOptions();
        }
#endif

        private static bool ShouldDiscardFirehoseBufferOnOpen(string authMode)
        {
#if EXCLUDE_REALME_AUTH
            return true;
#else
            return !string.Equals(authMode ?? "none", "realme", StringComparison.OrdinalIgnoreCase);
#endif
        }

#if !EXCLUDE_REALME_AUTH
        private async Task PrimeRealmeFirehoseSessionAsync(CancellationToken ct)
        {
            if (_firehose == null)
                return;

            await _firehose.PrepareForRealmeHandshakeAsync(ct).ConfigureAwait(false);
            _logDetail("[Realme] 进入 Firehose 后先发送 nop...");
            bool pingOk = await _firehose.PingAsync(false, ct).ConfigureAwait(false);
            if (pingOk)
                _logDetail("[Realme] Firehose nop 成功");
            else
                _logDetail("[Realme] Firehose nop 无回执，继续尝试 configure");
        }

        private async Task<bool> SendRealmePingAsync(CancellationToken ct)
        {
            if (_firehose == null)
                return false;

            _logDetail("[Realme] 进入 Firehose 后先发送 nop...");
            bool pingOk = await _firehose.PingAsync(false, ct).ConfigureAwait(false);
            if (pingOk)
                _logDetail("[Realme] Firehose nop 成功");
            else
                _logDetail("[Realme] Firehose nop 无回执，继续尝试 configure");
            return pingOk;
        }

        /// <summary>
        /// Modern Realme (SM8650+): Sahara Done 后、进入 Firehose XML 模式前，
        /// 将 DigestsToSign 原始二进制直接写入 USB OUT 端点。
        /// 官方工具在此阶段发送 ~33KB 的 DigestsToSign.bin.mbn，设备需要它
        /// 来完成 VIP 分区信息初始化，否则后续 nop 会收到 VIP auth failed。
        /// </summary>
        private async Task SendModernRealmeDigestAfterSaharaAsync(string digestPath, CancellationToken ct)
        {
            if (_portManager == null || !_portManager.IsOpen)
            {
                _log("[Realme] Modern: 端口未就绪，跳过 DigestsToSign 发送");
                return;
            }

            byte[] digestData = null;
            string digestLabel = null;

            List<RealmeDigestCandidate> candidates = LoadRealmeDigestCandidates(digestPath);
            if (candidates.Count > 0 && candidates[0].Data != null && candidates[0].Data.Length > 0)
            {
                digestData = candidates[0].Data;
                digestLabel = candidates[0].DisplayName;
            }

            if (digestData == null || digestData.Length == 0)
            {
                _log("[Realme] Modern: 未找到 DigestsToSign 文件，跳过 Sahara 后原始二进制发送");
                _log("[Realme] Modern: VIP 认证可能因缺少分区信息而失败");
                return;
            }

            _log(string.Format("[Realme] Modern: Sahara Done 后发送 DigestsToSign ({0} 字节): {1}", digestData.Length, digestLabel));

            try
            {
                bool sent = await _portManager.WriteAsync(digestData, 0, digestData.Length, ct);
                if (sent)
                    _log("[Realme] Modern: DigestsToSign 原始二进制已发送");
                else
                    _log("[Realme] Modern: DigestsToSign 发送失败");
            }
            catch (Exception ex)
            {
                _log(string.Format("[Realme] Modern: DigestsToSign 发送异常: {0}", ex.Message));
            }
        }
#endif

        private async Task<bool> ConfigureFirehoseForAuthModeAsync(string storageType, string authMode, CancellationToken ct)
        {
            if (_firehose == null)
                return false;

            string authModeLower = (authMode ?? "none").ToLowerInvariant();
#if !EXCLUDE_REALME_AUTH
            if (authModeLower == "realme")
            {
                await PrimeRealmeFirehoseSessionAsync(ct).ConfigureAwait(false);
                return await _firehose.ConfigureRealmeAsync(storageType, false, ct).ConfigureAwait(false);
            }
#endif
            return await _firehose.ConfigureAsync(storageType, 0, ct).ConfigureAwait(false);
        }

#if !EXCLUDE_REALME_AUTH
        private static string ResolveRealmeProjectNumber(QualcommRealmeAuthOptions options)
        {
            return NormalizeRealmeProjectNumber(FirstNonEmpty(
                options != null ? options.ProjectNumber : null,
                GetRealmeConfigValue("RealmeProjectNumber"),
                GetRealmeEnvironmentValue("SAKURAEDL_REALME_PROJECT_NUMBER", "REALME_PROJECT_NUMBER")));
        }

        private static string GetRealmeConfigValue(string key)
        {
            try
            {
                string value = ConfigurationManager.AppSettings[key];
                return string.IsNullOrWhiteSpace(value) ? null : value.Trim();
            }
            catch
            {
                return null;
            }
        }

        private static string GetRealmeEnvironmentValue(params string[] keys)
        {
            foreach (string key in keys)
            {
                string value = Environment.GetEnvironmentVariable(key, EnvironmentVariableTarget.Process);
                if (string.IsNullOrWhiteSpace(value))
                    value = Environment.GetEnvironmentVariable(key, EnvironmentVariableTarget.User);
                if (string.IsNullOrWhiteSpace(value))
                    value = Environment.GetEnvironmentVariable(key, EnvironmentVariableTarget.Machine);
                if (!string.IsNullOrWhiteSpace(value))
                    return value.Trim();
            }

            return null;
        }

        private static string NormalizeRealmeProjectNumber(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
                return null;

            string digits = new string(value.Where(char.IsDigit).ToArray());
            if (digits.Length >= 4 && digits.Length <= 6)
                return digits;

            return value.Trim();
        }

        private static string FirstNonEmpty(params string[] values)
        {
            foreach (string value in values)
            {
                if (!string.IsNullOrWhiteSpace(value))
                    return value.Trim();
            }

            return null;
        }

        private static string ComputeSha256Hex(byte[] data)
        {
            if (data == null || data.Length == 0)
                return string.Empty;

            using (SHA256 sha256 = SHA256.Create())
            {
                byte[] hash = sha256.ComputeHash(data);
                return BitConverter.ToString(hash).Replace("-", string.Empty).ToLowerInvariant();
            }
        }

        private static string GetRealmeLoaderLabel(RealmeFirehoseProtocol protocol)
        {
            switch (protocol)
            {
                case RealmeFirehoseProtocol.LegacyPreXmlDigest:
                    return "Legacy Realme loader (initdigest)";
                case RealmeFirehoseProtocol.LegacySimplified:
                    return "Legacy Realme loader (simplified)";
                case RealmeFirehoseProtocol.Modern:
                    return "Modern Realme loader";
                default:
                    return "Unknown Realme loader";
            }
        }

        private async Task TryFetchLoaderConfigFromCloudAsync(CancellationToken ct)
        {
            QualcommChipInfo chip = GetChipInfo();
            string chipName = chip != null ? chip.ChipName : null;
            if (string.IsNullOrWhiteSpace(chipName) || chipName == "Unknown") return;

            string signUrl = FirstNonEmpty(
                _realmeAuthOptions != null ? _realmeAuthOptions.ApiUrl : null,
                GetRealmeConfigValue("RealmeSignApiUrl"),
                "https://opluspro.top/api/sign/sign");
            Uri uri;
            if (!Uri.TryCreate(signUrl, UriKind.Absolute, out uri)) return;
            string baseUrl = uri.GetLeftPart(UriPartial.Authority);

            string queryUrl = string.Format("{0}/api/loaders/query?model={1}", baseUrl, chipName);
            try
            {
                using (var client = new HttpClient { Timeout = TimeSpan.FromSeconds(10) })
                using (var response = await client.GetAsync(queryUrl, ct).ConfigureAwait(false))
                {
                    if (!response.IsSuccessStatusCode) return;
                    string json = await response.Content.ReadAsStringAsync().ConfigureAwait(false);

                    // 响应格式: { "code": "000000", "data": [ { loader record } ] }
                    var root = JsonNode.Parse(json)?.AsObject();
                    if (root == null) return;

                    var dataList = root["data"]?.AsArray();
                    if (dataList == null || dataList.Count == 0) return;

                    var record = dataList[0]?.AsObject();
                    if (record == null) return;

                    if (_realmeAuthOptions == null) _realmeAuthOptions = new QualcommRealmeAuthOptions();

                    // project_id
                    string projectId = GetDictStr(record, "project_id");
                    if (!string.IsNullOrWhiteSpace(projectId) && string.IsNullOrWhiteSpace(_realmeAuthOptions.ProjectNumber))
                    {
                        _realmeAuthOptions.ProjectNumber = projectId;
                        _log(string.Format("[Realme][Loader] 云端自动填充 ProjectNumber: {0}", projectId));
                    }

                    // loader_file / digest_file — 仅在本地未提供时作为候选
                    string loaderFile = GetDictStr(record, "loader_file");
                    string digestFile = GetDictStr(record, "digest_file");

                    if (!string.IsNullOrWhiteSpace(loaderFile))
                    {
                        _realmeAuthOptions.CloudLoaderFile = loaderFile;
                        _log(string.Format("[Realme][Loader] 云端 Loader 文件: {0}", loaderFile));
                    }
                    if (!string.IsNullOrWhiteSpace(digestFile))
                    {
                        _realmeAuthOptions.CloudDigestFile = digestFile;
                        _log(string.Format("[Realme][Loader] 云端 Digest 文件: {0}", digestFile));
                    }
                }
            }
            catch { /* non-blocking — ignore network errors */ }
        }

        private static string GetDictStr(Dictionary<string, object> d, string key)
        {
            object v;
            return (d != null && d.TryGetValue(key, out v) && v != null) ? v.ToString() : null;
        }

        private static string GetDictStr(JsonObject d, string key)
        {
            var node = d != null ? d[key] : null;
            return node != null ? node.GetValue<string>() : null;
        }

        private List<RealmeDigestCandidate> LoadRealmeDigestCandidates(string digestPath)
        {
            QualcommRealmeAuthOptions options = GetRealmeAuthOptions();
            string projectNumber = ResolveRealmeProjectNumber(options);
            List<string> candidatePaths = GetRealmeDigestCandidatePaths(digestPath, projectNumber);
            var candidates = new List<RealmeDigestCandidate>();

            foreach (string candidatePath in candidatePaths)
            {
                try
                {
                    byte[] digestData = File.ReadAllBytes(candidatePath);
                    candidates.Add(new RealmeDigestCandidate
                    {
                        DisplayName = Path.GetFileName(candidatePath),
                        SourcePath = candidatePath,
                        Data = digestData,
                        Sha256 = ComputeSha256Hex(digestData)
                    });
                }
                catch (Exception ex)
                {
                    _log(string.Format("[Realme] 读取 Digest 失败: {0} ({1})", Path.GetFileName(candidatePath), ex.Message));
                }
            }

            return candidates;
        }

        private static List<RealmeDigestCandidate> CreateInlineRealmeDigestCandidate(byte[] digestData)
        {
            var candidates = new List<RealmeDigestCandidate>();
            if (digestData == null || digestData.Length == 0)
                return candidates;

            candidates.Add(new RealmeDigestCandidate
            {
                DisplayName = "inline digest",
                SourcePath = null,
                Data = digestData,
                Sha256 = ComputeSha256Hex(digestData)
            });
            return candidates;
        }

        private void LogRealmeDigestCandidateAttempt(int index, int total, RealmeDigestCandidate candidate)
        {
            string displayName = candidate != null ? candidate.DisplayName : "(unknown)";
            string sha256 = candidate != null ? candidate.Sha256 : string.Empty;
            string shortHash = !string.IsNullOrEmpty(sha256) && sha256.Length > 16 ? sha256.Substring(0, 16) : sha256;
            int length = candidate != null && candidate.Data != null ? candidate.Data.Length : 0;

            _logDetail(string.Format(
                "[Realme] 尝试 Digest {0}/{1}: {2} ({3} 字节, sha256={4}...)",
                index + 1,
                total,
                displayName,
                length,
                shortHash));

            if (candidate != null && !string.IsNullOrWhiteSpace(candidate.SourcePath))
                _logDetail(string.Format("[Realme] Digest 路径: {0}", candidate.SourcePath));
            if (!string.IsNullOrEmpty(sha256))
                _logDetail(string.Format("[Realme] Digest SHA-256: {0}", sha256));
        }

        private static List<string> GetRealmeDigestCandidatePaths(string digestPath, string projectNumber)
        {
            var candidates = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            if (string.IsNullOrWhiteSpace(digestPath))
                return candidates;

            string fullDigestPath = null;
            string digestDirectory = null;

            if (File.Exists(digestPath))
            {
                fullDigestPath = Path.GetFullPath(digestPath);
                digestDirectory = Path.GetDirectoryName(fullDigestPath);
            }
            else if (Directory.Exists(digestPath))
            {
                digestDirectory = Path.GetFullPath(digestPath);
            }
            else
            {
                return candidates;
            }

            Action<string> addCandidate = path =>
            {
                if (string.IsNullOrWhiteSpace(path))
                    return;

                string fullPath;
                try
                {
                    fullPath = Path.GetFullPath(path);
                }
                catch
                {
                    return;
                }

                if (!File.Exists(fullPath))
                    return;

                if (seen.Add(fullPath))
                    candidates.Add(fullPath);
            };

            addCandidate(fullDigestPath);

            string normalizedProject = NormalizeRealmeProjectNumber(projectNumber);
            if (!string.IsNullOrEmpty(digestDirectory) && !string.IsNullOrEmpty(normalizedProject))
            {
                string[] preferredNames =
                {
                    string.Format("DigestsToSign_{0}_persist_no_userdata_yes.bin.mbn", normalizedProject),
                    string.Format("DigestsToSign_{0}_persist_yes_userdata_yes.bin.mbn", normalizedProject),
                    string.Format("DigestsToSign_{0}_persist_yes_userdata_no.bin.mbn", normalizedProject),
                    string.Format("DigestsToSign_{0}_persist_no_userdata_no.bin.mbn", normalizedProject),
                    string.Format("DigestsToSign_{0}_all.bin.mbn", normalizedProject)
                };

                foreach (string preferredName in preferredNames)
                    addCandidate(Path.Combine(digestDirectory, preferredName));

                string searchPattern = string.Format("DigestsToSign_{0}_*.mbn", normalizedProject);
                foreach (string siblingPath in Directory.GetFiles(digestDirectory, searchPattern).OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
                    addCandidate(siblingPath);
            }

            return candidates;
        }

        private async Task<bool> PerformRealmeCloudAuthWithDigestCandidatesAsync(List<string> candidatePaths, CancellationToken ct)
        {
            string lastAttemptedCandidatePath = null;

            for (int i = 0; i < candidatePaths.Count; i++)
            {
                ct.ThrowIfCancellationRequested();

                string candidatePath = candidatePaths[i];
                lastAttemptedCandidatePath = candidatePath;

                try
                {
                    byte[] digestData = File.ReadAllBytes(candidatePath);
                    string sha256 = ComputeSha256Hex(digestData);
                    string shortHash = sha256.Length > 16 ? sha256.Substring(0, 16) : sha256;

                    _logDetail(string.Format(
                        "[Realme] 尝试 Digest {0}/{1}: {2} ({3} 字节, sha256={4}...)",
                        i + 1,
                        candidatePaths.Count,
                        Path.GetFileName(candidatePath),
                        digestData.Length,
                        shortHash));
                    _logDetail(string.Format("[Realme] Digest 路径: {0}", candidatePath));
                    _logDetail(string.Format("[Realme] Digest SHA-256: {0}", sha256));

                    if (await PerformRealmeCloudAuthAsync(digestData, ct).ConfigureAwait(false))
                    {
                        _logDetail(string.Format("[Realme] 已命中 Digest: {0}", Path.GetFileName(candidatePath)));
                        return true;
                    }
                }
                catch (OperationCanceledException)
                {
                    SetRealmeAuthState(false);
                    throw;
                }
                catch (Exception ex)
                {
                    SetRealmeAuthState(false);
                    _log(string.Format("[Realme] 读取 Digest 失败: {0} ({1})", Path.GetFileName(candidatePath), ex.Message));
                }

                _log(string.Format("[Realme] 当前 Digest 未通过: {0}", Path.GetFileName(candidatePath)));

                if (i < candidatePaths.Count - 1)
                    _log("[Realme] 当前 Digest 未通过，尝试下一个候选...");
            }

            if (!string.IsNullOrEmpty(lastAttemptedCandidatePath))
            {
                _log(string.Format(
                    "[Realme] 所有 Digest 候选均未通过，最后停在: {0}",
                    Path.GetFileName(lastAttemptedCandidatePath)));
            }

            return false;
        }

        private async Task<bool> PerformRealmeCloudAuthWithDigestCandidatesAsync(List<RealmeDigestCandidate> candidates, CancellationToken ct)
        {
            string lastAttemptedCandidate = null;

            for (int i = 0; i < candidates.Count; i++)
            {
                ct.ThrowIfCancellationRequested();

                RealmeDigestCandidate candidate = candidates[i];
                string displayName = candidate != null ? candidate.DisplayName : "(unknown)";
                lastAttemptedCandidate = displayName;

                try
                {
                    LogRealmeDigestCandidateAttempt(i, candidates.Count, candidate);

                    if (await PerformRealmeCloudAuthAsync(candidate.Data, ct).ConfigureAwait(false))
                    {
                        _logDetail(string.Format("[Realme] 已命中 Digest: {0}", displayName));
                        return true;
                    }
                }
                catch (OperationCanceledException)
                {
                    SetRealmeAuthState(false);
                    throw;
                }
                catch (Exception ex)
                {
                    SetRealmeAuthState(false);
                    _log(string.Format("[Realme] 读取 Digest 失败: {0} ({1})", displayName, ex.Message));
                }

                _log(string.Format("[Realme] 当前 Digest 未通过: {0}", displayName));

                if (i < candidates.Count - 1)
                    _log("[Realme] 当前 Digest 未通过，尝试下一个候选...");
            }

            if (!string.IsNullOrEmpty(lastAttemptedCandidate))
            {
                _log(string.Format(
                    "[Realme] 所有 Digest 候选均未通过，最后停在: {0}",
                    lastAttemptedCandidate));
            }

            return false;
        }

        public async Task<bool> PerformRealmeCloudAuthAsync(string digestPath, CancellationToken ct = default(CancellationToken))
        {
            QualcommRealmeAuthOptions options = GetRealmeAuthOptions();
            string projectNumber = ResolveRealmeProjectNumber(options);
            List<string> candidatePaths = GetRealmeDigestCandidatePaths(digestPath, projectNumber);

            if (candidatePaths.Count == 0)
            {
                _log("[Realme] 缺少 Digest 文件");
                return false;
            }

            try
            {
                return await PerformRealmeCloudAuthWithDigestCandidatesAsync(candidatePaths, ct);
            }
            catch (Exception ex)
            {
                SetRealmeAuthState(false);
                _log(string.Format("[Realme] 读取 Digest 失败: {0}", ex.Message));
                return false;
            }
        }

        private async Task<bool> ConfigureAndAuthenticateRealmeAsync(string storageType, string digestPath, CancellationToken ct)
        {
            await TryFetchLoaderConfigFromCloudAsync(ct).ConfigureAwait(false);

            // 若本地未指定 Digest 路径，尝试用云端文件名在固件目录中定位
            if (string.IsNullOrWhiteSpace(digestPath))
            {
                QualcommRealmeAuthOptions opts = GetRealmeAuthOptions();
                string cloudDigest = opts != null ? opts.CloudDigestFile : null;
                string firmwareDir = opts != null ? opts.FirmwarePath : null;
                if (!string.IsNullOrWhiteSpace(cloudDigest) && !string.IsNullOrWhiteSpace(firmwareDir))
                {
                    string candidate = Path.Combine(firmwareDir, cloudDigest);
                    if (File.Exists(candidate))
                    {
                        digestPath = candidate;
                        _log(string.Format("[Realme][Loader] 使用云端 Digest: {0}", Path.GetFileName(candidate)));
                    }
                }
            }

            List<RealmeDigestCandidate> candidates = LoadRealmeDigestCandidates(digestPath);
            if (candidates.Count == 0)
            {
                _log("[Realme] 缺少 Digest 文件");
                return false;
            }

            return await ConfigureAndAuthenticateRealmeAsync(storageType, candidates, ct).ConfigureAwait(false);
        }

        private async Task<bool> ConfigureAndAuthenticateRealmeAsync(string storageType, byte[] digestData, CancellationToken ct)
        {
            List<RealmeDigestCandidate> candidates = CreateInlineRealmeDigestCandidate(digestData);
            if (candidates.Count == 0)
            {
                _log("[Realme] 缺少 Digest 数据");
                return false;
            }

            return await ConfigureAndAuthenticateRealmeAsync(storageType, candidates, ct).ConfigureAwait(false);
        }

        private async Task<bool> ConfigureAndAuthenticateRealmeAsync(string storageType, List<RealmeDigestCandidate> candidates, CancellationToken ct)
        {
            if (_firehose == null)
                return false;

            QualcommRealmeAuthOptions opts = GetRealmeAuthOptions();
            bool forceModern = opts != null && opts.UseModernProtocol;

            await _firehose.PrepareForRealmeHandshakeAsync(ct).ConfigureAwait(false);

            RealmeFirehoseProtocol protocol = _firehose.DetectedRealmeProtocol;
            if (protocol == RealmeFirehoseProtocol.Unknown)
            {
                protocol = RealmeFirehoseProtocol.Modern;
                _log("[Realme] 未识别 loader build date，先按 modern Realme 流程继续");
            }
            else
            {
                _log(string.Format(
                    "[Realme] 检测到 {0}: {1}",
                    GetRealmeLoaderLabel(protocol),
                    string.IsNullOrWhiteSpace(_firehose.RealmeLoaderBuildDate) ? "(unknown)" : _firehose.RealmeLoaderBuildDate));
            }

            if (forceModern)
            {
                _log("[Realme] 用户选择新版协议，强制 Modern 流程 (EnableVip=1, 无 initdigest)");
                protocol = RealmeFirehoseProtocol.Modern;
            }

            if (protocol == RealmeFirehoseProtocol.LegacyPreXmlDigest)
                return await ConfigureAndAuthenticateLegacyRealmeAsync(storageType, candidates, ct).ConfigureAwait(false);

            if (protocol == RealmeFirehoseProtocol.LegacySimplified)
                return await ConfigureAndAuthenticateSimplifiedRealmeAsync(storageType, candidates, ct).ConfigureAwait(false);

            // Modern Realme (SM8650+): nop 不会收到 ACK，设备仅回复大量 INFO 日志
            // (芯片信息、anti-rollback、支持的功能列表)。使用 drain 方式排空日志，
            // 不等待 ACK，避免 15s 超时导致端口关闭。
            _log("[Realme] Modern: 发送 nop 并排空设备日志...");
            await _firehose.SendNopAndDrainLogsAsync(ct).ConfigureAwait(false);
            _log("[Realme] Modern: 设备日志已排空");

            // nop 排空后可能发现设备实际需要 Legacy 流程
            if (!forceModern)
            {
                var detected = _firehose.DetectedRealmeProtocol;
                if (detected == RealmeFirehoseProtocol.LegacyPreXmlDigest)
                {
                    _log("[Realme] nop 响应中检测到 initdigest 支持，回退到 Legacy 流程");
                    return await ConfigureAndAuthenticateLegacyRealmeAsync(storageType, candidates, ct, skipNop: true).ConfigureAwait(false);
                }
                if (detected == RealmeFirehoseProtocol.LegacySimplified)
                {
                    _log("[Realme] nop 响应中识别为 May 12 2020 简化流程");
                    return await ConfigureAndAuthenticateSimplifiedRealmeAsync(storageType, candidates, ct, skipNop: true).ConfigureAwait(false);
                }
            }

            _log("[Realme] 继续 Modern configure");
            bool enableVip = forceModern;
            if (!await _firehose.ConfigureRealmeAsync(storageType, false, ct, enableVip).ConfigureAwait(false))
                return false;

            // configure 后查询 diskId
            await _firehose.QueryRealmeDiskIdAsync(ct).ConfigureAwait(false);

            bool authOk = await PerformRealmeCloudAuthWithDigestCandidatesAsync(candidates, ct).ConfigureAwait(false);
            if (!authOk)
                _log("[Realme] Firehose 已配置，但 modern Realme 认证未通过");
            return authOk;
        }

        /// <summary>
        /// Legacy Realme auth flow (from official tool Bus Hound capture 8678.txt):
        ///   nop -> ACK -> configure -> ACK -> initdigest XML -> ACK(rawmode=true)
        ///   -> raw DigestsToSign binary -> ACK(rawmode=false) -> cloud auth
        /// The initdigest XML command tells the device to expect raw digest data,
        /// then the MBN binary is sent. Without initdigest, raw binary causes hash mismatch.
        /// </summary>
        private async Task<bool> ConfigureAndAuthenticateLegacyRealmeAsync(string storageType, List<RealmeDigestCandidate> candidates, CancellationToken ct, bool skipNop = false)
        {
            // Step 1: nop ping (device sends startup logs first, then ACK)
            if (skipNop)
                _log("[Realme] Legacy: nop 已在 Modern 检测中发送，跳过");
            else
                await SendRealmePingAsync(ct).ConfigureAwait(false);

            // Step 2: configure
            if (!await _firehose.ConfigureRealmeAsync(storageType, false, ct).ConfigureAwait(false))
            {
                _log("[Realme] legacy configure 失败");
                return false;
            }

            // Step 2.5: 查询 diskId（configure 后设备已就绪）
            await _firehose.QueryRealmeDiskIdAsync(ct).ConfigureAwait(false);

            // Step 3: initdigest XML + raw DigestsToSign binary
            byte[] digestData = null;
            string digestLabel = null;
            if (candidates.Count > 0 && candidates[0].Data != null && candidates[0].Data.Length > 0)
            {
                digestData = candidates[0].Data;
                digestLabel = candidates[0].DisplayName;
            }
            else if (_realmeElfHashSegment != null && _realmeElfHashSegment.Length > 0)
            {
                digestData = _realmeElfHashSegment;
                digestLabel = "ELF hash segment (fallback)";
            }

            if (digestData != null)
            {
                _logDetail(string.Format("[Realme] 发送 initdigest + DigestsToSign ({0} 字节): {1}", digestData.Length, digestLabel));
                bool digestOk = await _firehose.InitializeRealmeDigestAsync(digestData, ct).ConfigureAwait(false);
                if (!digestOk)
                {
                    _log("[Realme] initdigest 失败");
                    return false;
                }
                _logDetail("[Realme] initdigest 成功");
            }
            else
            {
                _log("[Realme] 未找到 DigestsToSign 数据，跳过 initdigest 步骤");
            }

            // Step 4: cloud auth
            bool authOk = await PerformRealmeCloudAuthWithDigestCandidatesAsync(candidates, ct).ConfigureAwait(false);
            if (!authOk)
                _log("[Realme] Firehose 已配置，但 legacy Realme 认证未通过");
            return authOk;
        }

        /// <summary>
        /// May 12 2020 loader 简化流程 (Bus Hound 123456.txt / 90000000009.txt 抓包确认):
        ///   RAW DigestsToSign → ACK → nop → ACK → configure → ACK → getsigndata → cloud auth → verify
        /// 与完整 Legacy 的区别:
        ///   - DigestsToSign 以原始二进制在 XML 命令之前发送 (无 initdigest XML 包装)
        ///   - 没有 getstorageinfo 步骤
        /// 如果不发送 RAW DigestsToSign，设备报 "Verifying signature failed with 3"。
        /// </summary>
        private async Task<bool> ConfigureAndAuthenticateSimplifiedRealmeAsync(string storageType, List<RealmeDigestCandidate> candidates, CancellationToken ct, bool skipNop = false)
        {
            // Step 0: 在任何 XML 命令之前发送 programmer 的 MBN hash segment (原始二进制)
            // 注意: 只能使用 _realmeElfHashSegment (从 programmer ELF 提取)，
            // 不能用 candidates (flash 包的 DigestsToSign)，二者数据结构完全不同。
            if (_realmeElfHashSegment != null && _realmeElfHashSegment.Length > 0)
            {
                bool rawOk = await _firehose.SendRawDigestBeforeFirehoseAsync(_realmeElfHashSegment, ct).ConfigureAwait(false);
                if (!rawOk)
                {
                    _log("[Realme] Simplified: programmer hash segment 发送失败，尝试继续");
                }
            }
            else
            {
                _log("[Realme] Simplified: 警告 - 未从 programmer ELF 提取到 hash segment，设备可能拒绝后续命令");
            }

            // Step 1: nop (标准 ping，设备返回 ACK)
            if (skipNop)
                _log("[Realme] Simplified: nop 已在 Modern 检测中发送，跳过");
            else
                await SendRealmePingAsync(ct).ConfigureAwait(false);

            // Step 2: configure (官方参数，无 EnableFlash / EnableVip / ModeType)
            if (!await _firehose.ConfigureRealmeSimplifiedAsync(storageType, ct).ConfigureAwait(false))
            {
                _log("[Realme] simplified configure 失败");
                return false;
            }

            // nop+configure 的响应可能暴露 initdigest 支持，此时升级为完整 Legacy
            if (_firehose.DetectedRealmeProtocol == RealmeFirehoseProtocol.LegacyPreXmlDigest)
            {
                _log("[Realme] configure 响应中检测到 initdigest 支持，升级为完整 Legacy 流程");
                return await ConfigureAndAuthenticateLegacyRealmeAsync(storageType, candidates, ct, skipNop: true).ConfigureAwait(false);
            }

            // Step 3: 直接 cloud auth (getsigndata → 云端签名 → verify)，不走 getstorageinfo / initdigest
            _log("[Realme] Simplified 流程: 直接 getsigndata");
            bool authOk = await PerformRealmeCloudAuthWithDigestCandidatesAsync(candidates, ct).ConfigureAwait(false);
            if (!authOk)
                _log("[Realme] Firehose 已配置，但 simplified Realme 认证未通过");
            return authOk;
        }

        /// <summary>
        /// Extract the hash segment (MBN header + hash table + signature + cert chain)
        /// from a Qualcomm ELF programmer binary. The hash segment is needed for
        /// Realme pre-authentication: sent as raw binary before any XML commands.
        /// </summary>
        private byte[] ExtractElfHashSegment(byte[] elfData)
        {
            if (elfData == null || elfData.Length < 64)
            {
                _log("[Realme] ExtractElfHashSegment: 数据为空或太小");
                return null;
            }

            if (elfData[0] != 0x7F || elfData[1] != (byte)'E' || elfData[2] != (byte)'L' || elfData[3] != (byte)'F')
            {
                _log(string.Format("[Realme] ExtractElfHashSegment: 不是 ELF 文件 (magic: {0:X2} {1:X2} {2:X2} {3:X2})",
                    elfData[0], elfData[1], elfData[2], elfData[3]));
                return null;
            }

            bool is64 = elfData[4] == 2;
            if (elfData[5] != 1)
            {
                _log(string.Format("[Realme] ExtractElfHashSegment: EI_DATA={0} (非小端)", elfData[5]));
                return null;
            }

            uint e_phoff;
            ushort e_phentsize, e_phnum;

            if (is64)
            {
                e_phoff = (uint)BitConverter.ToUInt64(elfData, 32);
                e_phentsize = BitConverter.ToUInt16(elfData, 54);
                e_phnum = BitConverter.ToUInt16(elfData, 56);
            }
            else
            {
                e_phoff = BitConverter.ToUInt32(elfData, 28);
                e_phentsize = BitConverter.ToUInt16(elfData, 42);
                e_phnum = BitConverter.ToUInt16(elfData, 44);
            }

            _logDetail(string.Format("[Realme] ELF: {0}bit, phoff={1}, phentsize={2}, phnum={3}, filesize={4}",
                is64 ? 64 : 32, e_phoff, e_phentsize, e_phnum, elfData.Length));

            if (e_phnum < 2 || e_phoff + (long)e_phentsize * e_phnum > elfData.Length)
            {
                _log(string.Format("[Realme] ExtractElfHashSegment: program header 越界 (phoff={0} + {1}*{2} > {3})",
                    e_phoff, e_phentsize, e_phnum, elfData.Length));
                return null;
            }

            // Qualcomm ELF 标准布局:
            //   PH[0] = ELF header 自引用 (p_offset=0)
            //   PH[1] = hash segment (包含 hash table + sig + cert chain)
            //   PH[2+] = code segments (PT_LOAD)
            // 策略: 找第一个 p_type==PT_NULL(0) 且 p_offset>0 且 p_filesz>=256 的段
            byte[] hashSegment = null;
            int hashSegmentIdx = -1;

            int limit = Math.Min((int)e_phnum, 32);
            for (int i = 0; i < limit; i++)
            {
                uint phOff = e_phoff + (uint)(i * e_phentsize);
                if (phOff + e_phentsize > (uint)elfData.Length)
                    break;

                ulong p_offset, p_filesz, p_vaddr;
                uint p_type, p_flags;
                if (is64)
                {
                    p_type = BitConverter.ToUInt32(elfData, (int)phOff);
                    p_flags = BitConverter.ToUInt32(elfData, (int)phOff + 4);
                    p_offset = BitConverter.ToUInt64(elfData, (int)phOff + 8);
                    p_vaddr = BitConverter.ToUInt64(elfData, (int)phOff + 16);
                    p_filesz = BitConverter.ToUInt64(elfData, (int)phOff + 32);
                }
                else
                {
                    p_type = BitConverter.ToUInt32(elfData, (int)phOff);
                    p_offset = BitConverter.ToUInt32(elfData, (int)phOff + 4);
                    p_vaddr = BitConverter.ToUInt32(elfData, (int)phOff + 8);
                    p_filesz = BitConverter.ToUInt32(elfData, (int)phOff + 16);
                    p_flags = BitConverter.ToUInt32(elfData, (int)phOff + 24);
                }

                _logDetail(string.Format("[Realme] PH[{0}]: type=0x{1:X}, offset=0x{2:X}, vaddr=0x{3:X}, filesz=0x{4:X} ({4}), flags=0x{5:X}",
                    i, p_type, p_offset, p_vaddr, p_filesz, p_flags));

                if (p_filesz < 256 || p_offset + p_filesz > (ulong)elfData.Length)
                    continue;

                // PT_NULL (0) 且非自引用 (p_offset>0) → 标准 hash segment 位置
                if (p_type == 0 && p_offset > 0 && hashSegment == null)
                {
                    hashSegmentIdx = i;
                    hashSegment = new byte[(int)p_filesz];
                    Array.Copy(elfData, (int)p_offset, hashSegment, 0, (int)p_filesz);

                    int segOff = (int)p_offset;
                    _logDetail(string.Format(
                        "[Realme] PH[{0}] = hash segment: offset=0x{1:X}, size={2}, head=[{3:X2} {4:X2} {5:X2} {6:X2} {7:X2} {8:X2} {9:X2} {10:X2}]",
                        i, p_offset, p_filesz,
                        elfData[segOff], elfData[segOff + 1], elfData[segOff + 2], elfData[segOff + 3],
                        elfData[segOff + 4], elfData[segOff + 5], elfData[segOff + 6], elfData[segOff + 7]));
                }
            }

            if (hashSegment != null)
                return hashSegment;

            _log("[Realme] ELF 中未找到 PT_NULL hash segment");
            return null;
        }

        private static bool ContainsByteSequence(byte[] buffer, byte[] pattern)
        {
            if (buffer == null || pattern == null || buffer.Length == 0 || pattern.Length == 0 || pattern.Length > buffer.Length)
                return false;

            for (int i = 0; i <= buffer.Length - pattern.Length; i++)
            {
                bool matched = true;
                for (int j = 0; j < pattern.Length; j++)
                {
                    if (buffer[i + j] != pattern[j])
                    {
                        matched = false;
                        break;
                    }
                }

                if (matched)
                    return true;
            }

            return false;
        }

        private async Task<bool> ProbeRealmeAuthenticatedAccessAsync(CancellationToken ct)
        {
            if (_firehose == null)
                return false;

            int gptSectors = SectorSize == 4096 ? 6 : 34;
            byte[] gptMagic = Encoding.ASCII.GetBytes("EFI PART");
            bool anySuccess = false;

            for (int lun = 0; lun <= 5; lun++)
            {
                if (ct.IsCancellationRequested) break;

                _logDetail(string.Format("[Realme] 读取 LUN{0} PrimaryGPT ({1} 扇区)...", lun, gptSectors));

                try
                {
                    byte[] gptData = await _firehose.ReadSectorsRealmeAsync(
                        lun, 0, gptSectors, "PrimaryGPT", null, ct, 10000).ConfigureAwait(false);

                    if (gptData != null && gptData.Length > 0)
                    {
                        bool hasGpt = ContainsByteSequence(gptData, gptMagic);
                        _logDetail(string.Format("[Realme] LUN{0} 读取成功 ({1} 字节){2}",
                            lun, gptData.Length, hasGpt ? " - GPT 有效" : ""));
                        anySuccess = true;
                        break; // 验证通过即可，无需继续探测其余 LUN
                    }
                    else
                    {
                        _log(string.Format("[Realme] LUN{0} 读取失败", lun));
                    }
                }
                catch (Exception ex)
                {
                    _log(string.Format("[Realme] LUN{0} 读取异常: {1}", lun, ex.Message));
                }
            }

            if (anySuccess)
                _logDetail("[Realme] 认证后 GPT 回读验证通过");
            else
                _logDetail("[Realme] 所有 LUN 读取均失败");

            return anySuccess;
        }

        // Realme post-getsigndata flow is now confirmed from Bus Hound capture 222.txt:
        // verify EnableVip="0" -> rawmode="true" -> 4096-byte payload
        // (256-byte signature + zero padding) -> verify passed / ACK rawmode="false".
        public async Task<bool> PerformRealmeCloudAuthAsync(byte[] digestData, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[Realme] Firehose 未连接");
                return false;
            }

            if (digestData == null || digestData.Length == 0)
            {
                _log("[Realme] 缺少 Digest 数据");
                return false;
            }

            QualcommRealmeAuthOptions options = GetRealmeAuthOptions();
            string projectNumber = ResolveRealmeProjectNumber(options);
            if (string.IsNullOrWhiteSpace(projectNumber))
            {
                _log("[Realme] 缺少 ProjectID");
                return false;
            }
            options.ProjectNumber = projectNumber;

            // 使用设备实际 diskId（若已查询到）
            if (string.IsNullOrWhiteSpace(options.DiskId) && _firehose != null && !string.IsNullOrEmpty(_firehose.RealmeDiskId))
                options.DiskId = _firehose.RealmeDiskId;

            try
            {
                _log("[Realme] 执行云端签名认证...");

                SetRealmeAuthState(false);

                RealmeSignMaterial material = await _firehose.CollectRealmeSignMaterialAsync(
                    digestData,
                    options.ProjectNumber,
                    ct).ConfigureAwait(false);

                if (material == null)
                {
                    _log("[Realme] 获取签名材料失败");
                    return false;
                }

                byte[] signature = await _realmeSignService.RequestSignatureAsync(
                    material,
                    GetChipInfo(),
                    options,
                    ct).ConfigureAwait(false);

                if (signature == null || signature.Length == 0)
                {
                    _log("[Realme] 签名接口返回空数据");
                    return false;
                }

                if (!await _firehose.SendRealmeSignatureAsync(signature, ct).ConfigureAwait(false))
                {
                    _log("[Realme] Signature upload back to device failed");
                    return false;
                }

                SetRealmeAuthState(true);
                _log("[Realme] 认证成功");

                // 设备侧 verify passed 即视为认证完成，跳过 GPT 回读探测以避免 8 秒空窗

                return true;
            }
            catch (OperationCanceledException)
            {
                SetRealmeAuthState(false);
                _log("[Realme] 云端签名认证已取消");
                throw;
            }
            catch (Exception ex)
            {
                SetRealmeAuthState(false);
                _log(string.Format("[Realme] 云端签名认证异常: {0}", ex.Message));
                return false;
            }
        }
#endif

        #region 连接管理

        /// <summary>
        /// 连接设备
        /// </summary>
        /// <param name="portName">COM 端口名</param>
        /// <param name="programmerPath">Programmer 文件路径</param>
        /// <param name="storageType">存储类型 (ufs/emmc)</param>
        /// <param name="authMode">认证模式: none, vip, oneplus, xiaomi</param>
        /// <param name="digestPath">VIP Digest 文件路径</param>
        /// <param name="signaturePath">VIP Signature 文件路径</param>
        /// <param name="ct">取消令牌</param>
        public async Task<bool> ConnectAsync(string portName, string programmerPath, string storageType = "ufs", 
            string authMode = "none", string digestPath = "", string signaturePath = "",
            CancellationToken ct = default(CancellationToken))
        {
            try
            {
#if EXCLUDE_REALME_AUTH
                if (string.Equals(authMode, "realme", StringComparison.OrdinalIgnoreCase))
                {
                    _log("[高通] 此构建已排除 Realme 认证，请选择其他认证模式");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }
#endif
                // 释放上次连接失败遗留的端口，避免二次连接时“无法打开端口”
                Disconnect();
                await Task.Delay(100, ct);

                SetState(QualcommConnectionState.Connecting);
                ResetAuthState();
                _log("等待高通 EDL USB 设备 : 成功");
                _log(string.Format("USB 端口 : {0}", portName));
                _log("正在连接设备 : 成功");

                // 验证 Programmer 文件
                if (!File.Exists(programmerPath))
                {
                    _log("[高通] Programmer 文件不存在: " + programmerPath);
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // 初始化串口
                _portManager = new SerialPortManager();

                // Sahara 模式必须保留初始 Hello 包，不清空缓冲区
                bool opened = await _portManager.OpenAsync(portName, 3, false, ct);
                if (!opened)
                {
                    _log("[高通] 无法打开端口");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // Sahara 握手
                SetState(QualcommConnectionState.SaharaMode);
                
                // 创建 Sahara 客户端并传递进度回调
                Action<double> saharaProgress = null;
                if (_progress != null)
                {
                    saharaProgress = percent => _progress((long)percent, 100);
                }
                _sahara = new SaharaClient(_portManager, _log, _logDetail, saharaProgress);

#if !EXCLUDE_REALME_AUTH
                // Realme 认证：先发 ResetStateMachine 确保状态干净，再进入命令模式读取芯片信息
                if (string.Equals(authMode, "realme", StringComparison.OrdinalIgnoreCase))
                {
                    _sahara.SendResetBeforeTransfer = true;
                }
#endif

                bool saharaOk = await _sahara.HandshakeAndUploadAsync(programmerPath, ct);
                if (!saharaOk)
                {
                    _log("[高通] Sahara 握手失败");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // 根据用户选择的认证模式设置标志 (不再自动检测)
                SetVipAuthState(authMode.ToLowerInvariant() == "vip" || authMode.ToLowerInvariant() == "oplus");

                _log("正在发送 Firehose 引导文件 : 成功");

#if !EXCLUDE_REALME_AUTH
                bool isRealmeAuth = string.Equals(authMode, "realme", StringComparison.OrdinalIgnoreCase);
                if (isRealmeAuth)
                {
                    _logDetail("[Realme] 保持端口不关闭，避免 USB 端点重置");
                    try
                    {
                        byte[] elfData = File.ReadAllBytes(programmerPath);
                        _realmeElfHashSegment = ExtractElfHashSegment(elfData);
                        if (_realmeElfHashSegment != null)
                            _logDetail(string.Format("[Realme] 从 ELF 提取 hash segment: {0} 字节", _realmeElfHashSegment.Length));
                        else
                            _log("[Realme] ELF 中未找到 hash segment");
                    }
                    catch (Exception ex)
                    {
                        _log(string.Format("[Realme] 提取 hash segment 异常: {0}", ex.Message));
                    }

                    QualcommRealmeAuthOptions earlyOpts = GetRealmeAuthOptions();
                    bool isModernRealme = earlyOpts != null && earlyOpts.UseModernProtocol;
                    if (isModernRealme)
                    {
                        await Task.Delay(300, ct);
                        await SendModernRealmeDigestAfterSaharaAsync(digestPath, ct).ConfigureAwait(false);
                        await Task.Delay(500, ct);
                    }
                    else
                    {
                        await Task.Delay(1000, ct);
                    }
                }
                else
#endif
                {
                    await Task.Delay(1000, ct);

                    // 重新打开端口 (Firehose 模式)
                    _portManager.Close();
                    await Task.Delay(500, ct);

                    opened = await _portManager.OpenAsync(portName, 5, ShouldDiscardFirehoseBufferOnOpen(authMode), ct);
                    if (!opened)
                    {
                        _log("[高通] 无法重新打开端口");
                        SetState(QualcommConnectionState.Error);
                        return false;
                    }
                }

                // Firehose 配置
                SetState(QualcommConnectionState.FirehoseMode);
                _firehose = new FirehoseClient(_portManager, _log, _progress, _logDetail);

                // 传递芯片信息
                if (ChipInfo != null)
                {
                    _firehose.ChipSerial = ChipInfo.SerialHex;
                    _firehose.ChipHwId = ChipInfo.HwIdHex;
                    _firehose.ChipPkHash = ChipInfo.PkHash;
                }

                // 根据用户选择执行认证 (配置前认证)
                string authModeLower = authMode.ToLowerInvariant();
                bool preConfigAuth = (authModeLower == "vip" || authModeLower == "oplus" || authModeLower == "xiaomi");
                
                // 小米设备自动认证：即使用户选择 none，也自动执行小米认证
                bool isXiaomi = IsXiaomiDevice();
                if (authModeLower == "none" && isXiaomi)
                {
                    _log("[高通] 检测到小米设备 (SecBoot)，自动执行 MiAuth 认证...");
                    var xiaomi = new XiaomiAuthStrategy(_log);
                    xiaomi.OnAuthTokenRequired += token => XiaomiAuthTokenRequired?.Invoke(token);
                    bool authOk = await xiaomi.AuthenticateAsync(_firehose, programmerPath, ct);
                    if (authOk)
                        _log("[高通] 小米认证成功");
                    else
                        _log("[高通] 小米认证失败，设备可能需要官方授权");
                }
                else if (preConfigAuth && authModeLower != "none")
                {
                    _log(string.Format("[高通] 执行 {0} 认证 (配置前)...", authMode.ToUpper()));
                    bool authOk = false;
                    
                    if (authModeLower == "vip" || authModeLower == "oplus")
                    {
                        // VIP 认证必须在配置前
                        if (!string.IsNullOrEmpty(digestPath) && !string.IsNullOrEmpty(signaturePath))
                        {
                            authOk = await PerformVipAuthManualAsync(digestPath, signaturePath, ct);
                        }
                        else
                        {
                            _log("[高通] VIP 认证需要 Digest 和 Signature 文件，将回退到普通模式");
                            // 没有认证文件，回退到普通模式
                            SetVipAuthState(false);
                        }
                    }
                    else if (authModeLower == "xiaomi")
                    {
                        var xiaomi = new XiaomiAuthStrategy(_log);
                        xiaomi.OnAuthTokenRequired += token => XiaomiAuthTokenRequired?.Invoke(token);
                        authOk = await xiaomi.AuthenticateAsync(_firehose, programmerPath, ct);
                    }
                    
                    if (authOk)
                    {
                        _log(string.Format("[高通] {0} 认证成功", authMode.ToUpper()));
                    }
                    else if (IsVipDevice)
                    {
                        // VIP 认证失败但有文件，回退到普通模式
                        _log(string.Format("[高通] {0} 认证失败，回退到普通读取模式", authMode.ToUpper()));
                        SetVipAuthState(false);
                    }
                }

                _log("正在配置 Firehose...");
#if !EXCLUDE_REALME_AUTH
                if (authModeLower == "realme")
                {
                    bool realmeOk = await ConfigureAndAuthenticateRealmeAsync(storageType, digestPath, ct).ConfigureAwait(false);
                    if (!realmeOk)
                    {
                        _log("配置 Firehose : 失败");
                        SetState(QualcommConnectionState.Error);
                        return false;
                    }

                    _log("配置 Firehose : 成功");
                    LastPortName = portName;
                    LastStorageType = storageType;

                    if (_portManager != null)
                    {
                        _portManager.PortDisconnected += (s, e) => HandlePortDisconnected();
                    }

                    SetState(QualcommConnectionState.Ready);
                    _log("[高通] 连接成功");
                    return true;
                }
#endif

                bool configOk = await ConfigureFirehoseForAuthModeAsync(storageType, authModeLower, ct);
                if (!configOk)
                {
                    _log("配置 Firehose : 失败");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }
                _log("配置 Firehose : 成功");

                // 配置后认证 (OnePlus / Realme)
                if (!preConfigAuth && authModeLower != "none")
                {
                    _log(string.Format("[高通] 执行 {0} 认证 (配置后)...", authMode.ToUpper()));
                    bool authOk = false;
                    
                    if (authModeLower == "oneplus" || authModeLower == "demacia")
                    {
                        var oneplus = new OnePlusAuthStrategy(_log);
                        authOk = await oneplus.AuthenticateAsync(_firehose, programmerPath, ct);
                    }
#if !EXCLUDE_REALME_AUTH
                    else if (authModeLower == "realme")
                    {
                        authOk = await PerformRealmeCloudAuthAsync(digestPath, ct);
                        if (!authOk)
                        {
                            SetState(QualcommConnectionState.Error);
                            return false;
                        }
                    }
#endif
                    
                    if (authOk)
                        _log(string.Format("[高通] {0} 认证成功", authMode.ToUpper()));
                    else
                        _log(string.Format("[高通] {0} 认证失败", authMode.ToUpper()));
                }

                // 保存连接参数
                LastPortName = portName;
                LastStorageType = storageType;

                // 注册端口断开事件
                if (_portManager != null)
                {
                    _portManager.PortDisconnected += (s, e) => HandlePortDisconnected();
                }
                
                SetState(QualcommConnectionState.Ready);
                _log("[高通] 连接成功");

                return true;
            }
            catch (OperationCanceledException)
            {
                _log("[高通] 连接已取消");
                SetState(QualcommConnectionState.Disconnected);
                return false;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 连接错误 - {0}", ex.Message));
                SetState(QualcommConnectionState.Error);
                return false;
            }
            finally
            {
                if (State != QualcommConnectionState.Ready)
                    Disconnect();
            }
        }

        /// <summary>
        /// 使用内嵌 Loader 数据连接设备 (VIP 模式，不含认证)
        /// </summary>
        public async Task<bool> ConnectWithLoaderDataAsync(string portName, byte[] loaderData, string storageType = "ufs", CancellationToken ct = default(CancellationToken))
        {
            return await ConnectWithVipAuthAsync(portName, loaderData, "", "", storageType, ct);
        }

        /// <summary>
        /// 使用内嵌 Loader 数据连接并执行 VIP 认证 (使用文件路径方式)
        /// 重要：VIP 认证在 Loader 上传后、Firehose 配置前执行
        /// </summary>
        /// <param name="portName">端口名</param>
        /// <param name="loaderData">Loader 二进制数据</param>
        /// <param name="digestPath">VIP Digest 文件路径 (可选)</param>
        /// <param name="signaturePath">VIP Signature 文件路径 (可选)</param>
        /// <param name="storageType">存储类型</param>
        /// <param name="ct">取消令牌</param>
        public async Task<bool> ConnectWithVipAuthAsync(string portName, byte[] loaderData, string digestPath, string signaturePath, string storageType = "ufs", CancellationToken ct = default(CancellationToken))
        {
            try
            {
                Disconnect();
                await Task.Delay(100, ct);

                SetState(QualcommConnectionState.Connecting);
                ResetAuthState();
                _log("[高通] 使用内嵌 Loader 连接...");
                _log(string.Format("USB 端口 : {0}", portName));

                if (loaderData == null || loaderData.Length == 0)
                {
                    _log("[高通] Loader 数据为空");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // 初始化串口
                _portManager = new SerialPortManager();
                bool opened = await _portManager.OpenAsync(portName, 3, false, ct);
                if (!opened)
                {
                    _log("[高通] 无法打开端口");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // Sahara 握手并上传内嵌 Loader
                SetState(QualcommConnectionState.SaharaMode);
                Action<double> saharaProgress = null;
                if (_progress != null)
                {
                    saharaProgress = percent => _progress((long)percent, 100);
                }
                _sahara = new SaharaClient(_portManager, _log, _logDetail, saharaProgress);
                
                bool saharaOk = await _sahara.HandshakeAndUploadAsync(loaderData, "VIP_Loader", ct);
                if (!saharaOk)
                {
                    _log("[高通] Sahara 握手/Loader 上传失败");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // 芯片信息已通过 _sahara.ChipInfo 保存，ChipInfo 属性会自动获取
                // 注意：IsVipDevice 将在 VIP 认证成功后设置

                // 等待 Firehose 就绪
                _log("正在发送 Firehose 引导文件 : 成功");
                await Task.Delay(1000, ct);

                // 重新打开端口 (Firehose 模式)
                _portManager.Close();
                await Task.Delay(500, ct);

                opened = await _portManager.OpenAsync(portName, 5, true, ct);
                if (!opened)
                {
                    _log("[高通] 无法重新打开端口");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                // 创建 Firehose 客户端
                SetState(QualcommConnectionState.FirehoseMode);
                _firehose = new FirehoseClient(_portManager, _log, _progress, _logDetail);

                // ========== VIP 认证 (关键：必须在 Firehose 配置之前执行) ==========
                // 使用文件路径方式发送二进制数据
                bool vipAuthOk = false;
                if (!string.IsNullOrEmpty(digestPath) && !string.IsNullOrEmpty(signaturePath) &&
                    System.IO.File.Exists(digestPath) && System.IO.File.Exists(signaturePath))
                {
                    var digestInfo = new System.IO.FileInfo(digestPath);
                    var sigInfo = new System.IO.FileInfo(signaturePath);
                    _log(string.Format("[高通] 执行 VIP 认证 (Digest={0}B, Sign={1}B)...", digestInfo.Length, sigInfo.Length));
                    
                    // 使用文件路径方式发送
                    vipAuthOk = await _firehose.PerformVipAuthAsync(digestPath, signaturePath, ct);
                    if (!vipAuthOk)
                    {
                        _log("[高通] VIP 认证失败，回退到普通模式...");
                        SetVipAuthState(false);  // 重要：认证失败时使用普通读取模式
                    }
                    else
                    {
                        _log("[高通] VIP 认证成功，已激活高权限模式");
                        SetVipAuthState(true);
                    }
                }
                else
                {
                    // 没有提供认证数据或文件不存在，使用普通模式
                    if (!string.IsNullOrEmpty(digestPath) || !string.IsNullOrEmpty(signaturePath))
                    {
                        _log("[高通] VIP 认证文件不存在，使用普通模式");
                    }
                    SetVipAuthState(false);
                }

                // Firehose 配置
                _log("正在配置 Firehose...");
                bool configOk = await _firehose.ConfigureAsync(storageType, 0, ct);
                if (!configOk)
                {
                    _log("配置 Firehose : 失败");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }
                _log("配置 Firehose : 成功");

                // 保存连接参数
                LastPortName = portName;
                LastStorageType = storageType;

                // 注册端口断开事件
                if (_portManager != null)
                {
                    _portManager.PortDisconnected += (s, e) => HandlePortDisconnected();
                }

                SetState(QualcommConnectionState.Ready);
                _log("[高通] VIP Loader 连接成功");

                return true;
            }
            catch (OperationCanceledException)
            {
                _log("[高通] 连接已取消");
                SetState(QualcommConnectionState.Disconnected);
                return false;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 连接错误 - {0}", ex.Message));
                SetState(QualcommConnectionState.Error);
                return false;
            }
            finally
            {
                if (State != QualcommConnectionState.Ready)
                    Disconnect();
            }
        }

        /// <summary>
        /// [已移除] 使用云端 Loader 数据连接设备 — 保留存根以防外部引用
        /// </summary>
        [System.Obsolete("Cloud loader removed. Use ConnectAsync instead.")]
        public async Task<bool> ConnectWithCloudLoaderAsync(string portName, byte[] loaderData, string storageType = "ufs",
            string authMode = "none", byte[] digestData = null, byte[] signatureData = null, CancellationToken ct = default(CancellationToken))
        {
            _log("[错误] 云端 Loader 功能已移除，请使用本地文件");
            return await Task.FromResult(false);
        }

        /// <summary>
        /// 直接连接 Firehose (跳过 Sahara)
        /// </summary>
        public async Task<bool> ConnectFirehoseDirectAsync(
            string portName,
            string storageType = "ufs",
            string authMode = "none",
            string digestPath = "",
            string signaturePath = "",
            CancellationToken ct = default(CancellationToken))
        {
            try
            {
#if EXCLUDE_REALME_AUTH
                if (string.Equals(authMode, "realme", StringComparison.OrdinalIgnoreCase))
                {
                    _log("[高通] 此构建已排除 Realme 认证");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }
#endif
                Disconnect();
                await Task.Delay(100, ct);

                SetState(QualcommConnectionState.Connecting);
                ResetAuthState();
                _log(string.Format("[高通] 直接连接 Firehose: {0}...", portName));

                _portManager = new SerialPortManager();
                bool opened = await _portManager.OpenAsync(portName, 3, ShouldDiscardFirehoseBufferOnOpen(authMode), ct);
                if (!opened)
                {
                    _log("[高通] 无法打开端口");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }

                SetState(QualcommConnectionState.FirehoseMode);
                _firehose = new FirehoseClient(_portManager, _log, _progress, _logDetail);

                _log("正在配置 Firehose...");
                string authModeLower = (authMode ?? "none").ToLowerInvariant();
#if !EXCLUDE_REALME_AUTH
                if (authModeLower == "realme")
                {
                    bool realmeOk = await ConfigureAndAuthenticateRealmeAsync(storageType, digestPath, ct).ConfigureAwait(false);
                    if (!realmeOk)
                    {
                        _log("配置 Firehose : 失败");
                        SetState(QualcommConnectionState.Error);
                        return false;
                    }

                    _log("配置 Firehose : 成功");
                    LastPortName = portName;
                    LastStorageType = storageType;
                    
                    if (_portManager != null)
                    {
                        _portManager.PortDisconnected += (s, e) => HandlePortDisconnected();
                    }

                    SetState(QualcommConnectionState.Ready);
                    _log("[高通] Firehose 直连成功");
                    return true;
                }
#endif

                bool configOk = await ConfigureFirehoseForAuthModeAsync(storageType, authMode, ct);
                if (!configOk)
                {
                    _log("配置 Firehose : 失败");
                    SetState(QualcommConnectionState.Error);
                    return false;
                }
                _log("配置 Firehose : 成功");

                // 保存连接参数
                LastPortName = portName;
                LastStorageType = storageType;
                
                // 注册端口断开事件
                if (_portManager != null)
                {
                    _portManager.PortDisconnected += (s, e) => HandlePortDisconnected();
                }
                
                if (authModeLower == "oneplus" || authModeLower == "demacia")
                {
                    var oneplus = new OnePlusAuthStrategy(_log);
                    bool authOk = await oneplus.AuthenticateAsync(_firehose, null, ct);
                    if (!authOk)
                    {
                        _log("[高通] OnePlus 认证失败");
                        SetState(QualcommConnectionState.Error);
                        return false;
                    }
                }
#if !EXCLUDE_REALME_AUTH
                else if (authModeLower == "realme")
                {
                    bool authOk = await PerformRealmeCloudAuthAsync(digestPath, ct);
                    if (!authOk)
                    {
                        SetState(QualcommConnectionState.Error);
                        return false;
                    }
                }
#endif

                SetState(QualcommConnectionState.Ready);
                _log("[高通] Firehose 直连成功");
                return true;
            }
            catch (OperationCanceledException)
            {
                _log("[高通] 连接已取消");
                SetState(QualcommConnectionState.Disconnected);
                return false;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 连接错误 - {0}", ex.Message));
                SetState(QualcommConnectionState.Error);
                return false;
            }
            finally
            {
                if (State != QualcommConnectionState.Ready)
                    Disconnect();
            }
        }

        /// <summary>
        /// 断开连接
        /// </summary>
        public void Disconnect()
        {
            if (_portManager != null || _sahara != null || _firehose != null)
                _log("[高通] 断开连接");

            if (_portManager != null)
            {
                _portManager.Close();
                _portManager.Dispose();
                _portManager = null;
            }

            if (_sahara != null)
            {
                _sahara.Dispose();
                _sahara = null;
            }

            if (_firehose != null)
            {
                _firehose.Dispose();
                _firehose = null;
            }

            _partitionCache.Clear();
            ResetAuthState();
            SetState(QualcommConnectionState.Disconnected);
        }
        
        /// <summary>
        /// 获取芯片信息
        /// </summary>
        public QualcommChipInfo GetChipInfo()
        {
            if (_sahara != null && _sahara.ChipInfo != null)
                return _sahara.ChipInfo;
            return null;
        }
        
        /// <summary>
        /// 重置卡住的 Sahara 状态
        /// 当设备因为其他软件或引导错误导致卡在 Sahara 模式时使用
        /// </summary>
        /// <param name="portName">端口名</param>
        /// <param name="ct">取消令牌</param>
        /// <returns>是否成功重置</returns>
        public async Task<bool> ResetSaharaAsync(string portName, CancellationToken ct = default(CancellationToken))
        {
            _log("[高通] 尝试重置卡住的 Sahara 状态...");
            
            try
            {
                // 确保之前的连接已关闭
                Disconnect();
                await Task.Delay(200, ct);
                
                // 打开端口
                _portManager = new SerialPortManager();
                bool opened = await _portManager.OpenAsync(portName, 3, true, ct);
                if (!opened)
                {
                    _log("[高通] 无法打开端口");
                    return false;
                }
                
                // 创建临时 Sahara 客户端
                _sahara = new SaharaClient(_portManager, _log, _logDetail, null);
                
                // 尝试重置
                bool success = await _sahara.TryResetSaharaAsync(ct);
                
                if (success)
                {
                    _log("[高通] ✓ Sahara 状态已重置");
                    _log("[高通] 设备已准备好，请点击[连接]按钮重新连接");
                    
                    // 重置成功后断开连接，让用户可以正常重新连接
                    // 保留端口名以便后续连接
                    string savedPortName = portName;
                    
                    // 关闭当前连接（释放端口资源）
                    if (_portManager != null)
                    {
                        _portManager.Close();
                        _portManager.Dispose();
                        _portManager = null;
                    }
                    if (_sahara != null)
                    {
                        _sahara.Dispose();
                        _sahara = null;
                    }
                    
                    // 设置为断开状态，等待用户重新连接
                    SetState(QualcommConnectionState.Disconnected);
                    LastPortName = savedPortName;  // 保留端口名
                }
                else
                {
                    _log("[高通] ❌ 无法重置 Sahara，请尝试断电重启设备");
                    // 关闭连接
                    Disconnect();
                }
                
                return success;
            }
            catch (Exception ex)
            {
                _log("[高通] 重置 Sahara 异常: " + ex.Message);
                Disconnect();
                return false;
            }
        }
        
        /// <summary>
        /// 硬重置设备 (完全重启)
        /// </summary>
        /// <param name="portName">端口名</param>
        /// <param name="ct">取消令牌</param>
        public async Task<bool> HardResetDeviceAsync(string portName, CancellationToken ct = default(CancellationToken))
        {
            _log("[高通] 发送硬重置命令...");
            
            try
            {
                // 如果已连接 Firehose，通过 Firehose 重置
                if (_firehose != null && State == QualcommConnectionState.Ready)
                {
                    bool ok = await _firehose.ResetAsync("reset", ct);
                    Disconnect();
                    return ok;
                }
                
                // 否则尝试通过 Sahara 重置
                if (_portManager == null || !_portManager.IsOpen)
                {
                    _portManager = new SerialPortManager();
                    await _portManager.OpenAsync(portName, 3, true, ct);
                }
                
                if (_sahara == null)
                {
                    _sahara = new SaharaClient(_portManager, _log, _logDetail, null);
                }
                
                _sahara.SendHardReset();
                _log("[高通] 硬重置命令已发送，设备将重启");
                
                await Task.Delay(500, ct);
                Disconnect();
                return true;
            }
            catch (Exception ex)
            {
                _log("[高通] 硬重置异常: " + ex.Message);
                return false;
            }
        }

        /// <summary>
        /// 执行认证
        /// </summary>
        public async Task<bool> AuthenticateAsync(string authMode, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接 Firehose，无法执行认证");
                return false;
            }

            try
            {
                switch (authMode.ToLowerInvariant())
                {
                    case "oneplus":
                        _log("[高通] 执行 OnePlus 认证...");
                        var oneplusAuth = new Authentication.OnePlusAuthStrategy();
                        // OnePlus 认证不需要外部文件，使用空字符串
                        return await oneplusAuth.AuthenticateAsync(_firehose, "", ct);

                    case "vip":
                    case "oplus":
                        _log("[高通] 执行 VIP/OPPO 认证...");
                        // VIP 认证通常需要签名文件，这里使用默认路径
                        string vipDir = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "vip");
                        string digestPath = System.IO.Path.Combine(vipDir, "digest.bin");
                        string signaturePath = System.IO.Path.Combine(vipDir, "signature.bin");
                        if (!System.IO.File.Exists(digestPath) || !System.IO.File.Exists(signaturePath))
                        {
                            _log("[高通] VIP 认证文件不存在，尝试无签名认证...");
                            // 如果没有签名文件，返回 true 继续（某些设备可能不需要认证）
                            return true;
                        }
                        bool ok = await _firehose.PerformVipAuthAsync(digestPath, signaturePath, ct);
                        if (ok) SetVipAuthState(true);
                        return ok;

                    case "xiaomi":
                        _log("[高通] 执行小米认证...");
                        var xiaomiAuth = new Authentication.XiaomiAuthStrategy(_log);
                        xiaomiAuth.OnAuthTokenRequired += token => XiaomiAuthTokenRequired?.Invoke(token);
                        return await xiaomiAuth.AuthenticateAsync(_firehose, "", ct);

                    default:
                        _log(string.Format("[高通] 未知认证模式: {0}", authMode));
                        return false;
                }
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 认证失败: {0}", ex.Message));
                return false;
            }
        }

        /// <summary>
        /// 执行 OnePlus 认证
        /// </summary>
        public async Task<bool> PerformOnePlusAuthAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接 Firehose，无法执行 OnePlus 认证");
                return false;
            }

            try
            {
                _log("[高通] 执行 OnePlus 认证...");
                var oneplusAuth = new Authentication.OnePlusAuthStrategy(_log);
                bool ok = await oneplusAuth.AuthenticateAsync(_firehose, "", ct);
                if (ok)
                    _log("[高通] OnePlus 认证成功");
                else
                    _log("[高通] OnePlus 认证失败");
                return ok;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] OnePlus 认证异常: {0}", ex.Message));
                return false;
            }
        }

        /// <summary>
        /// 执行小米认证
        /// </summary>
        public async Task<bool> PerformXiaomiAuthAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接 Firehose，无法执行小米认证");
                return false;
            }

            try
            {
                _log("[高通] 执行小米认证...");
                var xiaomiAuth = new Authentication.XiaomiAuthStrategy(_log);
                xiaomiAuth.OnAuthTokenRequired += token => XiaomiAuthTokenRequired?.Invoke(token);
                bool ok = await xiaomiAuth.AuthenticateAsync(_firehose, "", ct);
                if (ok)
                    _log("[高通] 小米认证成功");
                else
                    _log("[高通] 小米认证失败");
                return ok;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 小米认证异常: {0}", ex.Message));
                return false;
            }
        }

        private void SetState(QualcommConnectionState newState)
        {
            if (State != newState)
            {
                State = newState;
                if (StateChanged != null)
                    StateChanged(this, newState);
            }
        }

        #endregion

        #region 自动认证逻辑

        /// <summary>
        /// 自动认证 - 仅对小米设备自动执行
        /// 其他设备 (OnePlus/OPPO/Realme 等) 由用户手动选择认证方式
        /// </summary>
        private async Task<bool> AutoAuthenticateAsync(string programmerPath, CancellationToken ct)
        {
            if (_firehose == null) return true;

            // 只有小米设备自动认证
            if (IsXiaomiDevice())
            {
                _log("[高通] 检测到小米设备，自动执行 MiAuth 认证...");
                try
                {
                    var xiaomi = new XiaomiAuthStrategy(_log);
                    xiaomi.OnAuthTokenRequired += token => XiaomiAuthTokenRequired?.Invoke(token);
                    bool result = await xiaomi.AuthenticateAsync(_firehose, programmerPath, ct);
                    if (result)
                    {
                        _log("[高通] 小米认证成功");
                    }
                    else
                    {
                        _log("[高通] 小米认证失败，设备可能需要官方授权");
                    }
                    return result;
                }
                catch (Exception ex)
                {
                    _log(string.Format("[高通] 小米认证异常: {0}", ex.Message));
                    return false;
                }
            }

            // 其他设备不自动认证，由用户手动选择
            return true;
        }

        /// <summary>
        /// 检测是否为小米设备 (通过 OEM ID 或其他特征)
        /// </summary>
        public bool IsXiaomiDevice()
        {
            if (ChipInfo == null) return false;

            // 通过 OEM ID 检测 (0x0072 = Xiaomi 官方)
            if (ChipInfo.OemId == 0x0072) return true;

            // 通过 PK Hash 前缀检测 (小米常见 PK Hash)
            if (!string.IsNullOrEmpty(ChipInfo.PkHash))
            {
                string pkLower = ChipInfo.PkHash.ToLowerInvariant();
                // 小米设备 PK Hash 前缀列表 (持续更新)
                string[] xiaomiPkHashPrefixes = new[]
                {
                    "c924a35f",  // 常见小米设备
                    "3373d5c8",
                    "e07be28b",
                    "6f5c4e17",
                    "57158eaf",
                    "355d47f9",
                    "a7b8b825",
                    "1c845b80",
                    "58b4add1",
                    "dd0cba2f",
                    "1bebe386"
                };

                foreach (var prefix in xiaomiPkHashPrefixes)
                {
                    if (pkLower.StartsWith(prefix))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        /// <summary>
        /// 手动执行 OPLUS VIP 认证 (基于 Digest 和 Signature)
        /// </summary>
        public async Task<bool> PerformVipAuthManualAsync(string digestPath, string signaturePath, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接设备");
                return false;
            }

            _log("[高通] 启动 OPLUS VIP 认证 (Digest + Sign)...");
            try
            {
                bool result = await _firehose.PerformVipAuthAsync(digestPath, signaturePath, ct);
                if (result)
                {
                    _log("[高通] VIP 认证成功，已进入高权限模式");
                    SetVipAuthState(true); 
                }
                else
                {
                    _log("[高通] VIP 认证失败：校验未通过");
                }
                return result;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] VIP 认证异常: {0}", ex.Message));
                return false;
            }
        }

        /// <summary>
        /// 手动执行 OPLUS VIP 认证 (基于 byte[] 数据)
        /// 支持在发送 Digest 后直接写入签名数据
        /// </summary>
        /// <param name="digestData">Digest 数据 (Hash Segment, ~20-30KB)</param>
        /// <param name="signatureData">签名数据 (256 字节 RSA-2048)</param>
        public async Task<bool> PerformVipAuthAsync(byte[] digestData, byte[] signatureData, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接设备");
                return false;
            }

            _log(string.Format("[高通] 启动 VIP 认证 (Digest={0}B, Sign={1}B)...", 
                digestData?.Length ?? 0, signatureData?.Length ?? 0));
            try
            {
                bool result = await _firehose.PerformVipAuthAsync(digestData, signatureData, ct);
                if (result)
                {
                    _log("[高通] VIP 认证成功，已进入高权限模式");
                    SetVipAuthState(true);
                }
                else
                {
                    _log("[高通] VIP 认证失败：校验未通过");
                }
                return result;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] VIP 认证异常: {0}", ex.Message));
                return false;
            }
        }

        /// <summary>
        /// 分步执行 VIP 认证 - Step 1: 发送 Digest
        /// </summary>
        public async Task<bool> SendVipDigestAsync(byte[] digestData, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return false;
            return await _firehose.SendVipDigestAsync(digestData, ct);
        }

        /// <summary>
        /// 分步执行 VIP 认证 - Step 2-3: 准备 VIP 模式
        /// </summary>
        public async Task<bool> PrepareVipModeAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return false;
            return await _firehose.PrepareVipModeAsync(ct);
        }

        /// <summary>
        /// 分步执行 VIP 认证 - Step 4: 发送签名 (256 字节)
        /// 这是核心方法：在发送 Digest 后写入签名
        /// </summary>
        public async Task<bool> SendVipSignatureAsync(byte[] signatureData, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return false;
            return await _firehose.SendVipSignatureAsync(signatureData, ct);
        }

        /// <summary>
        /// 分步执行 VIP 认证 - Step 5: 完成认证
        /// </summary>
        public async Task<bool> FinalizeVipAuthAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return false;
            return await _firehose.FinalizeVipAuthAsync(ct);
        }

        /// <summary>
        /// 使用嵌入的奇美拉签名数据进行 VIP 认证 (已移除 ChimeraSignDatabase，始终返回 false)
        /// </summary>
        /// <param name="platform">平台代号 (如 SM8550, SM8650 等)</param>
        public async Task<bool> PerformChimeraAuthAsync(string platform, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接设备");
                return false;
            }
            _log("[高通] 奇美拉签名数据库已移除，请使用在线签名或其它方式认证");
            return await Task.FromResult(false);
        }

        /// <summary>
        /// 自动检测平台并使用奇美拉签名认证
        /// </summary>
        public async Task<bool> PerformChimeraAuthAutoAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
            {
                _log("[高通] 未连接设备");
                return false;
            }

            // 尝试从 Sahara 获取的芯片信息
            string platform = null;
            if (_sahara != null && _sahara.ChipInfo != null)
            {
                platform = _sahara.ChipInfo.ChipName;
                if (string.IsNullOrEmpty(platform) || platform == "Unknown")
                {
                    // 尝试从 MSM ID 推断
                    uint msmId = _sahara.ChipInfo.MsmId;
                    platform = QualcommDatabase.GetChipName(msmId);
                }
            }

            if (string.IsNullOrEmpty(platform) || platform == "Unknown")
            {
                _log("[高通] 无法自动检测平台，请手动指定");
                return false;
            }

            _log(string.Format("[高通] 自动检测到平台: {0}", platform));
            return await PerformChimeraAuthAsync(platform, ct);
        }

        /// <summary>
        /// 获取支持的奇美拉平台列表 (ChimeraSignDatabase 已移除，返回空数组)
        /// </summary>
        public string[] GetSupportedChimeraPlatforms()
        {
            return Array.Empty<string>();
        }

        /// <summary>
        /// 检查平台是否支持奇美拉签名 (ChimeraSignDatabase 已移除，始终返回 false)
        /// </summary>
        public bool IsChimeraSupported(string platform)
        {
            return false;
        }

        /// <summary>
        /// 获取设备挑战码 (用于在线签名)
        /// </summary>
        public async Task<string> GetVipChallengeAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return null;
            return await _firehose.GetVipChallengeAsync(ct);
        }

        #endregion

        #region 分区操作

        /// <summary>
        /// 读取所有 LUN 的 GPT 分区表
        /// </summary>
        public async Task<List<PartitionInfo>> ReadAllGptAsync(int maxLuns = 6, CancellationToken ct = default(CancellationToken))
        {
            return await ReadAllGptAsync(maxLuns, null, null, ct);
        }

        /// <summary>
        /// 读取所有 LUN 的 GPT 分区表（带进度回调）
        /// </summary>
        /// <param name="maxLuns">最大 LUN 数量</param>
        /// <param name="totalProgress">总进度回调 (当前LUN, 总LUN)</param>
        /// <param name="subProgress">子进度回调 (0-100)</param>
        /// <param name="ct">取消令牌</param>
        public async Task<List<PartitionInfo>> ReadAllGptAsync(
            int maxLuns, 
            IProgress<Tuple<int, int>> totalProgress,
            IProgress<double> subProgress,
            CancellationToken ct = default(CancellationToken),
            IProgress<int> lunStartProgress = null)
        {
            var allPartitions = new List<PartitionInfo>();

            if (_firehose == null)
                return allPartitions;

            _logDetail("正在读取 GUID 分区表...");

            // 报告开始
            if (totalProgress != null) totalProgress.Report(Tuple.Create(0, maxLuns));
            if (subProgress != null) subProgress.Report(0);

            // LUN 完成回调 - 推进主进度条
            var lunProgress = new Progress<int>(lun => {
                if (totalProgress != null) totalProgress.Report(Tuple.Create(lun, maxLuns));
            });

            var partitions = await _firehose.ReadGptPartitionsAsync(IsVipDevice, ct, lunProgress, lunStartProgress);
            
            // 报告中间进度
            if (subProgress != null) subProgress.Report(80);
            
            if (partitions != null && partitions.Count > 0)
            {
                allPartitions.AddRange(partitions);
                _logDetail(string.Format("读取 GUID 分区表 : 成功 [{0}]", partitions.Count));

                // 缓存分区
                _partitionCache.Clear();
                foreach (var p in partitions)
                {
                    if (!_partitionCache.ContainsKey(p.Lun))
                        _partitionCache[p.Lun] = new List<PartitionInfo>();
                    _partitionCache[p.Lun].Add(p);
                }
            }

            // 报告完成
            if (subProgress != null) subProgress.Report(100);
            if (totalProgress != null) totalProgress.Report(Tuple.Create(maxLuns, maxLuns));

            _logDetail(string.Format("[高通] 共发现 {0} 个分区", allPartitions.Count));
            return allPartitions;
        }

        /// <summary>
        /// 获取指定 LUN 的分区列表
        /// </summary>
        public List<PartitionInfo> GetCachedPartitions(int lun = -1)
        {
            var result = new List<PartitionInfo>();

            if (lun == -1)
            {
                foreach (var kv in _partitionCache)
                    result.AddRange(kv.Value);
            }
            else
            {
                List<PartitionInfo> list;
                if (_partitionCache.TryGetValue(lun, out list))
                    result.AddRange(list);
            }

            return result;
        }

        /// <summary>
        /// 查找分区
        /// </summary>
        public PartitionInfo FindPartition(string name)
        {
            foreach (var kv in _partitionCache)
            {
                foreach (var p in kv.Value)
                {
                    if (string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
                        return p;
                }
            }
            return null;
        }

        /// <summary>
        /// 读取分区到文件
        /// </summary>
        public async Task<bool> ReadPartitionAsync(string partitionName, string outputPath, IProgress<double> progress = null, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            var partition = FindPartition(partitionName);
            if (partition == null)
            {
                _log("[高通] 未找到分区 " + partitionName);
                return false;
            }

            _log(string.Format("[高通] 读取分区 {0} ({1})", partitionName, partition.FormattedSize));

            try
            {
                int sectorsPerChunk = _firehose.MaxPayloadSize / partition.SectorSize;
                long totalSectors = partition.NumSectors;
                long readSectors = 0;
                long totalBytes = partition.Size;
                long readBytes = 0;

                // 使用异步文件流，避免阻塞
                using (var fs = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 4 * 1024 * 1024, FileOptions.Asynchronous))
                {
                    while (readSectors < totalSectors && !ct.IsCancellationRequested)
                    {
                        int toRead = (int)Math.Min(sectorsPerChunk, totalSectors - readSectors);
                        // ConfigureAwait(false) 避免回到 UI 线程
                        byte[] data = await _firehose.ReadSectorsAsync(
                            partition.Lun, partition.StartSector + readSectors, toRead, ct, IsVipDevice, partitionName).ConfigureAwait(false);

                        if (data == null)
                        {
                            _log("[高通] 读取失败");
                            return false;
                        }

                        // 使用异步写入
                        await fs.WriteAsync(data, 0, data.Length, ct).ConfigureAwait(false);
                        readSectors += toRead;
                        readBytes += data.Length;

                        // 调用字节级进度回调 (用于速度计算)
                        _firehose.ReportProgress(readBytes, totalBytes);

                        // 百分比进度 (使用 double)
                        if (progress != null)
                            progress.Report(100.0 * readBytes / totalBytes);
                    }
                }

                _log(string.Format("[高通] 分区 {0} 已保存到 {1}", partitionName, outputPath));
                return true;
            }
            catch (Exception ex)
            {
                _log(string.Format("[高通] 读取错误 - {0}", ex.Message));
                return false;
            }
        }

        /// <summary>
        /// 写入分区
        /// </summary>
        public async Task<bool> WritePartitionAsync(string partitionName, string filePath, IProgress<double> progress = null, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            var partition = FindPartition(partitionName);
            if (partition == null)
            {
                _log("[高通] 未找到分区 " + partitionName);
                return false;
            }

            // OPLUS/VIP 某些分区需要 SHA256 校验环绕，Realme 不走这条写入链路
            bool useSha256 = IsOplusDevice && (partitionName.ToLower() == "xbl" || partitionName.ToLower() == "abl" || partitionName.ToLower() == "imagefv");
            if (useSha256) await _firehose.Sha256InitAsync(ct).ConfigureAwait(false);

            // VIP 设备使用伪装模式写入
            // ConfigureAwait(false) 避免回到 UI 线程，提高 IO 性能
            bool success = await _firehose.FlashPartitionFromFileAsync(
                partitionName, filePath, partition.Lun, partition.StartSector, progress, ct, IsVipDevice).ConfigureAwait(false);

            if (useSha256) await _firehose.Sha256FinalAsync(ct).ConfigureAwait(false);

            return success;
        }

        private bool IsOplusDevice 
        { 
            get { 
                if (IsVipDevice) return true;
                if (ChipInfo != null && (ChipInfo.Vendor == "OPPO" || ChipInfo.Vendor == "OnePlus")) return true;
                return false;
            } 
        }

        /// <summary>
        /// 直接写入指定 LUN 和 StartSector (用于 PrimaryGPT/BackupGPT 等特殊分区)
        /// 支持官方 NUM_DISK_SECTORS-N 负扇区格式
        /// </summary>
        public async Task<bool> WriteDirectAsync(string label, string filePath, int lun, long startSector, IProgress<double> progress = null, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            // 负扇区使用官方格式直接发送给设备 (不依赖客户端 GPT 缓存)
            if (startSector < 0)
            {
                _logDetail(string.Format("[高通] 写入: {0} -> LUN{1} @ NUM_DISK_SECTORS{2}", label, lun, startSector));
                
                // 使用官方 NUM_DISK_SECTORS-N 格式，让设备计算绝对地址
                // ConfigureAwait(false) 避免回到 UI 线程
                return await _firehose.FlashPartitionWithNegativeSectorAsync(
                    label, filePath, lun, startSector, progress, ct).ConfigureAwait(false);
            }
            else
            {
                _logDetail(string.Format("[高通] 写入: {0} -> LUN{1} @ sector {2}", label, lun, startSector));

                // 正数扇区正常写入
                // ConfigureAwait(false) 避免回到 UI 线程
                return await _firehose.FlashPartitionFromFileAsync(
                    label, filePath, lun, startSector, progress, ct, IsVipDevice).ConfigureAwait(false);
            }
        }

        /// <summary>
        /// 擦除分区
        /// </summary>
        public async Task<bool> ErasePartitionAsync(string partitionName, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            var partition = FindPartition(partitionName);
            if (partition == null)
            {
                _log("[高通] 未找到分区 " + partitionName);
                return false;
            }

            // VIP 设备使用伪装模式擦除
            // ConfigureAwait(false) 避免回到 UI 线程
            return await _firehose.ErasePartitionAsync(partition, ct, IsVipDevice).ConfigureAwait(false);
        }

        /// <summary>
        /// 读取分区指定偏移处的数据
        /// </summary>
        /// <param name="partitionName">分区名称</param>
        /// <param name="offset">偏移 (字节)</param>
        /// <param name="size">大小 (字节)</param>
        /// <param name="ct">取消令牌</param>
        /// <returns>读取的数据</returns>
        public async Task<byte[]> ReadPartitionDataAsync(string partitionName, long offset, int size, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return null;

            var partition = FindPartition(partitionName);
            if (partition == null)
            {
                _log("[高通] 未找到分区 " + partitionName);
                return null;
            }

            // 计算扇区位置
            int sectorSize = SectorSize > 0 ? SectorSize : 4096;
            long startSector = partition.StartSector + (offset / sectorSize);
            int numSectors = (size + sectorSize - 1) / sectorSize;

            // 只有 VIP 认证成功后才使用 VIP 模式读取
            // IsVipDevice = true 表示 VIP 认证已成功
            // IsOplusDevice 只用于判断是否需要 SHA256 校验，不用于读取模式
            bool useVipMode = IsVipDevice;

            // 读取数据
            byte[] data = await _firehose.ReadSectorsAsync(partition.Lun, startSector, numSectors, ct, useVipMode, partitionName);
            if (data == null) return null;

            // 如果有偏移对齐问题，截取正确的数据
            int offsetInSector = (int)(offset % sectorSize);
            if (offsetInSector > 0 || data.Length > size)
            {
                int actualSize = Math.Min(size, data.Length - offsetInSector);
                if (actualSize <= 0) return null;
                
                byte[] result = new byte[actualSize];
                Array.Copy(data, offsetInSector, result, 0, actualSize);
                return result;
            }

            return data;
        }

        /// <summary>
        /// 获取 Firehose 客户端 (供内部使用)
        /// </summary>
        internal Protocol.FirehoseClient GetFirehoseClient()
        {
            return _firehose;
        }

        #endregion

        #region 设备控制

        /// <summary>
        /// 重启设备
        /// </summary>
        public async Task<bool> RebootAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            bool result = await _firehose.ResetAsync("reset", ct);
            if (result)
                Disconnect();

            return result;
        }

        /// <summary>
        /// 关机
        /// </summary>
        public async Task<bool> PowerOffAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            bool result = await _firehose.PowerOffAsync(ct);
            if (result)
                Disconnect();

            return result;
        }

        /// <summary>
        /// 重启到 EDL 模式
        /// </summary>
        public async Task<bool> RebootToEdlAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            bool result = await _firehose.RebootToEdlAsync(ct);
            if (result)
                Disconnect();

            return result;
        }

        /// <summary>
        /// 设置活动 Slot
        /// </summary>
        public async Task<bool> SetActiveSlotAsync(string slot, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            return await _firehose.SetActiveSlotAsync(slot, ct);
        }

        /// <summary>
        /// 修复 GPT
        /// </summary>
        public async Task<bool> FixGptAsync(int lun = -1, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            return await _firehose.FixGptAsync(lun, true, ct);
        }

        /// <summary>
        /// 设置启动 LUN
        /// </summary>
        public async Task<bool> SetBootLunAsync(int lun, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            return await _firehose.SetBootLunAsync(lun, ct);
        }

        /// <summary>
        /// Ping 测试连接
        /// </summary>
        public async Task<bool> PingAsync(CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            return await _firehose.PingAsync(ct);
        }

        /// <summary>
        /// 应用 Patch XML 文件
        /// </summary>
        public async Task<int> ApplyPatchXmlAsync(string patchXmlPath, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return 0;

            return await _firehose.ApplyPatchXmlAsync(patchXmlPath, ct);
        }

        /// <summary>
        /// 应用多个 Patch XML 文件
        /// </summary>
        public async Task<int> ApplyPatchFilesAsync(IEnumerable<string> patchFiles, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return 0;

            int totalPatches = 0;
            foreach (var patchFile in patchFiles)
            {
                if (ct.IsCancellationRequested) break;
                totalPatches += await _firehose.ApplyPatchXmlAsync(patchFile, ct);
            }
            return totalPatches;
        }

        #endregion

        #region 批量刷写

        /// <summary>
        /// 批量刷写分区
        /// </summary>
        public async Task<bool> FlashMultipleAsync(IEnumerable<FlashPartitionInfo> partitions, IProgress<double> progress = null, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null)
                return false;

            var list = new List<FlashPartitionInfo>(partitions);
            int total = list.Count;
            int current = 0;
            bool allSuccess = true;

            foreach (var p in list)
            {
                if (ct.IsCancellationRequested)
                    break;

                _log(string.Format("[高通] 刷写 [{0}/{1}] {2}", current + 1, total, p.Name));

                bool ok = await WritePartitionAsync(p.Name, p.Filename, null, ct);
                if (!ok)
                {
                    allSuccess = false;
                    _log("[高通] 刷写失败 - " + p.Name);
                }

                current++;
                if (progress != null)
                    progress.Report(100.0 * current / total);
            }

            return allSuccess;
        }

        #endregion

        #region Diag 诊断功能
        
        /// <summary>
        /// 连接到 Diag 诊断端口
        /// </summary>
        public async Task<bool> ConnectDiagAsync(string portName, int baudRate = 115200)
        {
            try
            {
                if (_diagClient == null)
                    _diagClient = new DiagClient();
                
                _log($"[高通] 正在连接诊断端口 {portName}...");
                var result = await _diagClient.ConnectAsync(portName, baudRate);
                
                if (result)
                    _log("[高通] 诊断端口连接成功");
                else
                    _log("[高通] 诊断端口连接失败");
                
                return result;
            }
            catch (Exception ex)
            {
                _log($"[高通] 诊断端口连接异常: {ex.Message}");
                return false;
            }
        }
        
        /// <summary>
        /// 断开 Diag 诊断连接
        /// </summary>
        public void DisconnectDiag()
        {
            _diagClient?.Disconnect();
            _diagClient?.Dispose();
            _diagClient = null;
        }
        
        /// <summary>
        /// 发送 SPC 解锁
        /// </summary>
        public async Task<bool> SendSpcAsync(string spc = "000000")
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            _log("[高通] 正在发送 SPC 解锁...");
            var result = await _diagClient.SendSpcAsync(spc);
            _log(result ? "[高通] SPC 解锁成功" : "[高通] SPC 解锁失败");
            return result;
        }
        
        /// <summary>
        /// 读取 IMEI
        /// </summary>
        public async Task<string> ReadDiagImeiAsync(int slot = 1)
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return null;
            }
            
            _log($"[高通] 正在读取 IMEI (Slot {slot})...");
            var imei = await _diagClient.ReadImeiAsync(slot);
            
            if (!string.IsNullOrEmpty(imei))
                _log($"[高通] IMEI{slot}: {imei}");
            else
                _log($"[高通] 读取 IMEI{slot} 失败");
            
            return imei;
        }
        
        /// <summary>
        /// 写入 IMEI
        /// </summary>
        public async Task<bool> WriteDiagImeiAsync(string imei, int slot = 1)
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            if (string.IsNullOrEmpty(imei) || imei.Length != 15)
            {
                _log("[高通] IMEI 格式错误，必须为 15 位数字");
                return false;
            }
            
            _log($"[高通] 正在写入 IMEI (Slot {slot}): {imei}...");
            var result = await _diagClient.WriteImeiAsync(imei, slot);
            _log(result ? "[高通] IMEI 写入成功" : "[高通] IMEI 写入失败");
            return result;
        }
        
        /// <summary>
        /// 读取所有 IMEI
        /// </summary>
        public async Task<ImeiInfo> ReadAllDiagImeiAsync()
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return null;
            }
            
            _log("[高通] 正在读取所有 IMEI...");
            var info = await _diagClient.ReadAllImeiAsync();
            
            if (!string.IsNullOrEmpty(info?.Imei1))
                _log($"[高通] IMEI1: {info.Imei1}");
            if (!string.IsNullOrEmpty(info?.Imei2))
                _log($"[高通] IMEI2: {info.Imei2}");
            
            return info;
        }
        
        /// <summary>
        /// 读取 MEID
        /// </summary>
        public async Task<string> ReadDiagMeidAsync()
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return null;
            }
            
            _log("[高通] 正在读取 MEID...");
            var meid = await _diagClient.ReadMeidAsync();
            
            if (!string.IsNullOrEmpty(meid))
                _log($"[高通] MEID: {meid}");
            else
                _log("[高通] 读取 MEID 失败");
            
            return meid;
        }
        
        /// <summary>
        /// 读取 QCN 文件
        /// </summary>
        public async Task<bool> ReadQcnAsync(string filePath, IProgress<int> progress = null)
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            _log($"[高通] 正在读取 QCN 到 {filePath}...");
            var result = await _diagClient.ReadQcnAsync(filePath, progress);
            _log(result ? "[高通] QCN 读取成功" : "[高通] QCN 读取失败");
            return result;
        }
        
        /// <summary>
        /// 写入 QCN 文件
        /// </summary>
        public async Task<bool> WriteQcnAsync(string filePath, IProgress<int> progress = null)
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            if (!File.Exists(filePath))
            {
                _log($"[高通] QCN 文件不存在: {filePath}");
                return false;
            }
            
            _log($"[高通] 正在写入 QCN: {filePath}...");
            var result = await _diagClient.WriteQcnAsync(filePath, progress);
            _log(result ? "[高通] QCN 写入成功" : "[高通] QCN 写入失败");
            return result;
        }
        
        /// <summary>
        /// 通过 Diag 切换到下载模式 (EDL)
        /// </summary>
        public async Task<bool> SwitchToEdlModeAsync()
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            _log("[高通] 正在切换到下载模式 (EDL)...");
            var result = await _diagClient.SwitchToDownloadModeAsync();
            _log(result ? "[高通] 切换成功，设备即将进入 EDL" : "[高通] 切换失败");
            return result;
        }
        
        /// <summary>
        /// 通过 Diag 重启设备
        /// </summary>
        public async Task<bool> RebootDeviceAsync()
        {
            if (_diagClient == null || !_diagClient.IsConnected)
            {
                _log("[高通] 诊断端口未连接");
                return false;
            }
            
            _log("[高通] 正在重启设备...");
            var result = await _diagClient.RebootAsync();
            return result;
        }
        
        #endregion

        #region Loader 功能检测
        
        /// <summary>
        /// 获取 Loader 功能特性
        /// </summary>
        public LoaderFeatures LoaderFeatures => _loaderFeatures;
        
        /// <summary>
        /// 检测 Loader 功能
        /// </summary>
        public LoaderFeatures DetectLoaderFeatures(byte[] loaderData)
        {
            if (_loaderDetector == null)
                _loaderDetector = new LoaderFeatureDetector();
            
            _loaderFeatures = _loaderDetector.DetectFeatures(loaderData);
            
            if (_loaderFeatures != null)
            {
                _log("[高通] Loader 功能检测完成:");
                _log($"  芯片: {_loaderFeatures.ChipName ?? "未知"}");
                _log($"  存储: {_loaderFeatures.RecommendedMemoryType}");
                _log($"  受限: {_loaderFeatures.IsRestricted}");
                _log($"  功能: {string.Join(", ", _loaderFeatures.GetSupportedFeatures())}");
                
                if (_loaderFeatures.IsXiaomi)
                {
                    _log($"  [小米] EDL 验证: {_loaderFeatures.XiaomiEdlVerification}");
                    _log($"  [小米] 可利用漏洞: {_loaderFeatures.ExploitPossible}");
                }
            }
            
            return _loaderFeatures;
        }
        
        /// <summary>
        /// 从文件检测 Loader 功能
        /// </summary>
        public LoaderFeatures DetectLoaderFeaturesFromFile(string loaderPath)
        {
            if (!File.Exists(loaderPath))
            {
                _log($"[高通] Loader 文件不存在: {loaderPath}");
                return null;
            }
            
            var loaderData = File.ReadAllBytes(loaderPath);
            return DetectLoaderFeatures(loaderData);
        }
        
        /// <summary>
        /// 验证 Loader 是否有效
        /// </summary>
        public bool IsValidLoader(byte[] loaderData)
        {
            return LoaderFeatureDetector.IsValidLoader(loaderData);
        }
        
        #endregion

        #region Motorola 支持
        
        /// <summary>
        /// 检查是否为 Motorola 固件包
        /// </summary>
        public bool IsMotorolaPackage(string filePath)
        {
            return MotorolaSupport.IsMotorolaPackage(filePath);
        }
        
        /// <summary>
        /// 解析 Motorola 固件包
        /// </summary>
        public async Task<MotorolaPackageInfo> ParseMotorolaPackageAsync(string filePath)
        {
            if (_motorolaSupport == null)
            {
                _motorolaSupport = new MotorolaSupport();
                _motorolaSupport.OnLog += msg => _log($"[Motorola] {msg}");
            }
            
            _log($"[高通] 正在解析 Motorola 固件包: {Path.GetFileName(filePath)}...");
            return await _motorolaSupport.ParsePackageAsync(filePath);
        }
        
        /// <summary>
        /// 提取 Motorola 固件包
        /// </summary>
        public async Task<string> ExtractMotorolaPackageAsync(string filePath, string outputDir = null, IProgress<int> progress = null)
        {
            if (_motorolaSupport == null)
            {
                _motorolaSupport = new MotorolaSupport();
                _motorolaSupport.OnLog += msg => _log($"[Motorola] {msg}");
            }
            
            if (progress != null)
                _motorolaSupport.OnProgress += percent => progress.Report(percent);
            
            _log($"[高通] 正在提取 Motorola 固件包: {Path.GetFileName(filePath)}...");
            var result = await _motorolaSupport.ExtractPackageAsync(filePath, outputDir);
            _log($"[高通] 提取完成: {result}");
            return result;
        }
        
        #endregion

        #region IDisposable

        public void Dispose()
        {
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (!_disposed)
            {
                if (disposing)
                {
                    Disconnect();
                    DisconnectDiag();
                }
                _disposed = true;
            }
        }

        ~QualcommService()
        {
            Dispose(false);
        }

        #endregion
        /// <summary>
        /// 刷写 OPLUS 固件包中的 Super 逻辑分区 (拆解写入)
        /// </summary>
        public async Task<bool> FlashOplusSuperAsync(string firmwareRoot, string nvId = "", IProgress<double> progress = null, CancellationToken ct = default(CancellationToken))
        {
            if (_firehose == null) return false;

            // 1. 查找 super 分区信息
            var superPart = FindPartition("super");
            if (superPart == null)
            {
                _log("[高通] 未在设备上找到 super 分区");
                return false;
            }

            // 2. 准备任务
            _log("[高通] 正在解析 OPLUS 固件 Super 布局...");
            string activeSlot = CurrentSlot;
            if (activeSlot == "nonexistent" || string.IsNullOrEmpty(activeSlot))
                activeSlot = "a";

            // 计算 super 分区总大小 (用于校验)
            long superPartitionSize = superPart.Size;
            _log(string.Format("[高通] Super 分区: 起始扇区={0}, 大小={1} MB", superPart.StartSector, superPartitionSize / 1024 / 1024));

            var tasks = await _oplusSuperManager.PrepareSuperTasksAsync(
                firmwareRoot, superPart.StartSector, (int)superPart.SectorSize, 
                activeSlot, nvId, superPartitionSize);
            
            if (tasks.Count == 0)
            {
                _log("[高通] 未找到可用的 Super 逻辑分区镜像");
                return false;
            }
            
            // 3. 校验任务
            var validation = _oplusSuperManager.ValidateTasks(tasks, superPartitionSize, (int)superPart.SectorSize);
            if (!validation.IsValid)
            {
                foreach (var err in validation.Errors)
                {
                    _log(string.Format("[MetaSuper] 错误: {0}", err));
                }
                _log("[高通] Super 刷写校验失败，已中止");
                return false;
            }
            
            // 显示警告但继续
            foreach (var warn in validation.Warnings)
            {
                _log(string.Format("[MetaSuper] 警告: {0}", warn));
            }

            // 4. 执行任务
            long totalBytes = tasks.Sum(t => t.SizeInBytes);
            long totalWritten = 0;

            _log(string.Format("[高通] 开始拆解写入 {0} 个逻辑镜像 (总计: {1} MB)...", tasks.Count, totalBytes / 1024 / 1024));

            foreach (var task in tasks)
            {
                if (ct.IsCancellationRequested) break;

                _log(string.Format("[高通] 写入 {0} [{1}] 到物理扇区 {2}...", task.PartitionName, Path.GetFileName(task.FilePath), task.PhysicalSector));
                
                // 嵌套进度计算
                var taskProgress = new Progress<double>(p => {
                    if (progress != null)
                    {
                        double currentTaskWeight = (double)task.SizeInBytes / totalBytes;
                        double overallPercent = ((double)totalWritten / totalBytes * 100) + (p * currentTaskWeight);
                        progress.Report(overallPercent);
                    }
                });

                bool success = await _firehose.FlashPartitionFromFileAsync(
                    task.PartitionName, 
                    task.FilePath, 
                    superPart.Lun, 
                    task.PhysicalSector, 
                    taskProgress, 
                    ct, 
                    IsVipDevice);

                if (!success)
                {
                    _log(string.Format("[高通] 写入 {0} 失败，流程中止", task.PartitionName));
                    return false;
                }

                totalWritten += task.SizeInBytes;
            }

            _log("[高通] OPLUS Super 拆解写入完成");
            return true;
        }
    }
}
