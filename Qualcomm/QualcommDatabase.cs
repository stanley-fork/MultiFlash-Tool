using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace OPFlashTool.Qualcomm
{
    public enum MemoryType
    {
        Unknown = -1,
        Nand = 0,
        Emmc = 1,
        Ufs = 2,
        Spinor = 3
    }

    /// <summary>
    /// 高通芯片数据库 (扩展版)
    /// 包含 HWID、PK Hash、加载器匹配等功能
    /// </summary>
    public static class QualcommDatabase
    {
        #region HWID -> 芯片名称映射

        public static readonly Dictionary<uint, string> MsmIds = new Dictionary<uint, string>
        {
            // === 旗舰 8 系列 ===
            { 0x009470E1, "MSM8996" },   // Snapdragon 820/821
            { 0x0005E0E1, "MSM8998" },   // Snapdragon 835
            { 0x0008B0E1, "SDM845" },    // Snapdragon 845
            { 0x000A50E1, "SM8150" },    // Snapdragon 855/855+
            { 0x000C30E1, "SM8250" },    // Snapdragon 865/865+
            { 0x001350E1, "SM8350" },    // Snapdragon 888/888+
            { 0x001620E1, "SM8450" },    // Snapdragon 8 Gen 1
            { 0x001900E1, "SM8475" },    // Snapdragon 8+ Gen 1
            { 0x001CA0E1, "SM8550" },    // Snapdragon 8 Gen 2
            { 0x0022A0E1, "SM8650" },    // Snapdragon 8 Gen 3
            { 0x002480E1, "SM8750" },    // Snapdragon 8 Elite
            
            // === 中端 7 系列 ===
            { 0x000910E1, "SDM670" },    // Snapdragon 670
            { 0x000DB0E1, "SDM710" },    // Snapdragon 710
            { 0x000E70E1, "SM7150" },    // Snapdragon 730/730G
            { 0x0011E0E1, "SM7250" },    // Snapdragon 765/765G
            { 0x001920E1, "SM7325" },    // Snapdragon 778G/778G+
            { 0x001B40E1, "SM7450" },    // Snapdragon 7 Gen 1
            { 0x001F80E1, "SM7475" },    // Snapdragon 7+ Gen 2
            { 0x002120E1, "SM7550" },    // Snapdragon 7 Gen 3
            
            // === 中端 6 系列 ===
            { 0x000460E1, "MSM8953" },   // Snapdragon 625/626
            { 0x0008C0E1, "SDM660" },    // Snapdragon 660
            { 0x000CC0E1, "SDM636" },    // Snapdragon 636
            { 0x000DA0E1, "SDM632" },    // Snapdragon 632
            { 0x000F50E1, "SM6150" },    // Snapdragon 675
            { 0x00100E01, "SM6125" },    // Snapdragon 665
            { 0x001260E1, "SM6350" },    // Snapdragon 690
            { 0x001410E1, "SM6375" },    // Snapdragon 695
            { 0x001A10E1, "SM6450" },    // Snapdragon 6 Gen 1
            
            // === 入门 4 系列 ===
            { 0x007050E1, "MSM8916" },   // Snapdragon 410/412
            { 0x007B00E1, "MSM8917" },   // Snapdragon 425
            { 0x009600E1, "MSM8909" },   // Snapdragon 210
            { 0x009900E1, "MSM8937" },   // Snapdragon 430
            { 0x009A00E1, "MSM8940" },   // Snapdragon 435
            { 0x000E00E1, "SDM439" },    // Snapdragon 439
            { 0x000F70E1, "SM4250" },    // Snapdragon 460
            { 0x001700E1, "SM4350" },    // Snapdragon 480
            { 0x001D30E1, "SM4450" },    // Snapdragon 4 Gen 1
            { 0x002000E1, "SM4550" },    // Snapdragon 4 Gen 2
            
            // === 老旗舰 ===
            { 0x009400E1, "MSM8994" },   // Snapdragon 810
            { 0x009500E1, "MSM8992" },   // Snapdragon 808
            { 0x009100E1, "MSM8974" },   // Snapdragon 800/801
            { 0x009200E1, "APQ8084" },   // Snapdragon 805
            { 0x008110E1, "MSM8960" },   // Snapdragon S4 Plus
            
            // === PC 平台 ===
            { 0x0014A0E1, "SC8280X" },   // Snapdragon 8cx Gen 3
            { 0x001230E1, "SC8180X" },   // Snapdragon 8cx
            { 0x000F30E1, "SC7180" },    // Snapdragon 7c
            { 0x001550E1, "SC7280" },    // Snapdragon 7c+ Gen 3
            
            // === 未知 ===
            { 0x000000E1, "Unknown_QC" },
        };

        #endregion

        #region 芯片 -> 存储类型映射

        public static readonly Dictionary<string, MemoryType> PreferredMemory = new Dictionary<string, MemoryType>
        {
            // === UFS 芯片 ===
            { "MSM8996", MemoryType.Ufs },
            { "MSM8998", MemoryType.Ufs },
            { "SDM845", MemoryType.Ufs },
            { "SM8150", MemoryType.Ufs },
            { "SM8250", MemoryType.Ufs },
            { "SM8350", MemoryType.Ufs },
            { "SM8450", MemoryType.Ufs },
            { "SM8475", MemoryType.Ufs },
            { "SM8550", MemoryType.Ufs },
            { "SM8650", MemoryType.Ufs },
            { "SM8750", MemoryType.Ufs },
            { "SM7150", MemoryType.Ufs },
            { "SM7250", MemoryType.Ufs },
            { "SM7325", MemoryType.Ufs },
            { "SM7450", MemoryType.Ufs },
            { "SM7475", MemoryType.Ufs },
            { "SM7550", MemoryType.Ufs },
            { "SM6350", MemoryType.Ufs },
            { "SM6375", MemoryType.Ufs },
            { "SM6450", MemoryType.Ufs },
            { "SC8280X", MemoryType.Ufs },
            { "SC8180X", MemoryType.Ufs },
            { "SDM670", MemoryType.Ufs },
            { "SDM710", MemoryType.Ufs },
            { "SM6150", MemoryType.Ufs },
            { "SC7180", MemoryType.Ufs },
            { "SC7280", MemoryType.Ufs },
            
            // === eMMC 芯片 ===
            { "MSM8953", MemoryType.Emmc },
            { "MSM8974", MemoryType.Emmc },
            { "MSM8916", MemoryType.Emmc },
            { "MSM8917", MemoryType.Emmc },
            { "MSM8909", MemoryType.Emmc },
            { "MSM8937", MemoryType.Emmc },
            { "MSM8940", MemoryType.Emmc },
            { "MSM8994", MemoryType.Emmc },
            { "MSM8992", MemoryType.Emmc },
            { "MSM8960", MemoryType.Emmc },
            { "APQ8084", MemoryType.Emmc },
            { "SDM439", MemoryType.Emmc },
            { "SDM632", MemoryType.Emmc },
            { "SM4250", MemoryType.Emmc },
            { "SM4350", MemoryType.Emmc },
            { "SM4450", MemoryType.Emmc },
            { "SM4550", MemoryType.Emmc },
            { "SDM660", MemoryType.Emmc },
            { "SDM636", MemoryType.Emmc },
            { "SM6125", MemoryType.Emmc },
        };

        #endregion

        #region PK Hash 数据库 (用于自动加载器匹配)

        /// <summary>
        /// PK Hash -> 厂商/机型信息
        /// 格式: PKHash (前16字节) -> (厂商, 型号, 建议加载器名称)
        /// </summary>
        public static readonly Dictionary<string, (string Vendor, string Model, string LoaderHint)> PkHashDatabase = new Dictionary<string, (string, string, string)>
        {
            // === OPPO/OnePlus/Realme ===
            { "3C9014A2", ("OPPO", "ColorOS", "prog_firehose_ddr") },
            { "4B4E3B53", ("OPPO", "Reno Series", "prog_firehose_ddr") },
            { "5E4E9C6A", ("OnePlus", "OxygenOS", "prog_firehose_ddr") },
            { "71A2B3C4", ("Realme", "realme UI", "prog_firehose_ddr") },
            
            // === 小米/Redmi/POCO ===
            { "D40EBBF1", ("Xiaomi", "MIUI Global", "xbl_s_devprg_ns") },
            { "E8F2D9A1", ("Xiaomi", "MIUI China", "xbl_s_devprg_ns") },
            { "F1E2D3C4", ("Redmi", "MIUI", "xbl_s_devprg_ns") },
            { "A1B2C3D4", ("POCO", "MIUI", "xbl_s_devprg_ns") },
            
            // === 三星 ===
            { "53414D53", ("Samsung", "OneUI", "prog_firehose_ddr") },
            
            // === 华为/荣耀 ===
            { "48554157", ("Huawei", "EMUI", "prog_emmc_firehose") },
            { "484F4E4F", ("Honor", "MagicUI", "prog_emmc_firehose") },
            
            // === vivo/iQOO ===
            { "5649564F", ("vivo", "FuntouchOS", "prog_firehose_ddr") },
            { "69514F4F", ("iQOO", "OriginOS", "prog_firehose_ddr") },
            
            // === 联想/摩托罗拉 ===
            { "4C454E4F", ("Lenovo", "ZUI", "prog_firehose_ddr") },
            { "4D4F544F", ("Motorola", "MyUX", "prog_firehose_ddr") },
            
            // === 索尼 ===
            { "534F4E59", ("Sony", "Xperia", "prog_firehose_ddr") },
            
            // === LG ===
            { "4C472D45", ("LG", "LG UX", "prog_firehose_ddr") },
            
            // === 中兴/努比亚 ===
            { "5A544520", ("ZTE", "MiFavor", "prog_firehose_ddr") },
            { "4E554249", ("nubia", "nubia UI", "prog_firehose_ddr") },
            
            // === 华硕 ===
            { "41535553", ("ASUS", "ZenUI", "prog_firehose_ddr") },
            
            // === 诺基亚 ===
            { "4E4F4B49", ("Nokia", "Android One", "prog_firehose_ddr") },
            
            // === Google ===
            { "474F4F47", ("Google", "Pixel", "prog_firehose_ddr") },
        };

        #endregion

        #region Sahara 协议版本

        public static readonly Dictionary<string, int> SaharaVersion = new Dictionary<string, int>
        {
            // V3 芯片 (需要手动指定 loader)
            { "SM8450", 3 },
            { "SM8475", 3 },
            { "SM8550", 3 },
            { "SM8650", 3 },
            { "SM8750", 3 },
            { "SM7450", 3 },
            { "SM7475", 3 },
            { "SM7550", 3 },
            { "SM6450", 3 },
            // V2 芯片 (支持自动检测)
            { "SM8350", 2 },
            { "SM8250", 2 },
            { "SM8150", 2 },
            { "SDM845", 2 },
        };

        #endregion

        #region 公开方法

        public static string GetChipName(uint hwId)
        {
            return MsmIds.TryGetValue(hwId, out string name) ? name : $"Unknown_0x{hwId:X8}";
        }

        public static MemoryType GetMemoryType(string chipName)
        {
            return PreferredMemory.TryGetValue(chipName, out MemoryType type) ? type : MemoryType.Ufs;
        }
        
        public static int GetSaharaVersion(string chipName)
        {
            return SaharaVersion.TryGetValue(chipName, out int ver) ? ver : 2;
        }

        /// <summary>
        /// 根据 PK Hash 获取设备信息
        /// </summary>
        /// <param name="pkHash">PK Hash 字节数组 (至少8字节)</param>
        /// <returns>(厂商, 型号, 加载器提示)</returns>
        public static (string Vendor, string Model, string LoaderHint) GetDeviceByPkHash(byte[] pkHash)
        {
            if (pkHash == null || pkHash.Length < 4)
                return ("Unknown", "Unknown", "prog_firehose_ddr");

            // 取前4字节作为简化匹配
            string hashKey = BitConverter.ToString(pkHash, 0, 4).Replace("-", "");
            
            if (PkHashDatabase.TryGetValue(hashKey, out var info))
                return info;

            return ("Unknown", "Unknown", "prog_firehose_ddr");
        }

        /// <summary>
        /// 在指定目录中查找匹配的加载器
        /// </summary>
        /// <param name="loaderDirectory">加载器目录</param>
        /// <param name="msmId">芯片 ID</param>
        /// <param name="pkHash">PK Hash</param>
        /// <returns>加载器完整路径，未找到返回 null</returns>
        public static string FindMatchingLoader(string loaderDirectory, uint msmId, byte[] pkHash)
        {
            if (!Directory.Exists(loaderDirectory))
                return null;

            string chipName = GetChipName(msmId);
            var (vendor, model, loaderHint) = GetDeviceByPkHash(pkHash);
            
            // 搜索模式优先级：
            // 1. 完全匹配: {vendor}_{chipName}_{loaderHint}.mbn
            // 2. 芯片匹配: {chipName}_{loaderHint}.mbn 或 {chipName}_*.mbn
            // 3. 厂商匹配: {vendor}_*.mbn
            // 4. 通用匹配: prog_firehose_*.mbn, xbl_s_devprg_*.mbn

            var searchPatterns = new[]
            {
                $"{vendor}_{chipName}_{loaderHint}*.mbn",
                $"{vendor}_{chipName}*.mbn",
                $"{chipName}_{loaderHint}*.mbn",
                $"{chipName}*.mbn",
                $"{vendor}*.mbn",
                $"{loaderHint}*.mbn",
                "prog_firehose_ddr*.mbn",
                "xbl_s_devprg_ns*.mbn",
                "prog_emmc_firehose*.mbn",
                "*.mbn",
                "*.elf",
            };

            foreach (var pattern in searchPatterns)
            {
                var files = Directory.GetFiles(loaderDirectory, pattern, SearchOption.AllDirectories);
                if (files.Length > 0)
                {
                    // 优先选择较新的文件
                    return files.OrderByDescending(f => new FileInfo(f).LastWriteTime).First();
                }
            }

            return null;
        }

        /// <summary>
        /// 根据 PK Hash 的前缀获取厂商名称 (快速匹配)
        /// </summary>
        public static string GetVendorByPkHashPrefix(string pkHashHex)
        {
            if (string.IsNullOrEmpty(pkHashHex) || pkHashHex.Length < 8)
                return "Unknown";

            string prefix = pkHashHex.Substring(0, 8).ToUpper();
            
            if (PkHashDatabase.TryGetValue(prefix, out var info))
                return info.Vendor;

            // 备用：通过已知特征判断
            if (pkHashHex.Contains("D40EBBF1") || pkHashHex.Contains("E8F2D9A1"))
                return "Xiaomi";
            if (pkHashHex.Contains("3C9014A2") || pkHashHex.Contains("4B4E3B53"))
                return "OPPO";

            return "Unknown";
        }

        /// <summary>
        /// 获取芯片的详细信息字符串
        /// </summary>
        public static string GetChipInfo(uint msmId, byte[] pkHash = null)
        {
            string chipName = GetChipName(msmId);
            MemoryType memType = GetMemoryType(chipName);
            int saharaVer = GetSaharaVersion(chipName);
            
            string info = $"芯片: {chipName} | 存储: {memType} | Sahara V{saharaVer}";
            
            if (pkHash != null && pkHash.Length >= 4)
            {
                var (vendor, model, _) = GetDeviceByPkHash(pkHash);
                info += $" | 厂商: {vendor}";
            }
            
            return info;
        }

        /// <summary>
        /// 检查是否为 VIP 设备 (需要特殊认证)
        /// </summary>
        public static bool IsVipDevice(byte[] pkHash)
        {
            if (pkHash == null || pkHash.Length < 4)
                return false;

            var (vendor, _, _) = GetDeviceByPkHash(pkHash);
            
            // OPPO/OnePlus/Realme 设备通常需要 VIP 认证
            return vendor == "OPPO" || vendor == "OnePlus" || vendor == "Realme";
        }

        /// <summary>
        /// 检查是否为小米设备 (可能需要 MiAuth)
        /// </summary>
        public static bool IsXiaomiDevice(byte[] pkHash)
        {
            if (pkHash == null || pkHash.Length < 4)
                return false;

            var (vendor, _, _) = GetDeviceByPkHash(pkHash);
            return vendor == "Xiaomi" || vendor == "Redmi" || vendor == "POCO";
        }

        #endregion
    }
}
