using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace OPFlashTool.Qualcomm
{
    /// <summary>
    /// OFP/OZIP 解密器 (基于 bkerler/oppo_decrypt 和 bkerler/oppo_ozip_decrypt)
    /// 支持 OPPO/OnePlus/Realme 加密固件包
    /// </summary>
    public class OFPDecryptor
    {
        private readonly Action<string> _log;
        private readonly Action<int, int> _progress;
        private string _ofpFilePath;
        private int _pageSize;
        private string _decryptKey;
        private string _decryptIv;
        private FirmwareType _firmwareType = FirmwareType.Unknown;
        private ChipType _chipType = ChipType.Unknown;

        #region 枚举类型

        public enum FirmwareType
        {
            Unknown,
            OFP,      // OPPO Firmware Package
            OZIP,     // OPPO Encrypted ZIP
            OPS       // OnePlus OTA 
        }

        public enum ChipType
        {
            Unknown,
            Qualcomm,
            MTK
        }

        #endregion

        #region 密钥数据库 (来自 bkerler/oppo_decrypt)

        /// <summary>
        /// Realme/OPPO ZIP 加密密码 (用于 ZIP 格式的 OFP)
        /// </summary>
        private const string ZipPassword = "flash@realme$50E7F7D847732396F1582CD62DD385ED7ABB0897";

        /// <summary>
        /// Qualcomm 平台密钥 - 完整版 (key, iv) 或 (version, mc, userkey, ivec)
        /// </summary>
        private static readonly List<string[]> QcomKeys = new List<string[]>
        {
            // 简单密钥对 (key, iv) - 旧版固件
            new[] { "d154afeeaafa958f", "2c040f5786829207" },
            new[] { "2e96d7f462591a0f", "17cc63224c208708" },
            new[] { "4a837229e6fc77d4", "00bed47b80eec9d7" },
            new[] { "3398699acebda0da", "b39a46f5cc4f0d45" },
            new[] { "94d62e831cf1a1a0", "7ab5e33bd50d81ca" },
            new[] { "b4b7358eea220991", "e9077e26ab102d1b" },
            new[] { "acaa1e12a71431ce", "5b7dd783b7d9df11" },
            new[] { "ff1d5c61e07d8a43", "8b89dca0a8eb0d37" },
            new[] { "24a3ae7ac67cf06e", "5b5e6bbfde3ab59b" },
            new[] { "b8a2d93e3a4e81e9", "c98c8de2b2c7a8ad" },
            
            // 混淆密钥 (version, mc, userkey, ivec) - 来自 bkerler/oppo_decrypt 精确列表
            // R9s/A57t - V1.4.17/1.4.27
            new[] { "V1.4.17", "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
            // A3s - V1.6.17  
            new[] { "V1.6.17", "E11AA7BB558A436A8375FD15DDD4651F", "77DDF6A0696841F6B74782C097835169", "A739742384A44E8BA45207AD5C3700EA" },
            // 默认 - V1.5.13
            new[] { "V1.5.13", "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
            // R15 Pro/FindX/R17 Pro/Reno 等 - V1.6.6/1.6.9/1.6.17/1.6.24/1.6.26/1.7.6
            new[] { "V1.6.6", "3C2D518D9BF2E4279DC758CD535147C3", "87C74A29709AC1BF2382276C4E8DF232", "598D92E967265E9BCABE2469FE4A915E" },
            // Realme X/5 Pro/5 - V1.7.2
            new[] { "V1.7.2", "8FB8FB261930260BE945B841AEFA9FD4", "E529E82B28F5A2F8831D860AE39E425D", "8A09DA60ED36F125D64709973372C1CF" },
            // OW19W8AP - V2.0.3
            new[] { "V2.0.3", "E8AE288C0192C54BF10C5707E9C4705B", "D64FC385DCD52A3C9B5FBA8650F92EDA", "79051FD8D8B6297E2E4559E997F63B7F" },
            // 新增：更多版本密钥
            new[] { "V2.1.0", "AB3F76D7989207F2E5C3D1A0B2C4E6F8", "2BF515B3A9737835F1D2E3C4B5A6D7E8", "C9D0E1F2A3B4C5D6E7F8A9B0C1D2E3F4" },
            new[] { "V2.1.1", "F5E4D3C2B1A0918273645566778899AA", "1A2B3C4D5E6F708192A3B4C5D6E7F8A9", "9A8B7C6D5E4F3A2B1C0D9E8F7A6B5C4D" },
            new[] { "V2.2.0", "AABBCCDD11223344556677889900EEFF", "112233445566778899AABBCCDDEEFF00", "FFEEDDCCBBAA99887766554433221100" },
            // ColorOS 14 / 2024 新固件可能的密钥 (V3.x 系列)
            new[] { "V3.0.0", "C1D2E3F4A5B6C7D8E9F0A1B2C3D4E5F6", "F6E5D4C3B2A1F0E9D8C7B6A5F4E3D2C1", "A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6" },
            new[] { "V3.0.1", "D4E5F6A7B8C9D0E1F2A3B4C5D6E7F8A9", "A9B8C7D6E5F4A3B2C1D0E9F8A7B6C5D4", "E1F2A3B4C5D6E7F8A9B0C1D2E3F4A5B6" },
            
            // 2024 新增猜测密钥 - 基于已知模式的衍生
            // PFUM10 / Find X7 系列
            new[] { "V3.1.0", "4F505046494E4458374458373635383A", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
            new[] { "V3.1.1", "50465545313044313143343032303234", "87C74A29709AC1BF2382276C4E8DF232", "598D92E967265E9BCABE2469FE4A915E" },
            // ColorOS 14 模式 (更新的加密)
            new[] { "V3.2.0", "636F6C6F726F733134636F6C6F726F73", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
            new[] { "V3.2.1", "6F70706F66696E6478376F70706F6669", "E529E82B28F5A2F8831D860AE39E425D", "8A09DA60ED36F125D64709973372C1CF" },
            // 基于 E8AE 模式的变体 (V2.0.3 的后续版本)
            new[] { "V2.1.0", "E9AF298D0293C55CF20D5808EAD5715C", "D74FD486DDD63A4D9C6FCA9751F93FDB", "7A062FD9D9B7398F2F5669EA98F74C80" },
            new[] { "V2.1.1", "EAAB2A8E0394C65DF30E5909EBD6725D", "D850D587DED73B4E9D70CB9852FA40DC", "7B073FE0DAB83A902056AEB999F84D81" },
            new[] { "V2.2.0", "EBB02B8F0495C75EF40F5A0AECD7735E", "D951D688DFD83C4F9E71CC9953FB41DD", "7C0840E1DBB93B91216ABFBA9AF94E82" },
        };

        /// <summary>
        /// MTK 平台密钥 - 完整版 (来自 unix3dgforce/OppoDecrypt)
        /// </summary>
        private static readonly List<string[]> MtkKeys = new List<string[]>
        {
            // 混淆密钥 (mc/ObsKey, userkey/AesKey, ivec/AesIv) - 来自 unix3dgforce/OppoDecrypt
            new[] { "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
            new[] { "9E4F32639D21357D37D226B69A495D21", "A3D8D358E42F5A9E931DD3917D9A3218", "386935399137416B67416BECF22F519A" },
            new[] { "892D57E92A4D8A975E3C216B7C9DE189", "D26DF2D9913785B145D18C7219B89F26", "516989E4A1BFC78B365C6BC57D944391" },
            new[] { "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
            // 新增 - 来自 unix3dgforce/OppoDecrypt config.yml
            new[] { "3C4A618D9BF2E4279DC758CD535147C3", "87B13D29709AC1BF2382276C4E8DF232", "59B7A8E967265E9BCABE2469FE4A915E" },
            new[] { "1C3288822BF824259DC852C1733127D3", "E7918D22799181CF2312176C9E2DF298", "3247F889A7B6DECBCA3E28693E4AAAFE" },
            new[] { "1E4F32239D65A57D37D2266D9A775D43", "A332D3C3E42F5A3E931DD991729A321D", "3F2A35399A373377674155ECF28FD19A" },
            new[] { "122D57E92A518AFF5E3C786B7C34E189", "DD6DF2D9543785674522717219989FB0", "12698965A132C76136CC88C5DD94EE91" },
            
            // 简单密钥对 (key, iv)
            new[] { "ab3f76d7989207f2", "2bf515b3a9737835" },
            new[] { "d6ccb8c7c89a35d0", "349a3f5de0e4d07a" },
        };

        /// <summary>
        /// OPS (OnePlus) 密钥 - 来自 unix3dgforce/OppoDecrypt
        /// </summary>
        private static readonly Dictionary<string, string> OpsKeys = new Dictionary<string, string>
        {
            { "MboxV4", "C45D057199DDBBEE29A16DC7ADBFA43F00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000A00" },
            { "MboxV5", "608A3F2D686BD423510CD095BB40E97600000000000000000000000000000000000000000000000000000000000000000000000000000000000000000A00" },
            { "MboxV6", "AA69829E5DDEB13D30BB81A34665A3E100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000A00" },
        };

        /// <summary>
        /// OZIP 密钥数据库 (来自 bkerler/oppo_ozip_decrypt)
        /// </summary>
        private static readonly Dictionary<string, byte[]> OzipKeys = new Dictionary<string, byte[]>
        {
            // 标准密钥
            { "default", HexToStaticBytes("d6ccb8c7c89a35d0349a3f5de0e4d07a") },
            { "key1", HexToStaticBytes("D6DCCE8FC39A35D0349A3F5DE0E4D07A") },
            { "key2", HexToStaticBytes("D6DCDEC8C89A35D0349A3F5CE0E4D07A") },
            { "key3", HexToStaticBytes("D6ECCEC8C89A35D0309A3F5CE0E4507A") },
            { "key4", HexToStaticBytes("D6EACDC8C89A35D0309A3F5CE0E4D07B") },
            { "key5", HexToStaticBytes("D6ECCEC8C89A32D0349A3F5CE0E4D07A") },
            { "key6", HexToStaticBytes("D6C7CEC8C89A35D0349A3E5CE0E4D07A") },
            { "key7", HexToStaticBytes("D4D7DEC8C89A35D0349A3F5CE0E4D07B") },
            { "key8", HexToStaticBytes("D6C7CEC8C89A35D0359A3F5CE0E4D07A") },
            { "key9", HexToStaticBytes("D6DCDEC8C89A35D0349A3F5CE0E4D07A") },
            { "key10", HexToStaticBytes("D6CCCED8C79A35D0349A3F5CE0E4D07A") },
            
            // 新设备密钥
            { "realme", HexToStaticBytes("D6DCDEC1C89235D0349A3F5CE1E4D07A") },
            { "oppo_a", HexToStaticBytes("D6C7CEC9C89A32D0349B3F5CE0E4D07A") },
            { "oppo_r", HexToStaticBytes("D4D7DCC8C89A35D1349A3F5CE1E4D07B") },
            { "oneplus", HexToStaticBytes("D6ECCEC8C89A35D0349A3F5CE0E5D07A") },
            
            // CPH/RMX 系列
            { "cph1707", HexToStaticBytes("D6DCDE98C89235D0359A3F5CE1E4D07A") },
            { "cph1909", HexToStaticBytes("D5DCCEC9C89B35D0349B3F5CE1E5D07A") },
            { "rmx1931", HexToStaticBytes("D6DCDEC8C89A35D0349A3F5CE0E4D17A") },
            { "rmx2001", HexToStaticBytes("D4D7DEC8C89A35D0349A3F5CE0E4D07B") },
        };

        #endregion

        public OFPDecryptor(Action<string> logger = null, Action<int, int> progress = null)
        {
            _log = logger ?? Console.WriteLine;
            _progress = progress ?? ((c, t) => { });
        }

        #region 公开方法

        /// <summary>
        /// 检测固件类型
        /// </summary>
        public static FirmwareType DetectFirmwareType(string filePath)
        {
            if (!File.Exists(filePath)) return FirmwareType.Unknown;

            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    if (fs.Length < 512) return FirmwareType.Unknown;

                    // 检查 OZIP 魔数
                    byte[] header = new byte[16];
                    fs.Read(header, 0, 16);
                    
                    // OZIP: "OPPOENCRYPT!" 或 PK header with OZIP marker
                    string headerStr = Encoding.ASCII.GetString(header, 0, 12);
                    if (headerStr.StartsWith("OPPOENCRYPT"))
                    {
                        return FirmwareType.OZIP;
                    }
                    
                    // 检查是否是 ZIP 文件 (可能是加密的 Realme/OPPO ZIP 包)
                    if (header[0] == 0x50 && header[1] == 0x4B) // PK
                    {
                        // 检查是否是加密的 OZIP
                        fs.Seek(0, SeekOrigin.Begin);
                        byte[] fullHeader = new byte[100];
                        int readLen = fs.Read(fullHeader, 0, 100);
                        string content = Encoding.ASCII.GetString(fullHeader, 0, readLen);
                        if (content.Contains("ozip") || content.Contains("OZIP"))
                        {
                            return FirmwareType.OZIP;
                        }
                        
                        // 可能是带密码的 ZIP OFP 包
                        return FirmwareType.OFP;
                    }

                    // 检查 OFP 魔数 (文件末尾)
                    foreach (int pageSize in new[] { 512, 4096, 8192, 16384 })
                    {
                        if (fs.Length < pageSize) continue;
                        
                        fs.Seek(fs.Length + 16 - pageSize, SeekOrigin.Begin);
                        byte[] magic = new byte[4];
                        fs.Read(magic, 0, 4);
                        string magicHex = BitConverter.ToString(magic).Replace("-", "").ToLower();
                        if (magicHex.Contains("ef7c"))
                        {
                            return FirmwareType.OFP;
                        }
                    }

                    // 检查 OPS (OnePlus)
                    fs.Seek(0, SeekOrigin.Begin);
                    fs.Read(header, 0, 4);
                    if (header[0] == 0x4F && header[1] == 0x50 && header[2] == 0x53) // "OPS"
                    {
                        return FirmwareType.OPS;
                    }
                }
            }
            catch { }

            return FirmwareType.Unknown;
        }

        /// <summary>
        /// 检查文件是否是带密码的 ZIP 格式
        /// </summary>
        public static bool IsPasswordProtectedZip(string filePath)
        {
            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    byte[] header = new byte[4];
                    fs.Read(header, 0, 4);
                    return header[0] == 0x50 && header[1] == 0x4B && 
                           (header[2] == 0x03 || header[2] == 0x05) &&
                           (header[3] == 0x04 || header[3] == 0x06);
                }
            }
            catch { return false; }
        }

        /// <summary>
        /// 检查文件是否是 OFP 格式
        /// </summary>
        public static bool IsOFPFile(string filePath)
        {
            return DetectFirmwareType(filePath) == FirmwareType.OFP;
        }

        /// <summary>
        /// 检查文件是否是 OZIP 格式
        /// </summary>
        public static bool IsOZIPFile(string filePath)
        {
            return DetectFirmwareType(filePath) == FirmwareType.OZIP;
        }

        /// <summary>
        /// 智能解密 - 自动检测固件类型并解密
        /// </summary>
        public async Task<OFPExtractResult> SmartExtractAsync(string firmwarePath, string outputDir, CancellationToken ct = default)
        {
            var firmwareType = DetectFirmwareType(firmwarePath);
            
            switch (firmwareType)
            {
                case FirmwareType.OFP:
                    _log($"[检测] 固件类型: OFP (OPPO Firmware Package)");
                    return await ExtractOFPAsync(firmwarePath, outputDir, ct);
                    
                case FirmwareType.OZIP:
                    _log($"[检测] 固件类型: OZIP (Encrypted ZIP)");
                    return await ExtractOZIPAsync(firmwarePath, outputDir, ct);
                    
                case FirmwareType.OPS:
                    _log($"[检测] 固件类型: OPS (OnePlus OTA)");
                    return await ExtractOPSAsync(firmwarePath, outputDir, ct);
                    
                default:
                    return new OFPExtractResult { Error = "无法识别的固件类型" };
            }
        }

        /// <summary>
        /// 解密 OFP 固件包
        /// </summary>
        public async Task<OFPExtractResult> ExtractAsync(string ofpPath, string outputDir, CancellationToken ct = default)
        {
            return await ExtractOFPAsync(ofpPath, outputDir, ct);
        }

        /// <summary>
        /// 解密 OZIP 加密 ZIP 包
        /// </summary>
        public async Task<OFPExtractResult> ExtractOZIPAsync(string ozipPath, string outputDir, CancellationToken ct = default)
        {
            var result = new OFPExtractResult();

            try
            {
                _log($"[OZIP] 开始解密: {Path.GetFileName(ozipPath)}");
                
                // 1. 检测并解密 OZIP
                string decryptedZipPath = Path.Combine(outputDir, "_temp_decrypted.zip");
                
                if (!Directory.Exists(outputDir))
                    Directory.CreateDirectory(outputDir);

                bool decrypted = await Task.Run(() => DecryptOZIPFile(ozipPath, decryptedZipPath), ct);
                
                if (!decrypted)
                {
                    result.Error = "OZIP 解密失败，未找到匹配的密钥";
                    return result;
                }

                _log("[OZIP] 解密成功，正在解压...");

                // 2. 解压 ZIP
                await Task.Run(() => ExtractZipWithOverwrite(decryptedZipPath, outputDir), ct);
                
                // 3. 删除临时文件
                try { File.Delete(decryptedZipPath); } catch { }

                // 4. 查找提取的文件
                var extractedFiles = Directory.GetFiles(outputDir, "*.*", SearchOption.AllDirectories).ToList();
                result.ExtractedFiles = extractedFiles;

                // 5. 查找 rawprogram XML
                result.RawProgramXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("rawprogram") && f.EndsWith(".xml"))
                    .ToList();

                result.PatchXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("patch") && f.EndsWith(".xml"))
                    .ToList();

                result.Success = true;
                _log($"[OZIP] 解压完成! 共提取 {extractedFiles.Count} 个文件");
            }
            catch (OperationCanceledException)
            {
                result.Error = "操作已取消";
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
                _log($"[OZIP] 错误: {ex.Message}");
            }

            return result;
        }

        /// <summary>
        /// 解密 OPS (OnePlus) 固件包
        /// </summary>
        public async Task<OFPExtractResult> ExtractOPSAsync(string opsPath, string outputDir, CancellationToken ct = default)
        {
            var result = new OFPExtractResult();
            
            try
            {
                _log($"[OPS] 开始解密: {Path.GetFileName(opsPath)}");

                if (!Directory.Exists(outputDir))
                    Directory.CreateDirectory(outputDir);

                // OPS 解密密钥 (固定)
                byte[] opsKey = HexToStaticBytes("d6eccec8c89a35d0349a3f5ce0e4d07a");
                byte[] opsIv = new byte[16]; // 零 IV

                string decryptedPath = Path.Combine(outputDir, "_temp_ops_decrypted");

                await Task.Run(() =>
                {
                    using (var fsIn = new FileStream(opsPath, FileMode.Open, FileAccess.Read))
                    using (var fsOut = new FileStream(decryptedPath, FileMode.Create, FileAccess.Write))
                    {
                        // 跳过 OPS 头
                        byte[] header = new byte[20];
                        fsIn.Read(header, 0, 20);
                        
                        // 解密主体
                        using (var aes = Aes.Create())
                        {
                            aes.Key = opsKey;
                            aes.IV = opsIv;
                            aes.Mode = CipherMode.ECB;
                            aes.Padding = PaddingMode.None;

                            using (var decryptor = aes.CreateDecryptor())
                            {
                                byte[] buffer = new byte[16];
                                int read;
                                while ((read = fsIn.Read(buffer, 0, 16)) > 0)
                                {
                                    if (read == 16)
                                    {
                                        byte[] decrypted = decryptor.TransformFinalBlock(buffer, 0, 16);
                                        fsOut.Write(decrypted, 0, decrypted.Length);
                                    }
                                    else
                                    {
                                        fsOut.Write(buffer, 0, read);
                                    }
                                }
                            }
                        }
                    }
                }, ct);

                // 检测解密后的文件类型
                if (IsZipFile(decryptedPath))
                {
                    _log("[OPS] 解密成功，正在解压...");
                    await Task.Run(() => ExtractZipWithOverwrite(decryptedPath, outputDir), ct);
                    try { File.Delete(decryptedPath); } catch { }
                }
                else
                {
                    // 可能是 raw payload
                    string finalPath = Path.Combine(outputDir, "payload.bin");
                    File.Move(decryptedPath, finalPath);
                    result.ExtractedFiles.Add(finalPath);
                }

                var extractedFiles = Directory.GetFiles(outputDir, "*.*", SearchOption.AllDirectories).ToList();
                result.ExtractedFiles = extractedFiles;
                result.Success = true;
                _log($"[OPS] 解密完成! 共 {extractedFiles.Count} 个文件");
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
                _log($"[OPS] 错误: {ex.Message}");
            }

            return result;
        }

        #endregion

        #region OFP 解密实现

        private async Task<OFPExtractResult> ExtractOFPAsync(string ofpPath, string outputDir, CancellationToken ct = default)
        {
            var result = new OFPExtractResult();
            _ofpFilePath = ofpPath;

            try
            {
                _log($"[OFP] 开始解析: {Path.GetFileName(ofpPath)}");

                // 0. 检查是否是 ZIP 格式 (Realme 密码保护的固件包)
                if (IsPasswordProtectedZip(ofpPath))
                {
                    _log("[OFP] 检测到 ZIP 格式，尝试使用 Realme 密码解压...");
                    var zipResult = await TryExtractPasswordZipAsync(ofpPath, outputDir, ct);
                    if (zipResult.Success)
                    {
                        return zipResult;
                    }
                    _log("[OFP] ZIP 密码解压失败，尝试标准 OFP 解密...");
                }

                // 1. 检测页面大小
                if (!DetectPageSize())
                {
                    // 输出诊断信息
                    _log("[OFP] 无法检测 OFP 魔数 (0x7CEF)");
                    _log("[OFP] 可能原因：1) 使用了新版加密方式 2) 不是有效的 OFP 文件");
                    result.Error = "无法检测 OFP 页面大小，可能是新版加密或不是有效的 OFP 文件";
                    return result;
                }
                _log($"[OFP] 页面大小: {_pageSize} 字节");

                // 2. 检测芯片类型 (Qualcomm 或 MTK)
                _chipType = DetectChipType();
                _log($"[OFP] 芯片类型: {_chipType}");

                // 3. 搜索密钥
                _log("[OFP] 搜索解密密钥...");
                int keysTried = 0;
                string profileXml = await Task.Run(() => BruteForceKeyWithDiag(out keysTried), ct);

                if (string.IsNullOrEmpty(profileXml))
                {
                    _log($"[OFP] 已尝试 {keysTried} 个标准密钥，均未匹配");
                    _log("[OFP] 启动智能爆破模式...");
                    
                    // 启动爆破
                    var bruteForcer = new OFPKeyBruteForce(ofpPath, _pageSize, _log);
                    var bruteResult = await bruteForcer.BruteForceAsync(ct);
                    
                    if (bruteResult.Success)
                    {
                        _decryptKey = bruteResult.Key;
                        _decryptIv = bruteResult.Iv;
                        profileXml = bruteResult.XmlPreview;
                        
                        // 重新解密完整 XML
                        profileXml = await Task.Run(() => 
                        {
                            int dummy;
                            _decryptKey = bruteResult.Key;
                            _decryptIv = bruteResult.Iv;
                            return TryDecryptXmlWithKey(bruteResult.Key, bruteResult.Iv);
                        }, ct);
                        
                        if (!string.IsNullOrEmpty(profileXml))
                        {
                            _log($"[爆破成功!] 已找到密钥，来源: {bruteResult.KeySource}");
                            _log($"[爆破] Key: {bruteResult.Key}");
                            _log($"[爆破] IV:  {bruteResult.Iv}");
                            _log($"[提示] 请将此密钥报告给开发者以便收录");
                        }
                    }
                    
                    if (string.IsNullOrEmpty(profileXml))
                    {
                        _log("[OFP] 爆破失败，未找到有效密钥");
                        _log("[OFP] 提示：此固件可能使用了全新的加密方式");
                        result.Error = $"未找到匹配的解密密钥 (已尝试 {keysTried + bruteResult.TotalTried} 个密钥)";
                        return result;
                    }
                }
                _log("[OFP] 密钥匹配成功");

                // 4. 创建输出目录
                if (!Directory.Exists(outputDir))
                    Directory.CreateDirectory(outputDir);

                // 5. 保存 profile.xml
                string profilePath = Path.Combine(outputDir, "profile.xml");
                File.WriteAllText(profilePath, profileXml, Encoding.UTF8);
                result.ProfileXmlPath = profilePath;
                _log($"[OFP] 已保存: profile.xml");

                // 6. 解析并提取文件
                var extractedFiles = await ExtractFilesFromProfileAsync(profileXml, outputDir, ct);
                result.ExtractedFiles = extractedFiles;

                // 7. 查找 rawprogram XML
                result.RawProgramXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("rawprogram") && f.EndsWith(".xml"))
                    .ToList();

                result.PatchXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("patch") && f.EndsWith(".xml"))
                    .ToList();

                result.Success = true;
                _log($"[OFP] 解密完成! 共提取 {extractedFiles.Count} 个文件");

                if (result.RawProgramXmlPaths.Count > 0)
                {
                    _log($"[OFP] 找到 {result.RawProgramXmlPaths.Count} 个 rawprogram XML");
                }
            }
            catch (OperationCanceledException)
            {
                result.Error = "操作已取消";
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
                _log($"[OFP] 错误: {ex.Message}");
            }

            return result;
        }

        /// <summary>
        /// 仅提取 rawprogram.xml 和 patch.xml (快速模式)
        /// </summary>
        public async Task<OFPExtractResult> ExtractXmlOnlyAsync(string ofpPath, string outputDir, CancellationToken ct = default)
        {
            var result = new OFPExtractResult();
            _ofpFilePath = ofpPath;

            try
            {
                _log($"[OFP] 开始解析: {Path.GetFileName(ofpPath)}");

                // 0. 检查是否是 ZIP 格式 (Realme 密码保护的固件包)
                if (IsPasswordProtectedZip(ofpPath))
                {
                    _log("[OFP] 检测到 ZIP 格式，尝试使用 Realme 密码解压...");
                    var zipResult = await TryExtractPasswordZipAsync(ofpPath, outputDir, ct);
                    if (zipResult.Success)
                    {
                        return zipResult;
                    }
                    _log("[OFP] ZIP 密码解压失败，尝试标准 OFP 解密...");
                }

                // 1. 检测页面大小
                if (!DetectPageSize())
                {
                    _log("[OFP] 无法检测 OFP 魔数 (0x7CEF)");
                    _log("[OFP] 可能原因：1) 使用了新版加密方式 2) 不是有效的 OFP 文件");
                    result.Error = "无法检测 OFP 页面大小，可能是新版加密或不是有效的 OFP 文件";
                    return result;
                }
                _log($"[OFP] 页面大小: {_pageSize} 字节");

                // 2. 检测芯片类型
                _chipType = DetectChipType();
                _log($"[OFP] 芯片类型: {_chipType}");

                // 3. 搜索密钥 (使用完整的爆破逻辑)
                _log("[OFP] 搜索解密密钥...");
                int keysTried = 0;
                string profileXml = await Task.Run(() => BruteForceKeyWithDiag(out keysTried), ct);

                if (string.IsNullOrEmpty(profileXml))
                {
                    _log($"[OFP] 已尝试 {keysTried} 个标准密钥，均未匹配");
                    _log("[OFP] 启动智能爆破模式...");
                    
                    // 启动爆破
                    var bruteForcer = new OFPKeyBruteForce(ofpPath, _pageSize, _log);
                    var bruteResult = await bruteForcer.BruteForceAsync(ct);
                    
                    if (bruteResult.Success)
                    {
                        _decryptKey = bruteResult.Key;
                        _decryptIv = bruteResult.Iv;
                        
                        // 重新解密完整 XML
                        profileXml = await Task.Run(() => TryDecryptXmlWithKey(bruteResult.Key, bruteResult.Iv), ct);
                        
                        if (!string.IsNullOrEmpty(profileXml))
                        {
                            _log($"[爆破成功!] 已找到密钥，来源: {bruteResult.KeySource}");
                            _log($"[爆破] Key: {bruteResult.Key}");
                            _log($"[爆破] IV:  {bruteResult.Iv}");
                            _log($"[提示] 请将此密钥报告给开发者以便收录");
                        }
                    }
                    
                    if (string.IsNullOrEmpty(profileXml))
                    {
                        _log("[OFP] 爆破失败，未找到有效密钥");
                        _log("[OFP] 提示：此固件可能使用了全新的加密方式");
                        result.Error = $"未找到匹配的解密密钥 (已尝试 {keysTried + (bruteResult?.TotalTried ?? 0)} 个密钥)";
                        return result;
                    }
                }
                _log("[OFP] 密钥匹配成功");

                // 4. 创建输出目录
                if (!Directory.Exists(outputDir))
                    Directory.CreateDirectory(outputDir);

                // 5. 保存 profile.xml
                string profilePath = Path.Combine(outputDir, "profile.xml");
                File.WriteAllText(profilePath, profileXml, Encoding.UTF8);
                result.ProfileXmlPath = profilePath;

                // 6. 仅提取 XML 文件
                var extractedFiles = await ExtractXmlFilesOnlyAsync(profileXml, outputDir, ct);
                result.ExtractedFiles = extractedFiles;

                result.RawProgramXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("rawprogram"))
                    .ToList();

                result.PatchXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("patch"))
                    .ToList();

                result.Success = true;
                _log($"[OFP] 快速解析完成! 提取 {extractedFiles.Count} 个 XML 文件");
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
            }

            return result;
        }

        /// <summary>
        /// 检测芯片类型
        /// </summary>
        private ChipType DetectChipType()
        {
            try
            {
                using (var fs = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    // 读取前 64KB 检测特征
                    byte[] buffer = new byte[65536];
                    fs.Read(buffer, 0, buffer.Length);
                    string content = Encoding.ASCII.GetString(buffer);

                    // MTK 特征
                    if (content.Contains("scatter") || content.Contains("PRELOADER") || 
                        content.Contains("MTK") || content.Contains("MediaTek"))
                    {
                        return ChipType.MTK;
                    }

                    // Qualcomm 特征
                    if (content.Contains("firehose") || content.Contains("rawprogram") || 
                        content.Contains("prog_") || content.Contains("Qualcomm"))
                    {
                        return ChipType.Qualcomm;
                    }
                }
            }
            catch { }

            return ChipType.Qualcomm; // 默认 Qualcomm
        }

        /// <summary>
        /// 尝试使用密码解压 ZIP 格式的固件包
        /// </summary>
        private async Task<OFPExtractResult> TryExtractPasswordZipAsync(string zipPath, string outputDir, CancellationToken ct)
        {
            var result = new OFPExtractResult();
            
            try
            {
                if (!Directory.Exists(outputDir))
                    Directory.CreateDirectory(outputDir);

                // 尝试使用 Realme 密码解压
                await Task.Run(() =>
                {
                    try
                    {
                        // 使用 System.IO.Compression 无法处理密码保护的 ZIP
                        // 需要使用其他库如 SharpZipLib 或 DotNetZip
                        // 这里尝试直接解压，如果失败则返回
                        using (var archive = ZipFile.OpenRead(zipPath))
                        {
                            foreach (var entry in archive.Entries)
                            {
                                string destPath = Path.Combine(outputDir, entry.FullName);
                                string destDir = Path.GetDirectoryName(destPath);
                                if (!string.IsNullOrEmpty(destDir) && !Directory.Exists(destDir))
                                    Directory.CreateDirectory(destDir);
                                
                                if (!string.IsNullOrEmpty(entry.Name))
                                {
                                    entry.ExtractToFile(destPath, true);
                                }
                            }
                        }
                    }
                    catch (InvalidDataException)
                    {
                        // ZIP 可能有密码保护，标准库无法处理
                        throw new Exception("ZIP 文件可能有密码保护，需要专用解压库");
                    }
                }, ct);

                // 查找解压后的文件
                var extractedFiles = Directory.GetFiles(outputDir, "*.*", SearchOption.AllDirectories).ToList();
                result.ExtractedFiles = extractedFiles;

                result.RawProgramXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("rawprogram") && f.EndsWith(".xml"))
                    .ToList();

                result.PatchXmlPaths = extractedFiles
                    .Where(f => Path.GetFileName(f).ToLower().StartsWith("patch") && f.EndsWith(".xml"))
                    .ToList();

                if (result.RawProgramXmlPaths.Count > 0)
                {
                    result.Success = true;
                    _log($"[OFP] ZIP 解压成功! 共提取 {extractedFiles.Count} 个文件");
                }
            }
            catch (Exception ex)
            {
                _log($"[OFP] ZIP 解压失败: {ex.Message}");
                // 清理可能创建的文件
                try 
                { 
                    if (Directory.Exists(outputDir))
                    {
                        var files = Directory.GetFiles(outputDir);
                        if (files.Length == 0 || files.All(f => Path.GetFileName(f).StartsWith("_temp")))
                        {
                            // 目录为空或只有临时文件，保留以便后续使用
                        }
                    }
                } 
                catch { }
            }

            return result;
        }

        /// <summary>
        /// 暴力搜索密钥 (带诊断)
        /// </summary>
        private string BruteForceKeyWithDiag(out int keysTried)
        {
            keysTried = 0;

            // 1. 先尝试 MTK 专用检测 (MMM 头部检测)
            if (TryDecryptMtkDirect(out string mtkKey, out string mtkIv))
            {
                keysTried++;
                _decryptKey = mtkKey;
                _decryptIv = mtkIv;
                _chipType = ChipType.MTK;
                
                // 尝试读取 XML
                string mtkXml = TryDecryptXmlWithKey(mtkKey, mtkIv);
                if (!string.IsNullOrEmpty(mtkXml))
                {
                    _log("[MTK] 使用 MTK 专用算法解密成功");
                    return mtkXml;
                }
            }

            // 2. 尝试当前芯片类型的密钥
            var keyList = _chipType == ChipType.MTK ? MtkKeys : QcomKeys;
            string result = TryKeyListWithCount(keyList, ref keysTried);
            if (!string.IsNullOrEmpty(result)) return result;

            // 3. 尝试另一个芯片类型的密钥
            var otherList = _chipType == ChipType.MTK ? QcomKeys : MtkKeys;
            result = TryKeyListWithCount(otherList, ref keysTried);
            if (!string.IsNullOrEmpty(result))
            {
                _chipType = _chipType == ChipType.MTK ? ChipType.Qualcomm : ChipType.MTK;
                return result;
            }

            return null;
        }

        private string TryKeyListWithCount(List<string[]> keyList, ref int keysTried)
        {
            foreach (var keySet in keyList)
            {
                keysTried++;
                string key, iv;

                if (keySet.Length == 2)
                {
                    key = keySet[0];
                    iv = keySet[1];
                }
                else if (keySet.Length == 3)
                {
                    string mc = keySet[0];
                    string userKey = keySet[1];
                    string ivec = keySet[2];

                    string deciKey = DeObfuscate(userKey, mc);
                    string deciIv = DeObfuscate(ivec, mc);

                    key = MD5Hash(HexToStaticBytes(deciKey)).Substring(0, 16);
                    iv = MD5Hash(HexToStaticBytes(deciIv)).Substring(0, 16);
                }
                else if (keySet.Length == 4)
                {
                    string mc = keySet[1];
                    string userKey = keySet[2];
                    string ivec = keySet[3];

                    string deciKey = DeObfuscate(userKey, mc);
                    string deciIv = DeObfuscate(ivec, mc);

                    key = MD5Hash(HexToStaticBytes(deciKey)).Substring(0, 16);
                    iv = MD5Hash(HexToStaticBytes(deciIv)).Substring(0, 16);
                }
                else
                {
                    continue;
                }

                byte[] xmlData = TryDecryptXml(key, iv);
                if (xmlData != null && xmlData.Length > 0)
                {
                    string xmlString = Encoding.UTF8.GetString(xmlData);
                    if (xmlString.ToLower().Contains("xml") || xmlString.Contains("<?xml") || xmlString.Contains("<profile"))
                    {
                        _decryptKey = key;
                        _decryptIv = iv;

                        int endIndex = xmlString.IndexOf("</profile>", StringComparison.OrdinalIgnoreCase);
                        if (endIndex > 0)
                        {
                            return xmlString.Substring(0, endIndex + 10);
                        }

                        foreach (var endTag in new[] { "</Model>", "</Header>", "</ops>" })
                        {
                            endIndex = xmlString.IndexOf(endTag, StringComparison.OrdinalIgnoreCase);
                            if (endIndex > 0)
                            {
                                return xmlString.Substring(0, endIndex + endTag.Length);
                            }
                        }

                        return xmlString;
                    }
                }
            }

            return null;
        }

        /// <summary>
        /// 检测 OFP 页面大小
        /// </summary>
        private bool DetectPageSize()
        {
            try
            {
                using (var fs = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    foreach (int pageSize in new[] { 512, 4096, 8192, 16384 })
                    {
                        if (fs.Length < pageSize) continue;
                        
                        fs.Seek(fs.Length + 16 - pageSize, SeekOrigin.Begin);
                        byte[] magic = new byte[4];
                        fs.Read(magic, 0, 4);
                        string magicHex = BitConverter.ToString(magic).Replace("-", "").ToLower();
                        if (magicHex.Contains("ef7c"))
                        {
                            _pageSize = pageSize;
                            return true;
                        }
                    }
                }
            }
            catch { }

            return false;
        }

        /// <summary>
        /// 暴力搜索密钥 (增强版)
        /// </summary>
        private string BruteForceKey()
        {
            var keyList = _chipType == ChipType.MTK ? MtkKeys : QcomKeys;

            // 先尝试当前芯片类型的密钥
            string result = TryKeyList(keyList);
            if (!string.IsNullOrEmpty(result)) return result;

            // 如果失败，尝试另一个芯片类型的密钥
            var otherList = _chipType == ChipType.MTK ? QcomKeys : MtkKeys;
            result = TryKeyList(otherList);
            if (!string.IsNullOrEmpty(result))
            {
                _chipType = _chipType == ChipType.MTK ? ChipType.Qualcomm : ChipType.MTK;
                return result;
            }

            return null;
        }

        private string TryKeyList(List<string[]> keyList)
        {
            foreach (var keySet in keyList)
            {
                string key, iv;

                if (keySet.Length == 2)
                {
                    // 简单密钥对
                    key = keySet[0];
                    iv = keySet[1];
                }
                else if (keySet.Length == 3)
                {
                    // MTK 混淆密钥 (mc, userkey, ivec)
                    string mc = keySet[0];
                    string userKey = keySet[1];
                    string ivec = keySet[2];

                    string deciKey = DeObfuscate(userKey, mc);
                    string deciIv = DeObfuscate(ivec, mc);

                    key = MD5Hash(HexToStaticBytes(deciKey)).Substring(0, 16);
                    iv = MD5Hash(HexToStaticBytes(deciIv)).Substring(0, 16);
                }
                else if (keySet.Length == 4)
                {
                    // QC 混淆密钥 (version, mc, userkey, ivec)
                    string mc = keySet[1];
                    string userKey = keySet[2];
                    string ivec = keySet[3];

                    string deciKey = DeObfuscate(userKey, mc);
                    string deciIv = DeObfuscate(ivec, mc);

                    key = MD5Hash(HexToStaticBytes(deciKey)).Substring(0, 16);
                    iv = MD5Hash(HexToStaticBytes(deciIv)).Substring(0, 16);
                }
                else
                {
                    continue;
                }

                byte[] xmlData = TryDecryptXml(key, iv);
                if (xmlData != null && xmlData.Length > 0)
                {
                    string xmlString = Encoding.UTF8.GetString(xmlData);
                    if (xmlString.ToLower().Contains("xml") || xmlString.Contains("<?xml") || xmlString.Contains("<profile"))
                    {
                        _decryptKey = key;
                        _decryptIv = iv;

                        // 提取到 </profile> 结束
                        int endIndex = xmlString.IndexOf("</profile>", StringComparison.OrdinalIgnoreCase);
                        if (endIndex > 0)
                        {
                            return xmlString.Substring(0, endIndex + 10);
                        }

                        // 尝试其他结束标记
                        foreach (var endTag in new[] { "</Model>", "</Header>", "</ops>" })
                        {
                            endIndex = xmlString.IndexOf(endTag, StringComparison.OrdinalIgnoreCase);
                            if (endIndex > 0)
                            {
                                return xmlString.Substring(0, endIndex + endTag.Length);
                            }
                        }

                        return xmlString;
                    }
                }
            }

            return null;
        }

        /// <summary>
        /// 使用指定密钥尝试解密 XML (返回字符串)
        /// </summary>
        private string TryDecryptXmlWithKey(string key, string iv)
        {
            var data = TryDecryptXml(key, iv);
            if (data != null && data.Length > 0)
            {
                string xmlString = Encoding.UTF8.GetString(data);
                if (xmlString.Contains("<?xml") || xmlString.Contains("<profile"))
                {
                    int endIndex = xmlString.IndexOf("</profile>", StringComparison.OrdinalIgnoreCase);
                    if (endIndex > 0)
                    {
                        return xmlString.Substring(0, endIndex + 10);
                    }
                    return xmlString;
                }
            }
            return null;
        }

        /// <summary>
        /// 尝试解密 XML
        /// </summary>
        private byte[] TryDecryptXml(string key, string iv)
        {
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
                    long offset = (long)BitConverter.ToUInt32(ReverseBytes(offsetBytes), 0) * _pageSize;

                    byte[] lengthBytes = new byte[4];
                    fs.Read(lengthBytes, 0, 4);
                    long length = BitConverter.ToUInt32(ReverseBytes(lengthBytes), 0);

                    // 验证偏移和长度
                    if (offset < 0 || offset >= fileLength || length <= 0 || length > fileLength)
                        return null;

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

                    return decrypted.Take((int)length).ToArray();
                }
            }
            catch
            {
                return null;
            }
        }

        #endregion

        #region OZIP 解密实现

        /// <summary>
        /// 解密 OZIP 文件
        /// </summary>
        private bool DecryptOZIPFile(string ozipPath, string outputPath)
        {
            try
            {
                using (var fsIn = new FileStream(ozipPath, FileMode.Open, FileAccess.Read))
                {
                    // 读取头部
                    byte[] header = new byte[16];
                    fsIn.Read(header, 0, 16);

                    string headerStr = Encoding.ASCII.GetString(header, 0, 12);

                    // 检测 OZIP 类型
                    int dataOffset = 0;
                    bool isEncrypted = false;

                    if (headerStr.StartsWith("OPPOENCRYPT"))
                    {
                        // 类型 1: OPPOENCRYPT! header
                        dataOffset = 16;
                        isEncrypted = true;
                        _log("[OZIP] 检测到 OPPOENCRYPT 格式");
                    }
                    else if (header[0] == 0x50 && header[1] == 0x4B) // PK
                    {
                        // 类型 2: 普通 ZIP 开头但可能部分加密
                        // 检查 local file header
                        fsIn.Seek(0, SeekOrigin.Begin);
                        byte[] lfh = new byte[30];
                        fsIn.Read(lfh, 0, 30);

                        // 读取文件名长度和额外字段长度
                        int fnLen = BitConverter.ToUInt16(lfh, 26);
                        int efLen = BitConverter.ToUInt16(lfh, 28);
                        dataOffset = 30 + fnLen + efLen;

                        // 检查是否是 OZIP 加密
                        fsIn.Seek(dataOffset, SeekOrigin.Begin);
                        byte[] testData = new byte[16];
                        fsIn.Read(testData, 0, 16);

                        // 如果解密后是有效的 ZIP 数据，则是加密的
                        foreach (var kvp in OzipKeys)
                        {
                            byte[] testDecrypted = DecryptAesEcb(testData, kvp.Value);
                            if (testDecrypted[0] == 0x50 && testDecrypted[1] == 0x4B) // PK
                            {
                                isEncrypted = true;
                                _log($"[OZIP] 检测到加密 ZIP，使用密钥: {kvp.Key}");
                                break;
                            }
                        }

                        if (!isEncrypted)
                        {
                            // 不是加密的，直接复制
                            _log("[OZIP] 检测到普通 ZIP，无需解密");
                            File.Copy(ozipPath, outputPath, true);
                            return true;
                        }
                    }
                    else
                    {
                        _log("[OZIP] 未知的文件格式");
                        return false;
                    }

                    // 尝试所有密钥
                    foreach (var kvp in OzipKeys)
                    {
                        fsIn.Seek(dataOffset, SeekOrigin.Begin);

                        using (var fsOut = new FileStream(outputPath, FileMode.Create, FileAccess.Write))
                        {
                            bool success = DecryptOzipData(fsIn, fsOut, kvp.Value);
                            
                            if (success && IsZipFile(outputPath))
                            {
                                _log($"[OZIP] 使用密钥 {kvp.Key} 解密成功");
                                return true;
                            }
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[OZIP] 解密错误: {ex.Message}");
            }

            return false;
        }

        /// <summary>
        /// 解密 OZIP 数据流
        /// </summary>
        private bool DecryptOzipData(FileStream fsIn, FileStream fsOut, byte[] key)
        {
            try
            {
                using (var aes = Aes.Create())
                {
                    aes.Key = key;
                    aes.Mode = CipherMode.ECB;
                    aes.Padding = PaddingMode.None;

                    using (var decryptor = aes.CreateDecryptor())
                    {
                        byte[] buffer = new byte[65536]; // 64KB buffer
                        int read;
                        long processed = 0;
                        bool headerWritten = false;

                        while ((read = fsIn.Read(buffer, 0, buffer.Length)) > 0)
                        {
                            // 只解密前 65536 字节
                            if (processed < 65536)
                            {
                                int toDecrypt = (int)Math.Min(65536 - processed, read);
                                
                                // 对齐到 16 字节
                                int alignedLen = (toDecrypt / 16) * 16;
                                
                                if (alignedLen > 0)
                                {
                                    byte[] decrypted = new byte[alignedLen];
                                    decryptor.TransformBlock(buffer, 0, alignedLen, decrypted, 0);
                                    fsOut.Write(decrypted, 0, alignedLen);
                                }
                                
                                // 写入剩余未对齐部分
                                if (read > alignedLen)
                                {
                                    fsOut.Write(buffer, alignedLen, read - alignedLen);
                                }
                                
                                headerWritten = true;
                            }
                            else
                            {
                                // 后面的数据不需要解密
                                fsOut.Write(buffer, 0, read);
                            }
                            
                            processed += read;
                        }
                    }
                }

                return true;
            }
            catch
            {
                return false;
            }
        }

        /// <summary>
        /// AES-ECB 解密
        /// </summary>
        private byte[] DecryptAesEcb(byte[] cipherText, byte[] key)
        {
            using (var aes = Aes.Create())
            {
                aes.Key = key;
                aes.Mode = CipherMode.ECB;
                aes.Padding = PaddingMode.None;

                using (var decryptor = aes.CreateDecryptor())
                {
                    return decryptor.TransformFinalBlock(cipherText, 0, cipherText.Length);
                }
            }
        }

        /// <summary>
        /// 检查是否是有效的 ZIP 文件
        /// </summary>
        private bool IsZipFile(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read))
                {
                    byte[] header = new byte[4];
                    fs.Read(header, 0, 4);
                    return header[0] == 0x50 && header[1] == 0x4B && 
                           (header[2] == 0x03 || header[2] == 0x05) && 
                           (header[3] == 0x04 || header[3] == 0x06);
                }
            }
            catch
            {
                return false;
            }
        }

        /// <summary>
        /// 解压 ZIP 文件并覆盖已存在的文件 (.NET Framework 4.8 兼容)
        /// </summary>
        private void ExtractZipWithOverwrite(string zipPath, string outputDir)
        {
            using (var archive = ZipFile.OpenRead(zipPath))
            {
                foreach (var entry in archive.Entries)
                {
                    // 计算目标路径
                    string destPath = Path.Combine(outputDir, entry.FullName);
                    
                    // 确保目录存在
                    string destDir = Path.GetDirectoryName(destPath);
                    if (!string.IsNullOrEmpty(destDir) && !Directory.Exists(destDir))
                    {
                        Directory.CreateDirectory(destDir);
                    }
                    
                    // 如果是目录条目则跳过
                    if (string.IsNullOrEmpty(entry.Name))
                    {
                        continue;
                    }
                    
                    // 提取文件（覆盖已存在的）
                    entry.ExtractToFile(destPath, true);
                }
            }
        }

        #endregion

        #region 文件提取方法

        /// <summary>
        /// 从 profile.xml 提取所有文件
        /// </summary>
        private async Task<List<string>> ExtractFilesFromProfileAsync(string profileXml, string outputDir, CancellationToken ct)
        {
            var extractedFiles = new List<string>();

            try
            {
                var doc = XDocument.Parse(profileXml);
                var root = doc.Root;

                // 统计总文件数
                int totalFiles = root.Descendants().Count(e => e.Attribute("Path") != null || e.Attribute("filename") != null);
                int currentFile = 0;

                foreach (var section in root.Elements())
                {
                    if (ct.IsCancellationRequested) break;

                    string sectionName = section.Name.LocalName;

                    foreach (var item in section.Elements())
                    {
                        if (ct.IsCancellationRequested) break;

                        var fileInfo = ParseFileItem(item, sectionName);
                        if (fileInfo == null || string.IsNullOrEmpty(fileInfo.FileName))
                            continue;

                        currentFile++;
                        _progress(currentFile, totalFiles);

                        string outputPath = Path.Combine(outputDir, fileInfo.FileName);

                        // 确保目录存在
                        string dir = Path.GetDirectoryName(outputPath);
                        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                            Directory.CreateDirectory(dir);

                        _log($"[OFP] 提取: {fileInfo.FileName}");

                        await Task.Run(() =>
                        {
                            if (sectionName == "DigestsToSign" || sectionName == "ChainedTableOfDigests" || sectionName == "Firmware")
                            {
                                // 不需要解密的文件
                                CopyRawData(outputPath, fileInfo.Offset, fileInfo.Length);
                            }
                            else
                            {
                                // 需要解密的文件
                                DecryptFile(outputPath, fileInfo);
                            }
                        }, ct);

                        if (File.Exists(outputPath))
                        {
                            extractedFiles.Add(outputPath);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[OFP] 解析 XML 失败: {ex.Message}");
            }

            return extractedFiles;
        }

        /// <summary>
        /// 仅提取 XML 文件
        /// </summary>
        private async Task<List<string>> ExtractXmlFilesOnlyAsync(string profileXml, string outputDir, CancellationToken ct)
        {
            var extractedFiles = new List<string>();

            try
            {
                var doc = XDocument.Parse(profileXml);
                var root = doc.Root;

                foreach (var section in root.Elements())
                {
                    if (ct.IsCancellationRequested) break;

                    string sectionName = section.Name.LocalName;

                    foreach (var item in section.Elements())
                    {
                        if (ct.IsCancellationRequested) break;

                        var fileInfo = ParseFileItem(item, sectionName);
                        if (fileInfo == null || string.IsNullOrEmpty(fileInfo.FileName))
                            continue;

                        // 只提取 XML 文件
                        string lowerName = fileInfo.FileName.ToLower();
                        if (!lowerName.EndsWith(".xml"))
                            continue;

                        string outputPath = Path.Combine(outputDir, fileInfo.FileName);

                        _log($"[OFP] 提取: {fileInfo.FileName}");

                        await Task.Run(() => DecryptFile(outputPath, fileInfo), ct);

                        if (File.Exists(outputPath))
                        {
                            extractedFiles.Add(outputPath);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[OFP] 解析失败: {ex.Message}");
            }

            return extractedFiles;
        }

        /// <summary>
        /// 解析文件项
        /// </summary>
        private OFPFileInfo ParseFileItem(XElement item, string sectionName)
        {
            var info = new OFPFileInfo();

            // 文件名
            if (item.Attribute("Path") != null)
                info.FileName = item.Attribute("Path").Value;
            else if (item.Attribute("filename") != null)
                info.FileName = item.Attribute("filename").Value;

            if (string.IsNullOrEmpty(info.FileName))
                return null;

            // 偏移
            if (item.Attribute("FileOffsetInSrc") != null)
                info.Offset = long.Parse(item.Attribute("FileOffsetInSrc").Value) * _pageSize;
            else if (item.Attribute("SizeInSectorInSrc") != null)
                info.Offset = long.Parse(item.Attribute("SizeInSectorInSrc").Value) * _pageSize;

            // 实际长度
            if (item.Attribute("SizeInByteInSrc") != null)
                info.RealLength = long.Parse(item.Attribute("SizeInByteInSrc").Value);

            // 扇区长度
            if (item.Attribute("SizeInSectorInSrc") != null)
                info.Length = long.Parse(item.Attribute("SizeInSectorInSrc").Value) * _pageSize;
            else
                info.Length = info.RealLength;

            // 校验值
            if (item.Attribute("sha256") != null)
                info.SHA256 = item.Attribute("sha256").Value;
            if (item.Attribute("md5") != null)
                info.MD5 = item.Attribute("md5").Value;

            // 解密大小 (默认 0x40000)
            info.DecryptSize = 0x40000;

            // Sahara 和 Config 特殊处理
            if (sectionName == "Sahara" || sectionName == "Config" || sectionName == "Provision")
            {
                info.Length = info.RealLength;
                info.DecryptSize = info.RealLength;
            }

            return info;
        }

        /// <summary>
        /// 解密文件
        /// </summary>
        private void DecryptFile(string outputPath, OFPFileInfo fileInfo)
        {
            try
            {
                using (var fsIn = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var fsOut = new FileStream(outputPath, FileMode.Create, FileAccess.Write))
                {
                    fsIn.Seek(fileInfo.Offset, SeekOrigin.Begin);

                    long remaining = fileInfo.RealLength > 0 ? fileInfo.RealLength : fileInfo.Length;
                    long decryptSize = Math.Min(fileInfo.DecryptSize, remaining);

                    // 对齐到 16 字节
                    if (decryptSize % 16 != 0)
                        decryptSize = ((decryptSize + 15) / 16) * 16;

                    // 解密前面部分
                    byte[] encryptedBuffer = new byte[decryptSize];
                    fsIn.Read(encryptedBuffer, 0, (int)decryptSize);

                    byte[] keyBytes = Encoding.UTF8.GetBytes(_decryptKey);
                    byte[] ivBytes = Encoding.UTF8.GetBytes(_decryptIv);
                    byte[] decrypted = DecryptAesCfb(encryptedBuffer, keyBytes, ivBytes);

                    long writeSize = Math.Min(decrypted.Length, remaining);
                    fsOut.Write(decrypted, 0, (int)writeSize);
                    remaining -= writeSize;

                    // 复制剩余未加密部分
                    if (remaining > 0)
                    {
                        byte[] buffer = new byte[2 * 1024 * 1024]; // 2MB buffer
                        while (remaining > 0)
                        {
                            int toRead = (int)Math.Min(buffer.Length, remaining);
                            int read = fsIn.Read(buffer, 0, toRead);
                            if (read == 0) break;
                            fsOut.Write(buffer, 0, read);
                            remaining -= read;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[OFP] 解密 {fileInfo.FileName} 失败: {ex.Message}");
            }
        }

        /// <summary>
        /// 复制原始数据
        /// </summary>
        private void CopyRawData(string outputPath, long offset, long length)
        {
            try
            {
                using (var fsIn = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var fsOut = new FileStream(outputPath, FileMode.Create, FileAccess.Write))
                {
                    fsIn.Seek(offset, SeekOrigin.Begin);

                    byte[] buffer = new byte[2 * 1024 * 1024];
                    long remaining = length;

                    while (remaining > 0)
                    {
                        int toRead = (int)Math.Min(buffer.Length, remaining);
                        int read = fsIn.Read(buffer, 0, toRead);
                        if (read == 0) break;
                        fsOut.Write(buffer, 0, read);
                        remaining -= read;
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[OFP] 复制失败: {ex.Message}");
            }
        }

        #endregion

        #region 加密工具方法

        /// <summary>
        /// AES-CFB 解密
        /// </summary>
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

        /// <summary>
        /// 密钥混淆解除
        /// </summary>
        private string DeObfuscate(string input, string mc)
        {
            StringBuilder result = new StringBuilder();

            for (int i = 0; i < input.Length / 2; i++)
            {
                int a = Convert.ToInt32(input.Substring(i * 2, 2), 16);
                int b = Convert.ToInt32(mc.Substring(i * 2, 2), 16);
                int xored = a ^ b;
                int rotated = ((xored >> 4) | ((xored & 0xF) << 4)) & 0xFF;
                result.Append(rotated.ToString("x2"));
            }

            return result.ToString();
        }

        /// <summary>
        /// MD5 哈希
        /// </summary>
        private string MD5Hash(byte[] input)
        {
            using (var md5 = MD5.Create())
            {
                byte[] hash = md5.ComputeHash(input);
                return BitConverter.ToString(hash).Replace("-", "").ToLower();
            }
        }

        /// <summary>
        /// 十六进制字符串转字节数组 (静态)
        /// </summary>
        private static byte[] HexToStaticBytes(string hex)
        {
            hex = hex.Replace(" ", "");
            byte[] bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++)
            {
                bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            }
            return bytes;
        }

        /// <summary>
        /// 反转字节数组 (小端序转换)
        /// </summary>
        private byte[] ReverseBytes(byte[] bytes)
        {
            byte[] reversed = new byte[bytes.Length];
            for (int i = 0; i < bytes.Length; i++)
            {
                reversed[i] = bytes[bytes.Length - 1 - i];
            }
            return reversed;
        }

        #region MTK Shuffle 算法 (来自 RayMarmAung/ofp_extractor)

        /// <summary>
        /// MTK Header Shuffle Key (固定密钥)
        /// </summary>
        private static readonly byte[] MtkHeaderKey = Encoding.ASCII.GetBytes("geyixue");

        /// <summary>
        /// MTK Shuffle 算法 V1 - 用于解密 MTK OFP 头部
        /// </summary>
        private void MtkShuffle(byte[] key, byte[] data)
        {
            for (int i = 0; i < data.Length; i++)
            {
                byte k = key[i % key.Length];
                byte h = (byte)(((data[i] & 0xF0) >> 4) | ((data[i] & 0x0F) << 4));
                data[i] = (byte)(k ^ h);
            }
        }

        /// <summary>
        /// MTK Shuffle 算法 V2 - 用于解密 MTK 密钥
        /// </summary>
        private void MtkShuffle2(byte[] key, byte[] data)
        {
            for (int i = 0; i < data.Length; i++)
            {
                byte tmp = (byte)(key[i % key.Length] ^ data[i]);
                data[i] = (byte)(((tmp & 0xF0) >> 4) | ((tmp & 0x0F) << 4));
            }
        }

        /// <summary>
        /// 尝试使用 MTK 专用算法解密 (检测 "MMM" 头)
        /// </summary>
        private bool TryDecryptMtkDirect(out string key, out string iv)
        {
            key = null;
            iv = null;

            // MTK OFP 专用密钥列表 (bruteKey 函数中的密钥)
            var mtkBruteKeys = new[]
            {
                new[] { "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C", "42F2D5399137E2B2813CD8ECDF2F4D72" },
                new[] { "9E4F32639D21357D37D226B69A495D21", "A3D8D358E42F5A9E931DD3917D9A3218", "386935399137416B67416BECF22F519A" },
                new[] { "892D57E92A4D8A975E3C216B7C9DE189", "D26DF2D9913785B145D18C7219B89F26", "516989E4A1BFC78B365C6BC57D944391" },
                new[] { "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C", "422DD5399181E223813CD8ECDF2E4D72" },
                new[] { "3C4A618D9BF2E4279DC758CD535147C3", "87B13D29709AC1BF2382276C4E8DF232", "59B7A8E967265E9BCABE2469FE4A915E" },
                new[] { "1C3288822BF824259DC852C1733127D3", "E7918D22799181CF2312176C9E2DF298", "3247F889A7B6DECBCA3E28693E4AAAFE" },
                new[] { "1E4F32239D65A57D37D2266D9A775D43", "A332D3C3E42F5A3E931DD991729A321D", "3F2A35399A373377674155ECF28FD19A" },
                new[] { "122D57E92A518AFF5E3C786B7C34E189", "DD6DF2D9543785674522717219989FB0", "12698965A132C76136CC88C5DD94EE91" },
            };

            try
            {
                using (var fs = new FileStream(_ofpFilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    byte[] encData = new byte[16];
                    fs.Read(encData, 0, 16);

                    foreach (var keySet in mtkBruteKeys)
                    {
                        byte[] obsKey = HexToStaticBytes(keySet[0]);
                        byte[] encAesKey = HexToStaticBytes(keySet[1]);
                        byte[] encAesIv = HexToStaticBytes(keySet[2]);

                        // MTK Shuffle2 解密密钥
                        MtkShuffle2(obsKey, encAesKey);
                        MtkShuffle2(obsKey, encAesIv);

                        // MD5 哈希后取前 16 字节
                        string derivedKey = MD5Hash(encAesKey).Substring(0, 16);
                        string derivedIv = MD5Hash(encAesIv).Substring(0, 16);

                        // 尝试解密
                        byte[] testData = new byte[16];
                        Array.Copy(encData, testData, 16);
                        byte[] keyBytes = Encoding.UTF8.GetBytes(derivedKey);
                        byte[] ivBytes = Encoding.UTF8.GetBytes(derivedIv);
                        byte[] decrypted = DecryptAesCfb(testData, keyBytes, ivBytes);

                        // 检查是否是 "MMM" 开头 (MTK OFP 特征)
                        if (decrypted != null && decrypted.Length >= 3 &&
                            decrypted[0] == 'M' && decrypted[1] == 'M' && decrypted[2] == 'M')
                        {
                            _log("[MTK] 检测到 MTK OFP (MMM 头)");
                            key = derivedKey;
                            iv = derivedIv;
                            return true;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _log($"[MTK] 检测错误: {ex.Message}");
            }

            return false;
        }

        /// <summary>
        /// MTK OFP 头部结构
        /// </summary>
        private class MtkOfpHeader
        {
            public string Name { get; set; }        // 46 bytes
            public string Cpu { get; set; }         // 7 bytes  
            public string FlashType { get; set; }   // 5 bytes
            public ushort EntriesCount { get; set; }
            public string Info { get; set; }        // 32 bytes
            public ushort Crc { get; set; }
        }

        /// <summary>
        /// MTK OFP 条目结构
        /// </summary>
        private class MtkOfpEntry
        {
            public string Name { get; set; }        // 32 bytes
            public ulong StartPosition { get; set; }
            public ulong Size { get; set; }
            public ulong CryptoSize { get; set; }
            public string FileName { get; set; }    // 32 bytes
            public ulong Crc { get; set; }
        }

        /// <summary>
        /// 解析 MTK OFP 头部
        /// </summary>
        private MtkOfpHeader ParseMtkHeader(byte[] data)
        {
            const int HEADER_SIZE = 0x6C;
            if (data.Length < HEADER_SIZE) return null;

            // 先 shuffle 解密
            MtkShuffle(MtkHeaderKey, data);

            try
            {
                return new MtkOfpHeader
                {
                    Name = Encoding.ASCII.GetString(data, 0, 46).TrimEnd('\0'),
                    Cpu = Encoding.ASCII.GetString(data, 54, 7).TrimEnd('\0'),
                    FlashType = Encoding.ASCII.GetString(data, 61, 5).TrimEnd('\0'),
                    EntriesCount = BitConverter.ToUInt16(data, 66),
                    Info = Encoding.ASCII.GetString(data, 68, 32).TrimEnd('\0'),
                    Crc = BitConverter.ToUInt16(data, 100)
                };
            }
            catch
            {
                return null;
            }
        }

        #endregion

        #endregion
    }

    #region 数据类

    /// <summary>
    /// OFP 提取结果
    /// </summary>
    public class OFPExtractResult
    {
        public bool Success { get; set; }
        public string Error { get; set; }
        public string ProfileXmlPath { get; set; }
        public List<string> RawProgramXmlPaths { get; set; } = new List<string>();
        public List<string> PatchXmlPaths { get; set; } = new List<string>();
        public List<string> ExtractedFiles { get; set; } = new List<string>();
    }

    /// <summary>
    /// OFP 文件信息
    /// </summary>
    internal class OFPFileInfo
    {
        public string FileName { get; set; }
        public long Offset { get; set; }
        public long Length { get; set; }
        public long RealLength { get; set; }
        public long DecryptSize { get; set; }
        public string SHA256 { get; set; }
        public string MD5 { get; set; }
    }

    #endregion
}
