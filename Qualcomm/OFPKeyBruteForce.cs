using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OPFlashTool.Qualcomm
{
    /// <summary>
    /// OFP 密钥爆破器
    /// 基于已知密钥模式进行智能爆破
    /// </summary>
    public class OFPKeyBruteForce
    {
        private readonly Action<string> _log;
        private readonly string _ofpFilePath;
        private readonly int _pageSize;
        private volatile bool _found = false;
        private string _foundKey;
        private string _foundIv;
        private long _totalTried = 0;
        private DateTime _startTime;

        // 已知的密钥模板 (用于生成变体) - 来自 bkerler/oppo_decrypt 和 unix3dgforce/OppoDecrypt
        private static readonly List<string[]> KnownKeyTemplates = new List<string[]>
        {
            // Qualcomm (mc, userkey, ivec) 模板
            new[] { "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
            new[] { "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
            new[] { "3C2D518D9BF2E4279DC758CD535147C3", "87C74A29709AC1BF2382276C4E8DF232", "598D92E967265E9BCABE2469FE4A915E" },
            new[] { "E11AA7BB558A436A8375FD15DDD4651F", "77DDF6A0696841F6B74782C097835169", "A739742384A44E8BA45207AD5C3700EA" },
            new[] { "8FB8FB261930260BE945B841AEFA9FD4", "E529E82B28F5A2F8831D860AE39E425D", "8A09DA60ED36F125D64709973372C1CF" },
            new[] { "E8AE288C0192C54BF10C5707E9C4705B", "D64FC385DCD52A3C9B5FBA8650F92EDA", "79051FD8D8B6297E2E4559E997F63B7F" },
            // MTK - 来自 unix3dgforce/OppoDecrypt
            new[] { "9E4F32639D21357D37D226B69A495D21", "A3D8D358E42F5A9E931DD3917D9A3218", "386935399137416B67416BECF22F519A" },
            new[] { "892D57E92A4D8A975E3C216B7C9DE189", "D26DF2D9913785B145D18C7219B89F26", "516989E4A1BFC78B365C6BC57D944391" },
            new[] { "3C4A618D9BF2E4279DC758CD535147C3", "87B13D29709AC1BF2382276C4E8DF232", "59B7A8E967265E9BCABE2469FE4A915E" },
            new[] { "1C3288822BF824259DC852C1733127D3", "E7918D22799181CF2312176C9E2DF298", "3247F889A7B6DECBCA3E28693E4AAAFE" },
            new[] { "1E4F32239D65A57D37D2266D9A775D43", "A332D3C3E42F5A3E931DD991729A321D", "3F2A35399A373377674155ECF28FD19A" },
            new[] { "122D57E92A518AFF5E3C786B7C34E189", "DD6DF2D9543785674522717219989FB0", "12698965A132C76136CC88C5DD94EE91" },
            // 2024 新增 - 基于模式推测
            new[] { "E9AF298D0293C55CF20D5808EAD5715C", "D74FD486DDD63A4D9C6FCA9751F93FDB", "7A062FD9D9B7398F2F5669EA98F74C80" },
            new[] { "EAAB2A8E0394C65DF30E5909EBD6725D", "D850D587DED73B4E9D70CB9852FA40DC", "7B073FE0DAB83A902056AEB999F84D81" },
            new[] { "EBB02B8F0495C75EF40F5A0AECD7735E", "D951D688DFD83C4F9E71CC9953FB41DD", "7C0840E1DBB93B91216ABFBA9AF94E82" },
            // Find X7 / PFUM10 系列猜测
            new[] { "4F505046494E4458374458373635383A", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
            new[] { "50465545313044313143343032303234", "87C74A29709AC1BF2382276C4E8DF232", "598D92E967265E9BCABE2469FE4A915E" },
            new[] { "636F6C6F726F733134636F6C6F726F73", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
            new[] { "6F70706F66696E6478376F70706F6669", "E529E82B28F5A2F8831D860AE39E425D", "8A09DA60ED36F125D64709973372C1CF" },
        };

        // 常见的简单密钥对
        private static readonly List<string[]> SimpleKeyTemplates = new List<string[]>
        {
            new[] { "d154afeeaafa958f", "2c040f5786829207" },
            new[] { "2e96d7f462591a0f", "17cc63224c208708" },
            new[] { "ab3f76d7989207f2", "2bf515b3a9737835" },
            new[] { "d6ccb8c7c89a35d0", "349a3f5de0e4d07a" },
        };

        public OFPKeyBruteForce(string ofpPath, int pageSize, Action<string> logger = null)
        {
            _ofpFilePath = ofpPath;
            _pageSize = pageSize;
            _log = logger ?? Console.WriteLine;
        }

        /// <summary>
        /// 开始爆破 (返回找到的密钥或 null)
        /// </summary>
        public async Task<BruteForceResult> BruteForceAsync(CancellationToken ct = default, IProgress<BruteForceProgress> progress = null)
        {
            _startTime = DateTime.Now;
            _found = false;
            _totalTried = 0;

            _log("[爆破] 开始智能密钥爆破...");
            _log("[爆破] 模式1: 基于已知密钥的字节变体");
            _log("[爆破] 模式2: 常见十六进制模式");
            _log("[爆破] 模式3: 增量爆破");

            var result = new BruteForceResult();

            // 阶段1: 基于已知密钥模板的变体爆破
            _log("\n[阶段1] 基于已知模板的变体爆破...");
            var variantKeys = GenerateKeyVariants();
            _log($"[阶段1] 生成 {variantKeys.Count} 个变体密钥");

            foreach (var keySet in variantKeys)
            {
                if (ct.IsCancellationRequested || _found) break;

                Interlocked.Increment(ref _totalTried);
                
                if (_totalTried % 100 == 0)
                {
                    progress?.Report(new BruteForceProgress
                    {
                        Tried = _totalTried,
                        Speed = _totalTried / Math.Max(1, (DateTime.Now - _startTime).TotalSeconds),
                        Phase = "变体爆破"
                    });
                }

                if (TryKey(keySet.Key, keySet.Iv, out string xml))
                {
                    _found = true;
                    _foundKey = keySet.Key;
                    _foundIv = keySet.Iv;
                    result.Success = true;
                    result.Key = keySet.Key;
                    result.Iv = keySet.Iv;
                    result.KeySource = keySet.Source;
                    result.XmlPreview = xml.Length > 500 ? xml.Substring(0, 500) + "..." : xml;
                    _log($"\n[成功!] 找到密钥!");
                    _log($"  Key: {keySet.Key}");
                    _log($"  IV:  {keySet.Iv}");
                    _log($"  来源: {keySet.Source}");
                    return result;
                }
            }

            // 阶段2: 简单密钥变体
            if (!_found)
            {
                _log("\n[阶段2] 简单密钥变体爆破...");
                var simpleVariants = GenerateSimpleKeyVariants();
                _log($"[阶段2] 生成 {simpleVariants.Count} 个简单变体");

                foreach (var keySet in simpleVariants)
                {
                    if (ct.IsCancellationRequested || _found) break;

                    Interlocked.Increment(ref _totalTried);

                    if (TryKey(keySet.Key, keySet.Iv, out string xml))
                    {
                        _found = true;
                        result.Success = true;
                        result.Key = keySet.Key;
                        result.Iv = keySet.Iv;
                        result.KeySource = "简单变体";
                        result.XmlPreview = xml.Length > 500 ? xml.Substring(0, 500) + "..." : xml;
                        _log($"\n[成功!] 找到简单密钥!");
                        _log($"  Key: {keySet.Key}");
                        _log($"  IV:  {keySet.Iv}");
                        return result;
                    }
                }
            }

            // 阶段3: 基于字节模式的增量爆破 (更慢但更全面)
            if (!_found)
            {
                _log("\n[阶段3] 增量字节爆破 (可能需要较长时间)...");
                await IncrementalBruteForceAsync(ct, progress, result);
            }

            result.TotalTried = _totalTried;
            result.Duration = DateTime.Now - _startTime;

            if (!result.Success)
            {
                _log($"\n[爆破完成] 未找到匹配密钥");
                _log($"  已尝试: {_totalTried:N0} 个密钥");
                _log($"  耗时: {result.Duration.TotalSeconds:F1} 秒");
            }

            return result;
        }

        /// <summary>
        /// 生成基于已知密钥的变体
        /// </summary>
        private List<KeyCandidate> GenerateKeyVariants()
        {
            var candidates = new List<KeyCandidate>();

            foreach (var template in KnownKeyTemplates)
            {
                string mc = template[0];
                string userkey = template[1];
                string ivec = template[2];

                // 原始密钥
                var original = GenerateKeyFromTemplate(mc, userkey, ivec);
                candidates.Add(new KeyCandidate { Key = original.key, Iv = original.iv, Source = "原始模板" });

                // 字节变体: 修改 mc 的最后几个字节
                for (int i = 0; i < 256; i++)
                {
                    string mcVar = mc.Substring(0, 30) + i.ToString("X2");
                    var variant = GenerateKeyFromTemplate(mcVar, userkey, ivec);
                    candidates.Add(new KeyCandidate { Key = variant.key, Iv = variant.iv, Source = $"mc变体[{i:X2}]" });
                }

                // 字节变体: 修改 userkey 的最后几个字节
                for (int i = 0; i < 256; i++)
                {
                    string ukVar = userkey.Substring(0, 30) + i.ToString("X2");
                    var variant = GenerateKeyFromTemplate(mc, ukVar, ivec);
                    candidates.Add(new KeyCandidate { Key = variant.key, Iv = variant.iv, Source = $"uk变体[{i:X2}]" });
                }

                // 字节变体: 修改 ivec 的最后几个字节
                for (int i = 0; i < 256; i++)
                {
                    string ivVar = ivec.Substring(0, 30) + i.ToString("X2");
                    var variant = GenerateKeyFromTemplate(mc, userkey, ivVar);
                    candidates.Add(new KeyCandidate { Key = variant.key, Iv = variant.iv, Source = $"iv变体[{i:X2}]" });
                }

                // 混合变体: 同时修改 mc 和 userkey 的最后字节
                for (int i = 0; i < 16; i++)
                {
                    for (int j = 0; j < 16; j++)
                    {
                        string mcVar = mc.Substring(0, 30) + (i * 16).ToString("X2");
                        string ukVar = userkey.Substring(0, 30) + (j * 16).ToString("X2");
                        var variant = GenerateKeyFromTemplate(mcVar, ukVar, ivec);
                        candidates.Add(new KeyCandidate { Key = variant.key, Iv = variant.iv, Source = $"混合[{i:X},{j:X}]" });
                    }
                }
            }

            return candidates.Distinct(new KeyCandidateComparer()).ToList();
        }

        /// <summary>
        /// 生成简单密钥变体
        /// </summary>
        private List<KeyCandidate> GenerateSimpleKeyVariants()
        {
            var candidates = new List<KeyCandidate>();

            foreach (var template in SimpleKeyTemplates)
            {
                string key = template[0];
                string iv = template[1];

                // 原始
                candidates.Add(new KeyCandidate { Key = key, Iv = iv, Source = "简单原始" });

                // 修改最后一个字节
                for (int i = 0; i < 256; i++)
                {
                    string keyVar = key.Substring(0, 14) + i.ToString("x2");
                    candidates.Add(new KeyCandidate { Key = keyVar, Iv = iv, Source = $"简单key[{i:x2}]" });
                }

                // 修改 iv 最后字节
                for (int i = 0; i < 256; i++)
                {
                    string ivVar = iv.Substring(0, 14) + i.ToString("x2");
                    candidates.Add(new KeyCandidate { Key = key, Iv = ivVar, Source = $"简单iv[{i:x2}]" });
                }
            }

            return candidates.Distinct(new KeyCandidateComparer()).ToList();
        }

        /// <summary>
        /// 增量爆破 (更全面但更慢)
        /// </summary>
        private async Task IncrementalBruteForceAsync(CancellationToken ct, IProgress<BruteForceProgress> progress, BruteForceResult result)
        {
            // 基于常见十六进制模式生成更多变体
            var patterns = new[]
            {
                "0123456789ABCDEF",
                "FEDCBA9876543210",
                "1234567890ABCDEF",
                "ABCDEF0123456789"
            };

            int count = 0;
            int maxCount = 10000; // 限制增量爆破数量

            foreach (var p1 in patterns)
            {
                foreach (var p2 in patterns)
                {
                    if (ct.IsCancellationRequested || _found || count >= maxCount) return;

                    // 生成 16 字节的 key 和 iv
                    for (int offset = 0; offset < 8 && count < maxCount; offset++)
                    {
                        string key = (p1 + p1).Substring(offset, 16).ToLower();
                        string iv = (p2 + p2).Substring(offset, 16).ToLower();

                        Interlocked.Increment(ref _totalTried);
                        count++;

                        if (count % 500 == 0)
                        {
                            progress?.Report(new BruteForceProgress
                            {
                                Tried = _totalTried,
                                Speed = _totalTried / Math.Max(1, (DateTime.Now - _startTime).TotalSeconds),
                                Phase = "增量爆破"
                            });
                        }

                        if (TryKey(key, iv, out string xml))
                        {
                            _found = true;
                            result.Success = true;
                            result.Key = key;
                            result.Iv = iv;
                            result.KeySource = "增量爆破";
                            result.XmlPreview = xml.Length > 500 ? xml.Substring(0, 500) + "..." : xml;
                            _log($"\n[成功!] 增量爆破找到密钥!");
                            _log($"  Key: {key}");
                            _log($"  IV:  {iv}");
                            return;
                        }
                    }
                }
            }

            // 随机爆破
            _log("[阶段3b] 随机密钥爆破...");
            var random = new Random();
            for (int i = 0; i < 5000 && !ct.IsCancellationRequested && !_found; i++)
            {
                byte[] keyBytes = new byte[16];
                byte[] ivBytes = new byte[16];
                random.NextBytes(keyBytes);
                random.NextBytes(ivBytes);

                string key = BitConverter.ToString(keyBytes).Replace("-", "").ToLower().Substring(0, 16);
                string iv = BitConverter.ToString(ivBytes).Replace("-", "").ToLower().Substring(0, 16);

                Interlocked.Increment(ref _totalTried);

                if (TryKey(key, iv, out string xml))
                {
                    _found = true;
                    result.Success = true;
                    result.Key = key;
                    result.Iv = iv;
                    result.KeySource = "随机爆破";
                    result.XmlPreview = xml.Length > 500 ? xml.Substring(0, 500) + "..." : xml;
                    return;
                }
            }
        }

        /// <summary>
        /// 从模板生成实际密钥
        /// </summary>
        private (string key, string iv) GenerateKeyFromTemplate(string mc, string userkey, string ivec)
        {
            try
            {
                byte[] mcBytes = HexToBytes(mc);
                byte[] ukBytes = HexToBytes(userkey);
                byte[] ivBytes = HexToBytes(ivec);

                // 解混淆
                byte[] deobfKey = DeObfuscate(ukBytes, mcBytes);
                byte[] deobfIv = DeObfuscate(ivBytes, mcBytes);

                // MD5 哈希取前 16 字符
                string key = MD5Hash(deobfKey).Substring(0, 16);
                string iv = MD5Hash(deobfIv).Substring(0, 16);

                return (key, iv);
            }
            catch
            {
                return (null, null);
            }
        }

        /// <summary>
        /// 测试密钥是否有效
        /// </summary>
        private bool TryKey(string key, string iv, out string xmlContent)
        {
            xmlContent = null;
            if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(iv) || key.Length != 16 || iv.Length != 16)
                return false;

            try
            {
                using (var fs = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    long fileLength = fs.Length;
                    long xmlOffset = fileLength - _pageSize;

                    // 读取 XML 偏移和长度
                    fs.Seek(xmlOffset + 20, SeekOrigin.Begin);
                    byte[] offsetBytes = new byte[4];
                    fs.Read(offsetBytes, 0, 4);
                    Array.Reverse(offsetBytes);
                    long offset = (long)BitConverter.ToUInt32(offsetBytes, 0) * _pageSize;

                    byte[] lengthBytes = new byte[4];
                    fs.Read(lengthBytes, 0, 4);
                    Array.Reverse(lengthBytes);
                    long length = BitConverter.ToUInt32(lengthBytes, 0);

                    if (offset < 0 || offset >= fileLength || length <= 0 || length > fileLength)
                        return false;

                    // 对齐到 16 字节
                    long alignedLength = ((length + 15) / 16) * 16;

                    // 读取加密数据
                    fs.Seek(offset, SeekOrigin.Begin);
                    byte[] encryptedData = new byte[alignedLength];
                    fs.Read(encryptedData, 0, (int)alignedLength);

                    // 解密
                    byte[] keyBytes = Encoding.UTF8.GetBytes(key);
                    byte[] ivBytes = Encoding.UTF8.GetBytes(iv);
                    byte[] decrypted = DecryptAesCfb(encryptedData, keyBytes, ivBytes);

                    string xmlString = Encoding.UTF8.GetString(decrypted, 0, (int)Math.Min(length, decrypted.Length));

                    // 验证是否是有效的 XML
                    if (xmlString.Contains("<?xml") || xmlString.Contains("<profile") || 
                        (xmlString.Contains("<") && xmlString.Contains(">") && xmlString.Contains("Path=")))
                    {
                        xmlContent = xmlString;
                        return true;
                    }
                }
            }
            catch
            {
                // 忽略错误
            }

            return false;
        }

        #region 辅助方法

        private byte[] DeObfuscate(byte[] data, byte[] mask)
        {
            byte[] result = new byte[data.Length];
            for (int i = 0; i < data.Length && i < mask.Length; i++)
            {
                int xored = data[i] ^ mask[i];
                result[i] = (byte)(((xored >> 4) | ((xored & 0xF) << 4)) & 0xFF);
            }
            return result;
        }

        private string MD5Hash(byte[] input)
        {
            using (var md5 = MD5.Create())
            {
                byte[] hash = md5.ComputeHash(input);
                return BitConverter.ToString(hash).Replace("-", "").ToLower();
            }
        }

        private byte[] HexToBytes(string hex)
        {
            hex = hex.Replace(" ", "");
            byte[] bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            }
            return bytes;
        }

        private byte[] DecryptAesCfb(byte[] cipherText, byte[] key, byte[] iv)
        {
            using (var aes = new RijndaelManaged())
            {
                aes.BlockSize = 128;
                aes.KeySize = 128;
                aes.Mode = CipherMode.CFB;
                aes.FeedbackSize = 128;
                aes.Padding = PaddingMode.None;
                aes.Key = key;
                aes.IV = iv;

                using (var decryptor = aes.CreateDecryptor())
                using (var ms = new MemoryStream())
                {
                    using (var cs = new CryptoStream(ms, decryptor, CryptoStreamMode.Write))
                    {
                        cs.Write(cipherText, 0, cipherText.Length);
                    }
                    return ms.ToArray();
                }
            }
        }

        #endregion
    }

    #region 数据类

    public class BruteForceResult
    {
        public bool Success { get; set; }
        public string Key { get; set; }
        public string Iv { get; set; }
        public string KeySource { get; set; }
        public string XmlPreview { get; set; }
        public long TotalTried { get; set; }
        public TimeSpan Duration { get; set; }
    }

    public class BruteForceProgress
    {
        public long Tried { get; set; }
        public double Speed { get; set; }
        public string Phase { get; set; }
    }

    internal class KeyCandidate
    {
        public string Key { get; set; }
        public string Iv { get; set; }
        public string Source { get; set; }
    }

    internal class KeyCandidateComparer : IEqualityComparer<KeyCandidate>
    {
        public bool Equals(KeyCandidate x, KeyCandidate y)
        {
            return x.Key == y.Key && x.Iv == y.Iv;
        }

        public int GetHashCode(KeyCandidate obj)
        {
            return (obj.Key + obj.Iv).GetHashCode();
        }
    }

    #endregion
}
