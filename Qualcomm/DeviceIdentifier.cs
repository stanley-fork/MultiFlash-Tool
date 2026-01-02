using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace OPFlashTool.Qualcomm
{
    /// <summary>
    /// 设备识别结果
    /// </summary>
    public class DeviceIdentifyResult
    {
        public string ChipName { get; set; } = "Unknown";
        public string Vendor { get; set; } = "Unknown";
        public string Model { get; set; } = "Unknown";
        public string LoaderName { get; set; } = "";
        public string LoaderUrl { get; set; } = "";
        public string StorageType { get; set; } = "ufs";
        public int SaharaVersion { get; set; } = 2;
        public string RecommendedStrategy { get; set; } = "Standard";
        public bool RequiresAuth { get; set; } = false;
        public string AuthType { get; set; } = ""; // "vip", "miauth", "samsung_odin"
        public Dictionary<string, string> ExtraInfo { get; set; } = new Dictionary<string, string>();

        public override string ToString()
        {
            return $"{Vendor} {Model} ({ChipName}) - {RecommendedStrategy}";
        }
    }

    /// <summary>
    /// 设备自动识别器
    /// 支持本地数据库和云端查询
    /// </summary>
    public class DeviceIdentifier
    {
        private Action<string> _log;
        private static readonly HttpClient _httpClient = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
        
        // 云端 API 端点 (可配置)
        public string CloudApiEndpoint { get; set; } = "";
        
        // 是否启用云端查询
        public bool EnableCloudLookup { get; set; } = true;

        // 本地缓存 (PK Hash -> 识别结果)
        private static readonly Dictionary<string, DeviceIdentifyResult> _localCache = new Dictionary<string, DeviceIdentifyResult>();

        public DeviceIdentifier(Action<string> logger = null)
        {
            _log = logger ?? Console.WriteLine;
        }

        #region 主要识别方法

        /// <summary>
        /// 根据 Sahara PBL 信息识别设备
        /// </summary>
        public async Task<DeviceIdentifyResult> IdentifyDeviceAsync(SaharaPblInfo pblInfo)
        {
            if (pblInfo == null)
                return CreateUnknownResult();

            // 1. 解析 MSM ID
            uint msmId = 0;
            if (!string.IsNullOrEmpty(pblInfo.MsmId))
            {
                uint.TryParse(pblInfo.MsmId.Replace("0x", ""), 
                    System.Globalization.NumberStyles.HexNumber, null, out msmId);
            }

            // 2. 解析 PK Hash
            byte[] pkHash = null;
            if (!string.IsNullOrEmpty(pblInfo.PkHash))
            {
                pkHash = HexStringToBytes(pblInfo.PkHash);
            }

            return await IdentifyDeviceAsync(msmId, pkHash, pblInfo.Serial);
        }

        /// <summary>
        /// 根据 MSM ID 和 PK Hash 识别设备
        /// </summary>
        public async Task<DeviceIdentifyResult> IdentifyDeviceAsync(uint msmId, byte[] pkHash, string serial = null)
        {
            var result = new DeviceIdentifyResult();

            // 1. 从 MSM ID 获取芯片信息
            result.ChipName = QualcommDatabase.GetChipName(msmId);
            result.StorageType = QualcommDatabase.GetMemoryType(result.ChipName) == MemoryType.Ufs ? "ufs" : "emmc";
            result.SaharaVersion = QualcommDatabase.GetSaharaVersion(result.ChipName);

            // 2. 尝试本地数据库匹配
            if (pkHash != null && pkHash.Length >= 4)
            {
                string pkHashKey = GetPkHashKey(pkHash);

                // 检查本地缓存
                if (_localCache.TryGetValue(pkHashKey, out var cached))
                {
                    _log($"[识别] 命中本地缓存: {cached.Vendor} {cached.Model}");
                    MergeResult(result, cached);
                    return result;
                }

                // 查询本地数据库
                var localResult = IdentifyFromLocalDatabase(msmId, pkHash);
                if (localResult.Vendor != "Unknown")
                {
                    _log($"[识别] 本地数据库匹配: {localResult.Vendor} {localResult.Model}");
                    MergeResult(result, localResult);
                    _localCache[pkHashKey] = result;
                    return result;
                }

                // 3. 云端查询 (如果启用)
                if (EnableCloudLookup && !string.IsNullOrEmpty(CloudApiEndpoint))
                {
                    try
                    {
                        var cloudResult = await QueryCloudDatabaseAsync(msmId, pkHash, serial);
                        if (cloudResult != null && cloudResult.Vendor != "Unknown")
                        {
                            _log($"[识别] 云端识别成功: {cloudResult.Vendor} {cloudResult.Model}");
                            MergeResult(result, cloudResult);
                            _localCache[pkHashKey] = result;
                            return result;
                        }
                    }
                    catch (Exception ex)
                    {
                        _log($"[识别] 云端查询失败: {ex.Message}");
                    }
                }

                // 4. 基于 PK Hash 特征推断
                var inferredResult = InferFromPkHash(pkHash);
                if (inferredResult.Vendor != "Unknown")
                {
                    _log($"[识别] PK Hash 特征推断: {inferredResult.Vendor}");
                    MergeResult(result, inferredResult);
                }
            }

            // 5. 设置推荐策略
            DetermineRecommendedStrategy(result);

            return result;
        }

        #endregion

        #region 本地数据库查询

        /// <summary>
        /// 从本地数据库识别设备
        /// </summary>
        private DeviceIdentifyResult IdentifyFromLocalDatabase(uint msmId, byte[] pkHash)
        {
            var result = new DeviceIdentifyResult();

            // 使用 QualcommDatabase 的 PK Hash 数据库
            var (vendor, model, loaderHint) = QualcommDatabase.GetDeviceByPkHash(pkHash);
            
            result.Vendor = vendor;
            result.Model = model;
            result.LoaderName = loaderHint;

            // 设置认证类型
            if (QualcommDatabase.IsVipDevice(pkHash))
            {
                result.RequiresAuth = true;
                result.AuthType = "vip";
                result.RecommendedStrategy = "OppoVip";
            }
            else if (QualcommDatabase.IsXiaomiDevice(pkHash))
            {
                result.RequiresAuth = true;
                result.AuthType = "miauth";
                result.RecommendedStrategy = "Xiaomi";
            }

            return result;
        }

        /// <summary>
        /// 基于 PK Hash 特征推断厂商
        /// </summary>
        private DeviceIdentifyResult InferFromPkHash(byte[] pkHash)
        {
            var result = new DeviceIdentifyResult();
            
            if (pkHash == null || pkHash.Length < 32)
                return result;

            // 已知的 PK Hash 前缀特征
            string hashHex = BitConverter.ToString(pkHash).Replace("-", "").ToUpper();
            
            // OPPO/OnePlus/Realme 特征
            if (hashHex.StartsWith("3C9014") || hashHex.StartsWith("4B4E3B") || 
                hashHex.StartsWith("5E4E9C") || hashHex.StartsWith("71A2B3") ||
                hashHex.Contains("OPPO") || hashHex.Contains("4F50504F"))
            {
                result.Vendor = "OPPO";
                result.RequiresAuth = true;
                result.AuthType = "vip";
                result.RecommendedStrategy = "OppoVip";
            }
            // 小米特征
            else if (hashHex.StartsWith("D40EBB") || hashHex.StartsWith("E8F2D9") ||
                     hashHex.Contains("XIAOMI") || hashHex.Contains("5849414F4D49"))
            {
                result.Vendor = "Xiaomi";
                result.RequiresAuth = true;
                result.AuthType = "miauth";
                result.RecommendedStrategy = "Xiaomi";
            }
            // 三星特征
            else if (hashHex.StartsWith("534D53") || hashHex.Contains("SAMSUNG"))
            {
                result.Vendor = "Samsung";
                result.RecommendedStrategy = "Standard";
            }
            // 华为特征
            else if (hashHex.StartsWith("485741") || hashHex.Contains("HUAWEI"))
            {
                result.Vendor = "Huawei";
                result.RecommendedStrategy = "Standard";
            }
            // vivo 特征
            else if (hashHex.StartsWith("5649564F") || hashHex.Contains("VIVO"))
            {
                result.Vendor = "vivo";
                result.RecommendedStrategy = "Standard";
            }

            return result;
        }

        #endregion

        #region 云端查询

        /// <summary>
        /// 查询云端设备数据库
        /// </summary>
        private async Task<DeviceIdentifyResult> QueryCloudDatabaseAsync(uint msmId, byte[] pkHash, string serial)
        {
            if (string.IsNullOrEmpty(CloudApiEndpoint))
                return null;

            try
            {
                string pkHashHex = pkHash != null ? BitConverter.ToString(pkHash).Replace("-", "") : "";
                
                var requestData = new
                {
                    msm_id = msmId.ToString("X8"),
                    pk_hash = pkHashHex,
                    serial = serial ?? "",
                    version = "1.0"
                };

                string json = JsonSerializer.Serialize(requestData);
                var content = new StringContent(json, Encoding.UTF8, "application/json");

                var response = await _httpClient.PostAsync(CloudApiEndpoint + "/identify", content);
                
                if (response.IsSuccessStatusCode)
                {
                    string responseJson = await response.Content.ReadAsStringAsync();
                    return JsonSerializer.Deserialize<DeviceIdentifyResult>(responseJson, 
                        new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                }
            }
            catch (Exception ex)
            {
                _log($"[云端] 查询异常: {ex.Message}");
            }

            return null;
        }

        /// <summary>
        /// 上报未知设备信息到云端 (用于扩展数据库)
        /// </summary>
        public async Task ReportUnknownDeviceAsync(SaharaPblInfo pblInfo, string userNote = "")
        {
            if (string.IsNullOrEmpty(CloudApiEndpoint) || pblInfo == null)
                return;

            try
            {
                var reportData = new
                {
                    msm_id = pblInfo.MsmId,
                    pk_hash = pblInfo.PkHash,
                    serial = pblInfo.Serial,
                    oem_id = pblInfo.OemId,
                    model_id = pblInfo.ModelId,
                    chip_name = pblInfo.ChipName,
                    sahara_version = pblInfo.SaharaVersion,
                    is_64bit = pblInfo.Is64Bit,
                    user_note = userNote,
                    timestamp = DateTime.UtcNow.ToString("o")
                };

                string json = JsonSerializer.Serialize(reportData);
                var content = new StringContent(json, Encoding.UTF8, "application/json");

                await _httpClient.PostAsync(CloudApiEndpoint + "/report", content);
                _log("[云端] 设备信息已上报");
            }
            catch (Exception ex)
            {
                _log($"[云端] 上报失败: {ex.Message}");
            }
        }

        #endregion

        #region 加载器查找

        /// <summary>
        /// 根据识别结果查找加载器
        /// </summary>
        public string FindLoader(DeviceIdentifyResult deviceInfo, string loaderDirectory)
        {
            if (!Directory.Exists(loaderDirectory))
                return null;

            // 搜索优先级
            var searchPatterns = new List<string>();

            // 1. 精确匹配 (厂商_芯片_加载器名)
            if (!string.IsNullOrEmpty(deviceInfo.Vendor) && deviceInfo.Vendor != "Unknown")
            {
                searchPatterns.Add($"{deviceInfo.Vendor}_{deviceInfo.ChipName}*.mbn");
                searchPatterns.Add($"{deviceInfo.Vendor}_{deviceInfo.ChipName}*.elf");
            }

            // 2. 芯片匹配
            if (!string.IsNullOrEmpty(deviceInfo.ChipName) && deviceInfo.ChipName != "Unknown")
            {
                searchPatterns.Add($"{deviceInfo.ChipName}*.mbn");
                searchPatterns.Add($"{deviceInfo.ChipName}*.elf");
                searchPatterns.Add($"*{deviceInfo.ChipName}*.mbn");
            }

            // 3. 加载器名称匹配
            if (!string.IsNullOrEmpty(deviceInfo.LoaderName))
            {
                searchPatterns.Add($"*{deviceInfo.LoaderName}*.mbn");
                searchPatterns.Add($"*{deviceInfo.LoaderName}*.elf");
            }

            // 4. 通用模式
            searchPatterns.Add("prog_firehose_ddr*.mbn");
            searchPatterns.Add("xbl_s_devprg_ns*.mbn");
            searchPatterns.Add("prog_emmc_firehose*.mbn");
            searchPatterns.Add("prog_ufs_firehose*.mbn");

            foreach (var pattern in searchPatterns)
            {
                try
                {
                    var files = Directory.GetFiles(loaderDirectory, pattern, SearchOption.AllDirectories);
                    if (files.Length > 0)
                    {
                        // 选择最新的文件
                        var bestFile = files.OrderByDescending(f => new FileInfo(f).LastWriteTime).First();
                        _log($"[加载器] 找到匹配: {Path.GetFileName(bestFile)}");
                        return bestFile;
                    }
                }
                catch { }
            }

            return null;
        }

        /// <summary>
        /// 从云端下载加载器 (如果有 URL)
        /// </summary>
        public async Task<string> DownloadLoaderAsync(DeviceIdentifyResult deviceInfo, string saveDirectory)
        {
            if (string.IsNullOrEmpty(deviceInfo.LoaderUrl))
                return null;

            try
            {
                string fileName = !string.IsNullOrEmpty(deviceInfo.LoaderName) 
                    ? deviceInfo.LoaderName 
                    : $"{deviceInfo.ChipName}_loader.mbn";

                string savePath = Path.Combine(saveDirectory, fileName);

                if (File.Exists(savePath))
                {
                    _log($"[下载] 加载器已存在: {fileName}");
                    return savePath;
                }

                _log($"[下载] 正在下载加载器: {deviceInfo.LoaderUrl}");
                
                var response = await _httpClient.GetAsync(deviceInfo.LoaderUrl);
                if (response.IsSuccessStatusCode)
                {
                    byte[] data = await response.Content.ReadAsByteArrayAsync();
                    File.WriteAllBytes(savePath, data);
                    _log($"[下载] 加载器下载完成: {fileName} ({data.Length} bytes)");
                    return savePath;
                }
            }
            catch (Exception ex)
            {
                _log($"[下载] 下载失败: {ex.Message}");
            }

            return null;
        }

        #endregion

        #region 辅助方法

        private DeviceIdentifyResult CreateUnknownResult()
        {
            return new DeviceIdentifyResult
            {
                ChipName = "Unknown",
                Vendor = "Unknown",
                Model = "Unknown",
                RecommendedStrategy = "Standard"
            };
        }

        private void MergeResult(DeviceIdentifyResult target, DeviceIdentifyResult source)
        {
            if (!string.IsNullOrEmpty(source.Vendor) && source.Vendor != "Unknown")
                target.Vendor = source.Vendor;
            if (!string.IsNullOrEmpty(source.Model) && source.Model != "Unknown")
                target.Model = source.Model;
            if (!string.IsNullOrEmpty(source.LoaderName))
                target.LoaderName = source.LoaderName;
            if (!string.IsNullOrEmpty(source.LoaderUrl))
                target.LoaderUrl = source.LoaderUrl;
            if (!string.IsNullOrEmpty(source.RecommendedStrategy) && source.RecommendedStrategy != "Standard")
                target.RecommendedStrategy = source.RecommendedStrategy;
            if (source.RequiresAuth)
            {
                target.RequiresAuth = true;
                target.AuthType = source.AuthType;
            }
            foreach (var kvp in source.ExtraInfo)
                target.ExtraInfo[kvp.Key] = kvp.Value;
        }

        private void DetermineRecommendedStrategy(DeviceIdentifyResult result)
        {
            if (!string.IsNullOrEmpty(result.RecommendedStrategy) && result.RecommendedStrategy != "Standard")
                return;

            if (result.Vendor == "OPPO" || result.Vendor == "OnePlus" || result.Vendor == "Realme")
            {
                result.RecommendedStrategy = "OppoVip";
                result.RequiresAuth = true;
                result.AuthType = "vip";
            }
            else if (result.Vendor == "Xiaomi" || result.Vendor == "Redmi" || result.Vendor == "POCO")
            {
                result.RecommendedStrategy = "Xiaomi";
                result.RequiresAuth = true;
                result.AuthType = "miauth";
            }
            else
            {
                result.RecommendedStrategy = "Standard";
            }
        }

        private string GetPkHashKey(byte[] pkHash)
        {
            if (pkHash == null || pkHash.Length < 8)
                return "";
            return BitConverter.ToString(pkHash, 0, 8).Replace("-", "");
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

        #region 静态便捷方法

        /// <summary>
        /// 快速识别设备
        /// </summary>
        public static DeviceIdentifyResult QuickIdentify(uint msmId, byte[] pkHash)
        {
            var identifier = new DeviceIdentifier();
            return identifier.IdentifyDeviceAsync(msmId, pkHash, null).Result;
        }

        /// <summary>
        /// 获取芯片详细信息
        /// </summary>
        public static string GetChipDetails(uint msmId)
        {
            string chipName = QualcommDatabase.GetChipName(msmId);
            var memType = QualcommDatabase.GetMemoryType(chipName);
            int saharaVer = QualcommDatabase.GetSaharaVersion(chipName);
            
            return $"{chipName} | {memType} | Sahara V{saharaVer}";
        }

        #endregion
    }
}
