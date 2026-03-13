// ============================================================================
// SakuraEDL - 多语言管理器
// Multi-Language Manager - 支持 6 种语言 (ZH/EN/JA/KO/RU/ES)
// ============================================================================

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Threading.Tasks;

namespace SakuraEDL.Common
{
    public class LanguageInfo
    {
        public string Code { get; set; }
        public string NativeName { get; set; }
        public string EnglishName { get; set; }
        public CultureInfo Culture { get; set; }
    }

    public static class LanguageManager
    {
        public static readonly List<LanguageInfo> SupportedLanguages = new List<LanguageInfo>
        {
            new LanguageInfo { Code = "zh", NativeName = "简体中文", EnglishName = "Chinese", Culture = new CultureInfo("zh-CN") },
            new LanguageInfo { Code = "en", NativeName = "English", EnglishName = "English", Culture = new CultureInfo("en-US") },
            new LanguageInfo { Code = "ja", NativeName = "日本語", EnglishName = "Japanese", Culture = new CultureInfo("ja-JP") },
            new LanguageInfo { Code = "ko", NativeName = "한국어", EnglishName = "Korean", Culture = new CultureInfo("ko-KR") },
            new LanguageInfo { Code = "ru", NativeName = "Русский", EnglishName = "Russian", Culture = new CultureInfo("ru-RU") },
            new LanguageInfo { Code = "es", NativeName = "Español", EnglishName = "Spanish", Culture = new CultureInfo("es-ES") }
        };

        private static readonly Dictionary<string, string> CountryToLanguage = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "CN", "zh" }, { "TW", "zh" }, { "HK", "zh" }, { "SG", "zh" },
            { "US", "en" }, { "GB", "en" }, { "AU", "en" }, { "CA", "en" }, { "NZ", "en" }, { "IE", "en" }, { "IN", "en" },
            { "JP", "ja" },
            { "KR", "ko" }, { "KP", "ko" },
            { "RU", "ru" }, { "BY", "ru" }, { "KZ", "ru" }, { "UA", "ru" },
            { "ES", "es" }, { "MX", "es" }, { "AR", "es" }, { "CO", "es" }, { "CL", "es" }, { "PE", "es" }, { "VE", "es" }
        };

        private static string _currentLanguage = "zh";
        private static Dictionary<string, Dictionary<string, string>> _translations;
        private static readonly object _lock = new object();
        private static string _settingsPath;

        public static string CurrentLanguage => _currentLanguage;
        public static LanguageInfo CurrentLanguageInfo => SupportedLanguages.Find(l => l.Code == _currentLanguage) ?? SupportedLanguages[0];
        public static event Action<string> LanguageChanged;

        public static string[] GetLanguageDisplayNames()
        {
            var names = new string[SupportedLanguages.Count];
            for (int i = 0; i < SupportedLanguages.Count; i++)
                names[i] = SupportedLanguages[i].NativeName;
            return names;
        }

        public static int GetCurrentLanguageIndex()
        {
            for (int i = 0; i < SupportedLanguages.Count; i++)
                if (SupportedLanguages[i].Code == _currentLanguage) return i;
            return 0;
        }

        public static string GetLanguageCodeByIndex(int index)
        {
            if (index >= 0 && index < SupportedLanguages.Count)
                return SupportedLanguages[index].Code;
            return "zh";
        }

        public static void SetLanguage(string langCode)
        {
            if (string.IsNullOrEmpty(langCode)) langCode = "zh";
            var lang = SupportedLanguages.Find(l => l.Code == langCode);
            if (lang == null) lang = SupportedLanguages[0];
            _currentLanguage = lang.Code;
            SaveLanguageSetting();
            LanguageChanged?.Invoke(_currentLanguage);
        }

        public static void Initialize()
        {
            try
            {
                _settingsPath = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "SakuraEDL", "language.txt");

                if (File.Exists(_settingsPath))
                {
                    var saved = File.ReadAllText(_settingsPath).Trim();
                    if (SupportedLanguages.Exists(l => l.Code == saved))
                    {
                        _currentLanguage = saved;
                        return;
                    }
                }

                var sysLang = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName.ToLower();
                if (SupportedLanguages.Exists(l => l.Code == sysLang))
                    _currentLanguage = sysLang;
            }
            catch { }
        }

        public static async Task DetectLanguageByIPAsync()
        {
            try
            {
                using (var client = new HttpClient())
                {
                    client.Timeout = TimeSpan.FromSeconds(3);
                    var response = await client.GetStringAsync("http://ip-api.com/json/?fields=countryCode").ConfigureAwait(false);
                    var match = System.Text.RegularExpressions.Regex.Match(response, "\"countryCode\"\\s*:\\s*\"([A-Z]+)\"");
                    if (match.Success)
                    {
                        var countryCode = match.Groups[1].Value;
                        if (CountryToLanguage.TryGetValue(countryCode, out var langCode))
                        {
                            if (_currentLanguage != langCode && SupportedLanguages.Exists(l => l.Code == langCode))
                            {
                                _currentLanguage = langCode;
                                SaveLanguageSetting();
                                LanguageChanged?.Invoke(_currentLanguage);
                            }
                        }
                    }
                }
            }
            catch { }
        }

        private static void SaveLanguageSetting()
        {
            try
            {
                var dir = Path.GetDirectoryName(_settingsPath);
                if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(_settingsPath, _currentLanguage);
            }
            catch { }
        }

        public static string T(string key)
        {
            EnsureTranslationsLoaded();
            if (_translations.TryGetValue(_currentLanguage, out var langDict))
                if (langDict.TryGetValue(key, out var value)) return value;
            if (_translations.TryGetValue("zh", out var zhDict))
                if (zhDict.TryGetValue(key, out var value)) return value;
            return key;
        }

        private static void EnsureTranslationsLoaded()
        {
            if (_translations != null) return;
            lock (_lock)
            {
                if (_translations != null) return;
                _translations = CreateTranslations();
            }
        }

        private static Dictionary<string, Dictionary<string, string>> CreateTranslations()
        {
            return new Dictionary<string, Dictionary<string, string>>
            {
                ["zh"] = new Dictionary<string, string>
                {
                    // 标签页
                    ["tab.autoRoot"] = "自动root",
                    ["tab.qualcomm"] = "高通",
                    ["tab.mtk"] = "联发科",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "设置",
                    
                    // 高通引导选择
                    ["qualcomm.autoDetectBoot"] = "云端自动匹配",
                    
                    // 菜单
                    ["menu.quickRestart"] = "快捷重启",
                    ["menu.edlOps"] = "EDL操作",
                    ["menu.other"] = "其他",
                    ["menu.rebootSystem"] = "重启系统",
                    ["menu.rebootBootloader"] = "重启到Bootloader",
                    ["menu.rebootFastbootd"] = "重启到Fastbootd",
                    ["menu.rebootRecovery"] = "重启到Recovery",
                    ["menu.miKickEdl"] = "小米踢EDL",
                    ["menu.lenovoKickEdl"] = "联想/安卓踢EDL",
                    ["menu.eraseFrp"] = "擦除谷歌锁",
                    ["menu.switchSlot"] = "切换槽位",
                    ["menu.mergeSuper"] = "合并Super",
                    ["menu.extractPayload"] = "提取Payload",
                    ["menu.edlToEdl"] = "EDL到EDL",
                    ["menu.edlToFbd"] = "EDL到FBD",
                    ["menu.edlEraseFrp"] = "EDL擦除谷歌锁",
                    ["menu.edlSwitchSlot"] = "EDL切换槽位",
                    ["menu.activateLun"] = "激活LUN",
                    ["menu.deviceManager"] = "设备管理器",
                    ["menu.cmdPrompt"] = "CMD命令行",
                    ["menu.androidDriver"] = "安卓驱动",
                    ["menu.mtkDriver"] = "MTK驱动",
                    ["menu.qualcommDriver"] = "高通驱动",
                    ["menu.viewLog"] = "查看日志",
                    
                    // 设置页
                    ["settings.blur"] = "背景模糊度",
                    ["settings.wallpaper"] = "壁纸",
                    ["settings.preview"] = "预览",
                    ["settings.previewStatus"] = "预览: {0}×{1} ({2}张图片)",
                    ["settings.language"] = "语言",
                    ["settings.localWallpaper"] = "本地壁纸",
                    ["settings.apply"] = "应用",
                    ["settings.realmeAccount"] = "realme 签名账号",
                    ["settings.realmeToken"] = "realme 接口 Token",
                    ["settings.saveRealmeAuth"] = "保存 realme 签名配置",
                    ["settings.testRealmeAuth"] = "测试 realme 签名接口",
                    ["settings.realmeAuthSaved"] = "realme 签名配置已保存",
                    ["settings.realmeAuthSaveFailed"] = "保存 realme 签名配置失败: {0}",
                    ["settings.realmeAuthMissingAccount"] = "请先填写 realme 签名账号",
                    ["settings.realmeAuthMissingToken"] = "请先填写 realme 接口 Token",
                    ["settings.realmeAuthTesting"] = "正在测试 realme 签名接口...",
                    ["settings.realmeAuthTestStatus"] = "realme 测试状态: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "realme 测试响应: {0}",
                    ["settings.realmeAuthTestSuccess"] = "realme 签名接口已返回响应",
                    ["settings.realmeAuthTestFailed"] = "realme 签名接口测试失败: {0}",
                    
                    // 高通页面
                    ["qualcomm.cloudAuto"] = "云端自动匹配",
                    ["qualcomm.autoDetect"] = "自动识别",
                    ["qualcomm.selectProgrammer"] = "双击选择引导文件",
                    ["qualcomm.selectRawXml"] = "选择Raw XML",
                    ["qualcomm.browse"] = "浏览",
                    ["qualcomm.partTable"] = "分区表",
                    ["qualcomm.partition"] = "分区",
                    ["qualcomm.lun"] = "LUN",
                    ["qualcomm.size"] = "大小",
                    ["qualcomm.startSector"] = "起始扇区",
                    ["qualcomm.endSector"] = "结束扇区",
                    ["qualcomm.sectorCount"] = "扇区数",
                    ["qualcomm.startAddr"] = "起始地址",
                    ["qualcomm.endAddr"] = "结束地址",
                    ["qualcomm.filePath"] = "文件路径",
                    ["qualcomm.readPartTable"] = "读取分区表",
                    ["qualcomm.readPart"] = "读取分区",
                    ["qualcomm.writePart"] = "写入分区",
                    ["qualcomm.erasePart"] = "擦除分区",
                    ["qualcomm.stop"] = "停止",
                    ["qualcomm.findPart"] = "查找分区",
                    
                    // 选项
                    ["option.skipBoot"] = "跳过引导",
                    ["option.protectPart"] = "保护分区",
                    ["option.generateXml"] = "生成XML",
                    ["option.autoReboot"] = "自动重启",
                    ["option.selectAll"] = "全选",
                    ["option.keepData"] = "保留数据",
                    
                    // 设备信息
                    ["device.status"] = "设备状态",
                    ["device.noDevice"] = "未连接任何设备",
                    ["device.info"] = "信息",
                    ["device.brand"] = "品牌",
                    ["device.chip"] = "芯片",
                    ["device.ota"] = "OTA",
                    ["device.serial"] = "序列号",
                    ["device.model"] = "型号",
                    ["device.storage"] = "存储",
                    ["device.waiting"] = "等待连接",
                    ["device.log"] = "日志",
                    
                    // 状态栏
                    ["status.ready"] = "当前操作：空闲",
                    ["status.operation"] = "当前操作",
                    ["status.idle"] = "空闲",
                    ["status.speed"] = "速度",
                    ["status.time"] = "时间",
                    ["status.computer"] = "计算机",
                    ["status.bit"] = "位",
                    ["status.contactDev"] = "联系开发者",
                    
                    // 日志
                    ["log.loaded"] = "加载完成",
                    ["log.langChanged"] = "界面语言已切换为：{0}",
                    ["log.qualcommInit"] = "高通模块初始化完成",
                    ["log.fastbootInit"] = "Fastboot 模块初始化完成",
                    ["log.mtkInit"] = "联发科模块已初始化",
                    ["log.selectLoader"] = "[提示] 请从下拉列表选择云端 Loader 或浏览本地引导文件",
                    
                    // MTK 页面
                    ["mtk.da"] = "DA文件",
                    ["mtk.scatter"] = "Scatter文件",
                    ["mtk.auth"] = "认证文件",
                    ["mtk.bootMode"] = "启动模式",
                    ["mtk.authMethod"] = "认证方式",
                    ["mtk.storageType"] = "存储类型",
                    ["mtk.readInfo"] = "读取信息",
                    ["mtk.formatAll"] = "格式化全盘",
                    ["mtk.flash"] = "刷写",
                    ["mtk.readBack"] = "回读",
                    
                    // SPD 页面
                    
                    // Fastboot 页面
                    ["fastboot.device"] = "设备",
                    ["fastboot.flash"] = "刷写",
                    ["fastboot.erase"] = "擦除",
                    ["fastboot.reboot"] = "重启",
                    ["fastboot.unlock"] = "解锁",
                    ["fastboot.lock"] = "上锁",
                    ["fastboot.execute"] = "执行",
                    ["fastboot.readInfo"] = "读取信息",
                    ["fastboot.fixFbd"] = "修复FBD",
                    ["fastboot.oplusFlash"] = "欧加刷写",
                    ["fastboot.lockBl"] = "锁定BL",
                    ["fastboot.clearData"] = "清除数据",
                    ["fastboot.switchSlotA"] = "切换A槽",
                    ["fastboot.extractImage"] = "提取镜像",
                    ["fastboot.fbdFlash"] = "FBD刷写",
                    ["fastboot.selectFlashBat"] = "选择flash Bat或输出路径",
                    ["fastboot.selectPayload"] = "选择Payload或输入URL",
                    ["fastboot.quickCommand"] = "执行快捷命令",
                    
                    // MTK 页面扩展
                    ["mtk.rebootDevice"] = "重启设备",
                    ["mtk.readImei"] = "读取IMEI",
                    ["mtk.writeImei"] = "写入IMEI",
                    ["mtk.backupNvram"] = "备份NVRAM",
                    ["mtk.restoreNvram"] = "恢复NVRAM",
                    ["mtk.formatData"] = "格式化Data",
                    ["mtk.unlockBl"] = "解锁BL",
                    ["mtk.exploit"] = "执行漏洞",
                    ["mtk.connect"] = "连接",
                    ["mtk.disconnect"] = "断开",
                    ["mtk.auto"] = "自动",
                    ["mtk.autoDetectBoot"] = "自动识别或自选引导",
                    ["mtk.cloudMatch"] = "云端自动匹配 (推荐)",
                    ["mtk.localSelect"] = "本地手动选择",
                    ["mtk.authMethod"] = "验证方式",
                    ["mtk.normalAuth"] = "正常验证 (签名DA)",
                    ["mtk.realmeCloud"] = "Realme云端签名",
                    ["mtk.bypassAuth"] = "绕过验证 (漏洞利用)",
                    
                    // SPD 页面扩展
                    
                    // 表格列
                    ["table.operation"] = "操作",
                    ["table.type"] = "类型",
                    ["table.address"] = "地址",
                    ["table.fileName"] = "文件名",
                    ["table.loadAddr"] = "加载地址",
                    ["table.offset"] = "偏移",
                    
                    // 设置页扩展
                    ["settings.clearCache"] = "清理缓存日志",
                    
                    // 开发中
                    ["dev.inProgress"] = "开发中...",
                    
                    // 遗漏的菜单
                    ["menu.edlFactoryReset"] = "EDL通用恢复出厂",
                    
                    // 标签页扩展
                    ["tab.partManage"] = "分区管理",
                    ["tab.fileManage"] = "文件管理",
                    
                    // 投屏功能
                    ["scrcpy.execute"] = "执行",
                    ["scrcpy.startMirror"] = "开启投屏",
                    ["scrcpy.fixMirror"] = "修复投屏异常",
                    ["scrcpy.flashZip"] = "刷入卡刷包",
                    ["scrcpy.screenOn"] = "屏幕常亮",
                    ["scrcpy.audioForward"] = "音频转发",
                    ["scrcpy.autoReconnect"] = "自动重连",
                    ["scrcpy.battery"] = "电池",
                    ["scrcpy.refreshRate"] = "刷新率",
                    ["scrcpy.resolution"] = "分辨率",
                    ["scrcpy.powerKey"] = "电源键",
                    ["scrcpy.recentKey"] = "后台键",
                    ["scrcpy.homeKey"] = "主页键",
                    ["scrcpy.backKey"] = "返回键",
                    
                    // 其他
                    ["status.loading"] = "「加载中...」",
                    ["app.version"] = "v3.0 · 永久免费"
                },
                
                ["en"] = new Dictionary<string, string>
                {
                    ["tab.autoRoot"] = "Auto Root",
                    ["tab.qualcomm"] = "Qualcomm",
                    ["tab.mtk"] = "MediaTek",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "Settings",
                    
                    ["qualcomm.autoDetectBoot"] = "Cloud Auto Match",
                    
                    ["menu.quickRestart"] = "Quick Restart",
                    ["menu.edlOps"] = "EDL Operations",
                    ["menu.other"] = "Other",
                    ["menu.rebootSystem"] = "Reboot System",
                    ["menu.rebootBootloader"] = "Reboot to Bootloader",
                    ["menu.rebootFastbootd"] = "Reboot to Fastbootd",
                    ["menu.rebootRecovery"] = "Reboot to Recovery",
                    ["menu.miKickEdl"] = "Mi Kick EDL",
                    ["menu.lenovoKickEdl"] = "Lenovo/Android Kick EDL",
                    ["menu.eraseFrp"] = "Erase FRP",
                    ["menu.switchSlot"] = "Switch Slot",
                    ["menu.mergeSuper"] = "Merge Super",
                    ["menu.extractPayload"] = "Extract Payload",
                    ["menu.edlToEdl"] = "EDL to EDL",
                    ["menu.edlToFbd"] = "EDL to FBD",
                    ["menu.edlEraseFrp"] = "EDL Erase FRP",
                    ["menu.edlSwitchSlot"] = "EDL Switch Slot",
                    ["menu.activateLun"] = "Activate LUN",
                    ["menu.deviceManager"] = "Device Manager",
                    ["menu.cmdPrompt"] = "CMD Prompt",
                    ["menu.androidDriver"] = "Android Driver",
                    ["menu.mtkDriver"] = "MTK Driver",
                    ["menu.qualcommDriver"] = "Qualcomm Driver",
                    ["menu.viewLog"] = "View Log",
                    
                    ["settings.blur"] = "Background Blur",
                    ["settings.wallpaper"] = "Wallpaper",
                    ["settings.preview"] = "Preview",
                    ["settings.previewStatus"] = "Preview: {0}×{1} ({2} images)",
                    ["settings.language"] = "Language",
                    ["settings.localWallpaper"] = "Local Wallpaper",
                    ["settings.apply"] = "Apply",
                    ["settings.realmeAccount"] = "realme Sign Account",
                    ["settings.realmeToken"] = "realme API Token",
                    ["settings.saveRealmeAuth"] = "Save realme Sign Config",
                    ["settings.testRealmeAuth"] = "Test realme Sign API",
                    ["settings.realmeAuthSaved"] = "realme sign settings saved",
                    ["settings.realmeAuthSaveFailed"] = "Failed to save realme sign settings: {0}",
                    ["settings.realmeAuthMissingAccount"] = "Please enter the realme sign account first",
                    ["settings.realmeAuthMissingToken"] = "Please enter the realme API token first",
                    ["settings.realmeAuthTesting"] = "Testing realme sign API...",
                    ["settings.realmeAuthTestStatus"] = "realme test status: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "realme test response: {0}",
                    ["settings.realmeAuthTestSuccess"] = "The realme sign API returned a response",
                    ["settings.realmeAuthTestFailed"] = "realme sign API test failed: {0}",
                    
                    ["qualcomm.cloudAuto"] = "Cloud Auto Match",
                    ["qualcomm.autoDetect"] = "Auto Detect",
                    ["qualcomm.selectProgrammer"] = "Double-click to select programmer",
                    ["qualcomm.selectRawXml"] = "Select Raw XML",
                    ["qualcomm.browse"] = "Browse",
                    ["qualcomm.partTable"] = "Partition Table",
                    ["qualcomm.partition"] = "Partition",
                    ["qualcomm.lun"] = "LUN",
                    ["qualcomm.size"] = "Size",
                    ["qualcomm.startSector"] = "Start Sector",
                    ["qualcomm.endSector"] = "End Sector",
                    ["qualcomm.sectorCount"] = "Sector Count",
                    ["qualcomm.startAddr"] = "Start Address",
                    ["qualcomm.endAddr"] = "End Address",
                    ["qualcomm.filePath"] = "File Path",
                    ["qualcomm.readPartTable"] = "Read Partition Table",
                    ["qualcomm.readPart"] = "Read Partition",
                    ["qualcomm.writePart"] = "Write Partition",
                    ["qualcomm.erasePart"] = "Erase Partition",
                    ["qualcomm.stop"] = "Stop",
                    ["qualcomm.findPart"] = "Find Partition",
                    
                    ["option.skipBoot"] = "Skip Boot",
                    ["option.protectPart"] = "Protect Partitions",
                    ["option.generateXml"] = "Generate XML",
                    ["option.autoReboot"] = "Auto Reboot",
                    ["option.selectAll"] = "Select All",
                    ["option.keepData"] = "Keep Data",
                    
                    ["device.status"] = "Device Status",
                    ["device.noDevice"] = "No device connected",
                    ["device.info"] = "Info",
                    ["device.brand"] = "Brand",
                    ["device.chip"] = "Chip",
                    ["device.ota"] = "OTA",
                    ["device.serial"] = "Serial",
                    ["device.model"] = "Model",
                    ["device.storage"] = "Storage",
                    ["device.waiting"] = "Waiting",
                    ["device.log"] = "Log",
                    
                    ["status.ready"] = "Operation: Idle",
                    ["status.operation"] = "Operation",
                    ["status.idle"] = "Idle",
                    ["status.speed"] = "Speed",
                    ["status.time"] = "Time",
                    ["status.computer"] = "Computer",
                    ["status.bit"] = "-bit",
                    ["status.contactDev"] = "Contact Developer",
                    
                    ["log.loaded"] = "Loaded",
                    ["log.langChanged"] = "Language changed to: {0}",
                    ["log.qualcommInit"] = "Qualcomm module initialized",
                    ["log.fastbootInit"] = "Fastboot module initialized",
                    ["log.mtkInit"] = "MediaTek module initialized",
                    ["log.selectLoader"] = "[Hint] Select cloud loader from dropdown or browse local programmer",
                    
                    ["mtk.da"] = "DA File",
                    ["mtk.scatter"] = "Scatter File",
                    ["mtk.auth"] = "Auth File",
                    ["mtk.bootMode"] = "Boot Mode",
                    ["mtk.authMethod"] = "Auth Method",
                    ["mtk.storageType"] = "Storage Type",
                    ["mtk.readInfo"] = "Read Info",
                    ["mtk.formatAll"] = "Format All",
                    ["mtk.flash"] = "Flash",
                    ["mtk.readBack"] = "Read Back",
                    
                    
                    ["fastboot.device"] = "Device",
                    ["fastboot.flash"] = "Flash",
                    ["fastboot.erase"] = "Erase",
                    ["fastboot.reboot"] = "Reboot",
                    ["fastboot.unlock"] = "Unlock",
                    ["fastboot.lock"] = "Lock",
                    ["fastboot.execute"] = "Execute",
                    ["fastboot.readInfo"] = "Read Info",
                    ["fastboot.fixFbd"] = "Fix FBD",
                    ["fastboot.oplusFlash"] = "Oplus Flash",
                    ["fastboot.lockBl"] = "Lock BL",
                    ["fastboot.clearData"] = "Clear Data",
                    ["fastboot.switchSlotA"] = "Switch to Slot A",
                    ["fastboot.extractImage"] = "Extract Image",
                    ["fastboot.fbdFlash"] = "FBD Flash",
                    ["fastboot.selectFlashBat"] = "Select flash Bat or output path",
                    ["fastboot.selectPayload"] = "Select Payload or enter URL",
                    ["fastboot.quickCommand"] = "Execute quick command",
                    
                    ["mtk.rebootDevice"] = "Reboot Device",
                    ["mtk.readImei"] = "Read IMEI",
                    ["mtk.writeImei"] = "Write IMEI",
                    ["mtk.backupNvram"] = "Backup NVRAM",
                    ["mtk.restoreNvram"] = "Restore NVRAM",
                    ["mtk.formatData"] = "Format Data",
                    ["mtk.unlockBl"] = "Unlock BL",
                    ["mtk.exploit"] = "Run Exploit",
                    ["mtk.connect"] = "Connect",
                    ["mtk.disconnect"] = "Disconnect",
                    ["mtk.auto"] = "Auto",
                    ["mtk.autoDetectBoot"] = "Auto detect or select boot",
                    ["mtk.cloudMatch"] = "Cloud Auto Match (Recommended)",
                    ["mtk.localSelect"] = "Local Manual Select",
                    ["mtk.authMethod"] = "Auth Method",
                    ["mtk.normalAuth"] = "Normal Auth (Signed DA)",
                    ["mtk.realmeCloud"] = "Realme Cloud Sign",
                    ["mtk.bypassAuth"] = "Bypass Auth (Exploit)",
                    
                    
                    ["table.operation"] = "Operation",
                    ["table.type"] = "Type",
                    ["table.address"] = "Address",
                    ["table.fileName"] = "File Name",
                    ["table.loadAddr"] = "Load Address",
                    ["table.offset"] = "Offset",
                    
                    ["settings.clearCache"] = "Clear Cache & Logs",
                    
                    ["dev.inProgress"] = "In Development...",
                    
                    ["menu.edlFactoryReset"] = "EDL Factory Reset",
                    
                    ["tab.partManage"] = "Partition Manager",
                    ["tab.fileManage"] = "File Manager",
                    
                    ["scrcpy.execute"] = "Execute",
                    ["scrcpy.startMirror"] = "Start Mirroring",
                    ["scrcpy.fixMirror"] = "Fix Mirroring",
                    ["scrcpy.flashZip"] = "Flash ZIP",
                    ["scrcpy.screenOn"] = "Keep Screen On",
                    ["scrcpy.audioForward"] = "Audio Forward",
                    ["scrcpy.autoReconnect"] = "Auto Reconnect",
                    ["scrcpy.battery"] = "Battery",
                    ["scrcpy.refreshRate"] = "Refresh Rate",
                    ["scrcpy.resolution"] = "Resolution",
                    ["scrcpy.powerKey"] = "Power",
                    ["scrcpy.recentKey"] = "Recent",
                    ["scrcpy.homeKey"] = "Home",
                    ["scrcpy.backKey"] = "Back",
                    
                    ["status.loading"] = "Loading...",
                    ["app.version"] = "v3.0 · Free Forever"
                },
                
                ["ja"] = new Dictionary<string, string>
                {
                    ["tab.autoRoot"] = "自動Root",
                    ["tab.qualcomm"] = "Qualcomm",
                    ["tab.mtk"] = "MediaTek",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "設定",
                    
                    ["qualcomm.autoDetectBoot"] = "クラウド自動マッチ",
                    
                    ["menu.quickRestart"] = "クイック再起動",
                    ["menu.edlOps"] = "EDL操作",
                    ["menu.other"] = "その他",
                    ["menu.rebootSystem"] = "システム再起動",
                    ["menu.rebootBootloader"] = "Bootloaderへ再起動",
                    ["menu.rebootFastbootd"] = "Fastbootdへ再起動",
                    ["menu.rebootRecovery"] = "Recoveryへ再起動",
                    ["menu.miKickEdl"] = "Mi EDLキック",
                    ["menu.lenovoKickEdl"] = "Lenovo/Android EDLキック",
                    ["menu.eraseFrp"] = "FRP消去",
                    ["menu.switchSlot"] = "スロット切替",
                    ["menu.mergeSuper"] = "Super統合",
                    ["menu.extractPayload"] = "Payload抽出",
                    ["menu.deviceManager"] = "デバイスマネージャー",
                    ["menu.cmdPrompt"] = "コマンドプロンプト",
                    ["menu.viewLog"] = "ログを見る",
                    
                    ["settings.blur"] = "背景ぼかし",
                    ["settings.wallpaper"] = "壁紙",
                    ["settings.preview"] = "プレビュー",
                    ["settings.previewStatus"] = "プレビュー: {0}×{1} ({2}枚)",
                    ["settings.language"] = "言語",
                    ["settings.localWallpaper"] = "ローカル壁紙",
                    ["settings.apply"] = "適用",
                    ["settings.realmeAccount"] = "realme 署名アカウント",
                    ["settings.realmeToken"] = "realme API トークン",
                    ["settings.saveRealmeAuth"] = "realme 署名設定を保存",
                    ["settings.testRealmeAuth"] = "realme 署名APIをテスト",
                    ["settings.realmeAuthSaved"] = "realme 署名設定を保存しました",
                    ["settings.realmeAuthSaveFailed"] = "realme 署名設定の保存に失敗しました: {0}",
                    ["settings.realmeAuthMissingAccount"] = "まず realme 署名アカウントを入力してください",
                    ["settings.realmeAuthMissingToken"] = "まず realme API トークンを入力してください",
                    ["settings.realmeAuthTesting"] = "realme 署名APIをテスト中...",
                    ["settings.realmeAuthTestStatus"] = "realme テスト状態: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "realme テスト応答: {0}",
                    ["settings.realmeAuthTestSuccess"] = "realme 署名APIから応答が返されました",
                    ["settings.realmeAuthTestFailed"] = "realme 署名APIのテストに失敗しました: {0}",
                    
                    ["qualcomm.cloudAuto"] = "クラウド自動マッチ",
                    ["qualcomm.autoDetect"] = "自動検出",
                    ["qualcomm.selectProgrammer"] = "ダブルクリックでプログラマー選択",
                    ["qualcomm.selectRawXml"] = "Raw XML選択",
                    ["qualcomm.browse"] = "参照",
                    ["qualcomm.partTable"] = "パーティションテーブル",
                    ["qualcomm.partition"] = "パーティション",
                    ["qualcomm.readPartTable"] = "パーティションテーブル読取",
                    ["qualcomm.readPart"] = "パーティション読取",
                    ["qualcomm.writePart"] = "パーティション書込",
                    ["qualcomm.erasePart"] = "パーティション消去",
                    ["qualcomm.stop"] = "停止",
                    ["qualcomm.findPart"] = "パーティション検索",
                    
                    ["option.skipBoot"] = "ブートスキップ",
                    ["option.protectPart"] = "パーティション保護",
                    ["option.generateXml"] = "XML生成",
                    ["option.autoReboot"] = "自動再起動",
                    ["option.selectAll"] = "全選択",
                    ["option.keepData"] = "データ保持",
                    
                    ["device.status"] = "デバイス状態",
                    ["device.noDevice"] = "デバイス未接続",
                    ["device.info"] = "情報",
                    ["device.brand"] = "ブランド",
                    ["device.chip"] = "チップ",
                    ["device.waiting"] = "接続待ち",
                    ["device.log"] = "ログ",
                    
                    ["status.ready"] = "操作：待機中",
                    ["status.operation"] = "操作",
                    ["status.idle"] = "待機中",
                    ["status.speed"] = "速度",
                    ["status.time"] = "時間",
                    ["status.computer"] = "コンピュータ",
                    ["status.bit"] = "ビット",
                    ["status.contactDev"] = "開発者連絡",
                    
                    ["log.loaded"] = "読み込み完了",
                    ["log.langChanged"] = "言語を変更しました：{0}",
                    ["log.qualcommInit"] = "Qualcommモジュール初期化完了",
                    ["log.fastbootInit"] = "Fastbootモジュール初期化完了",
                    ["log.mtkInit"] = "MediaTekモジュール初期化完了",
                    ["log.selectLoader"] = "[ヒント] ドロップダウンからクラウドローダーを選択するか、ローカルファイルを参照",
                    
                    ["fastboot.execute"] = "実行",
                    ["fastboot.readInfo"] = "情報読取",
                    ["fastboot.fixFbd"] = "FBD修復",
                    ["fastboot.oplusFlash"] = "Oplus書込",
                    ["fastboot.lockBl"] = "BLロック",
                    ["fastboot.clearData"] = "データ消去",
                    ["fastboot.switchSlotA"] = "スロットA切替",
                    ["fastboot.extractImage"] = "イメージ抽出",
                    ["fastboot.fbdFlash"] = "FBD書込",
                    ["fastboot.selectFlashBat"] = "flash Batまたは出力パス選択",
                    ["fastboot.selectPayload"] = "PayloadまたはURL入力",
                    ["fastboot.quickCommand"] = "クイックコマンド実行",
                    
                    ["mtk.rebootDevice"] = "デバイス再起動",
                    ["mtk.readImei"] = "IMEI読取",
                    ["mtk.writeImei"] = "IMEI書込",
                    ["mtk.backupNvram"] = "NVRAMバックアップ",
                    ["mtk.restoreNvram"] = "NVRAM復元",
                    ["mtk.formatData"] = "Dataフォーマット",
                    ["mtk.unlockBl"] = "BLアンロック",
                    ["mtk.exploit"] = "エクスプロイト実行",
                    ["mtk.connect"] = "接続",
                    ["mtk.disconnect"] = "切断",
                    ["mtk.auto"] = "自動",
                    ["mtk.autoDetectBoot"] = "自動検出またはブート選択",
                    ["mtk.cloudMatch"] = "クラウド自動マッチ (推奨)",
                    ["mtk.localSelect"] = "ローカル手動選択",
                    ["mtk.authMethod"] = "認証方式",
                    ["mtk.normalAuth"] = "通常認証 (署名DA)",
                    ["mtk.realmeCloud"] = "Realmeクラウド署名",
                    ["mtk.bypassAuth"] = "認証バイパス (エクスプロイト)",
                    
                    
                    ["table.operation"] = "操作",
                    ["table.type"] = "タイプ",
                    ["table.address"] = "アドレス",
                    ["table.fileName"] = "ファイル名",
                    ["table.loadAddr"] = "ロードアドレス",
                    ["table.offset"] = "オフセット",
                    
                    ["settings.clearCache"] = "キャッシュとログを消去",
                    
                    ["dev.inProgress"] = "開発中...",
                    
                    ["menu.edlFactoryReset"] = "EDL工場出荷時リセット",
                    
                    ["tab.partManage"] = "パーティション管理",
                    ["tab.fileManage"] = "ファイル管理",
                    
                    ["scrcpy.execute"] = "実行",
                    ["scrcpy.startMirror"] = "ミラーリング開始",
                    ["scrcpy.fixMirror"] = "ミラーリング修復",
                    ["scrcpy.flashZip"] = "ZIP書込",
                    ["scrcpy.screenOn"] = "画面常時オン",
                    ["scrcpy.audioForward"] = "オーディオ転送",
                    ["scrcpy.autoReconnect"] = "自動再接続",
                    ["scrcpy.battery"] = "バッテリー",
                    ["scrcpy.refreshRate"] = "リフレッシュレート",
                    ["scrcpy.resolution"] = "解像度",
                    ["scrcpy.powerKey"] = "電源",
                    ["scrcpy.recentKey"] = "履歴",
                    ["scrcpy.homeKey"] = "ホーム",
                    ["scrcpy.backKey"] = "戻る",
                    
                    ["status.loading"] = "読み込み中...",
                    ["app.version"] = "v3.0 · 永久無料"
                },
                
                ["ko"] = new Dictionary<string, string>
                {
                    ["tab.autoRoot"] = "자동 Root",
                    ["tab.qualcomm"] = "퀄컴",
                    ["tab.mtk"] = "미디어텍",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "설정",
                    
                    ["qualcomm.autoDetectBoot"] = "클라우드 자동 매칭",
                    
                    ["menu.quickRestart"] = "빠른 재시작",
                    ["menu.edlOps"] = "EDL 작업",
                    ["menu.other"] = "기타",
                    ["menu.rebootSystem"] = "시스템 재시작",
                    ["menu.deviceManager"] = "장치 관리자",
                    ["menu.viewLog"] = "로그 보기",
                    
                    ["settings.blur"] = "배경 흐림",
                    ["settings.wallpaper"] = "배경화면",
                    ["settings.preview"] = "미리보기",
                    ["settings.previewStatus"] = "미리보기: {0}×{1} ({2}장)",
                    ["settings.language"] = "언어",
                    ["settings.localWallpaper"] = "로컬 배경화면",
                    ["settings.apply"] = "적용",
                    ["settings.realmeAccount"] = "realme 서명 계정",
                    ["settings.realmeToken"] = "realme API 토큰",
                    ["settings.saveRealmeAuth"] = "realme 서명 설정 저장",
                    ["settings.testRealmeAuth"] = "realme 서명 API 테스트",
                    ["settings.realmeAuthSaved"] = "realme 서명 설정이 저장되었습니다",
                    ["settings.realmeAuthSaveFailed"] = "realme 서명 설정 저장 실패: {0}",
                    ["settings.realmeAuthMissingAccount"] = "먼저 realme 서명 계정을 입력하세요",
                    ["settings.realmeAuthMissingToken"] = "먼저 realme API 토큰을 입력하세요",
                    ["settings.realmeAuthTesting"] = "realme 서명 API 테스트 중...",
                    ["settings.realmeAuthTestStatus"] = "realme 테스트 상태: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "realme 테스트 응답: {0}",
                    ["settings.realmeAuthTestSuccess"] = "realme 서명 API가 응답을 반환했습니다",
                    ["settings.realmeAuthTestFailed"] = "realme 서명 API 테스트 실패: {0}",
                    
                    ["qualcomm.cloudAuto"] = "클라우드 자동 매칭",
                    ["qualcomm.autoDetect"] = "자동 감지",
                    ["qualcomm.selectProgrammer"] = "더블클릭하여 프로그래머 선택",
                    ["qualcomm.selectRawXml"] = "Raw XML 선택",
                    ["qualcomm.browse"] = "찾아보기",
                    ["qualcomm.partTable"] = "파티션 테이블",
                    ["qualcomm.partition"] = "파티션",
                    ["qualcomm.readPartTable"] = "파티션 테이블 읽기",
                    ["qualcomm.readPart"] = "파티션 읽기",
                    ["qualcomm.writePart"] = "파티션 쓰기",
                    ["qualcomm.erasePart"] = "파티션 지우기",
                    ["qualcomm.stop"] = "중지",
                    ["qualcomm.findPart"] = "파티션 찾기",
                    
                    ["option.skipBoot"] = "부팅 건너뛰기",
                    ["option.protectPart"] = "파티션 보호",
                    ["option.generateXml"] = "XML 생성",
                    ["option.autoReboot"] = "자동 재부팅",
                    ["option.selectAll"] = "전체 선택",
                    ["option.keepData"] = "데이터 유지",
                    
                    ["device.status"] = "장치 상태",
                    ["device.noDevice"] = "연결된 장치 없음",
                    ["device.info"] = "정보",
                    ["device.brand"] = "브랜드",
                    ["device.chip"] = "칩",
                    ["device.waiting"] = "대기 중",
                    ["device.log"] = "로그",
                    
                    ["status.ready"] = "작업: 대기 중",
                    ["status.operation"] = "작업",
                    ["status.idle"] = "대기 중",
                    ["status.speed"] = "속도",
                    ["status.time"] = "시간",
                    ["status.computer"] = "컴퓨터",
                    ["status.bit"] = "비트",
                    ["status.contactDev"] = "개발자 연락",
                    
                    ["log.loaded"] = "로드 완료",
                    ["log.langChanged"] = "언어가 변경되었습니다: {0}",
                    ["log.qualcommInit"] = "퀄컴 모듈 초기화 완료",
                    ["log.fastbootInit"] = "Fastboot 모듈 초기화 완료",
                    ["log.mtkInit"] = "미디어텍 모듈 초기화 완료",
                    ["log.selectLoader"] = "[힌트] 드롭다운에서 클라우드 로더를 선택하거나 로컬 파일 찾아보기",
                    
                    ["fastboot.execute"] = "실행",
                    ["fastboot.readInfo"] = "정보 읽기",
                    ["fastboot.fixFbd"] = "FBD 수정",
                    ["fastboot.oplusFlash"] = "Oplus 플래시",
                    ["fastboot.lockBl"] = "BL 잠금",
                    ["fastboot.clearData"] = "데이터 삭제",
                    ["fastboot.switchSlotA"] = "슬롯 A 전환",
                    ["fastboot.extractImage"] = "이미지 추출",
                    ["fastboot.fbdFlash"] = "FBD 플래시",
                    ["fastboot.selectFlashBat"] = "flash Bat 또는 출력 경로 선택",
                    ["fastboot.selectPayload"] = "Payload 또는 URL 입력",
                    ["fastboot.quickCommand"] = "빠른 명령 실행",
                    
                    ["mtk.rebootDevice"] = "장치 재시작",
                    ["mtk.readImei"] = "IMEI 읽기",
                    ["mtk.writeImei"] = "IMEI 쓰기",
                    ["mtk.backupNvram"] = "NVRAM 백업",
                    ["mtk.restoreNvram"] = "NVRAM 복원",
                    ["mtk.formatData"] = "Data 포맷",
                    ["mtk.unlockBl"] = "BL 잠금해제",
                    ["mtk.exploit"] = "익스플로잇 실행",
                    ["mtk.connect"] = "연결",
                    ["mtk.disconnect"] = "연결 해제",
                    ["mtk.auto"] = "자동",
                    ["mtk.autoDetectBoot"] = "자동 감지 또는 부트 선택",
                    ["mtk.cloudMatch"] = "클라우드 자동 매칭 (권장)",
                    ["mtk.localSelect"] = "로컬 수동 선택",
                    ["mtk.authMethod"] = "인증 방식",
                    ["mtk.normalAuth"] = "일반 인증 (서명된 DA)",
                    ["mtk.realmeCloud"] = "Realme 클라우드 서명",
                    ["mtk.bypassAuth"] = "인증 우회 (익스플로잇)",
                    
                    
                    ["table.operation"] = "작업",
                    ["table.type"] = "유형",
                    ["table.address"] = "주소",
                    ["table.fileName"] = "파일명",
                    ["table.loadAddr"] = "로드 주소",
                    ["table.offset"] = "오프셋",
                    
                    ["settings.clearCache"] = "캐시 및 로그 삭제",
                    
                    ["dev.inProgress"] = "개발 중...",
                    
                    ["menu.edlFactoryReset"] = "EDL 공장 초기화",
                    
                    ["tab.partManage"] = "파티션 관리",
                    ["tab.fileManage"] = "파일 관리",
                    
                    ["scrcpy.execute"] = "실행",
                    ["scrcpy.startMirror"] = "미러링 시작",
                    ["scrcpy.fixMirror"] = "미러링 수정",
                    ["scrcpy.flashZip"] = "ZIP 플래시",
                    ["scrcpy.screenOn"] = "화면 켜짐 유지",
                    ["scrcpy.audioForward"] = "오디오 전달",
                    ["scrcpy.autoReconnect"] = "자동 재연결",
                    ["scrcpy.battery"] = "배터리",
                    ["scrcpy.refreshRate"] = "주사율",
                    ["scrcpy.resolution"] = "해상도",
                    ["scrcpy.powerKey"] = "전원",
                    ["scrcpy.recentKey"] = "최근",
                    ["scrcpy.homeKey"] = "홈",
                    ["scrcpy.backKey"] = "뒤로",
                    
                    ["status.loading"] = "로딩 중...",
                    ["app.version"] = "v3.0 · 영구 무료"
                },
                
                ["ru"] = new Dictionary<string, string>
                {
                    ["tab.autoRoot"] = "Авто Root",
                    ["tab.qualcomm"] = "Qualcomm",
                    ["tab.mtk"] = "MediaTek",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "Настройки",
                    
                    ["qualcomm.autoDetectBoot"] = "Облачный автоподбор",
                    
                    ["menu.quickRestart"] = "Быстрый перезапуск",
                    ["menu.edlOps"] = "Операции EDL",
                    ["menu.other"] = "Другое",
                    ["menu.rebootSystem"] = "Перезагрузка системы",
                    ["menu.deviceManager"] = "Диспетчер устройств",
                    ["menu.viewLog"] = "Просмотр журнала",
                    
                    ["settings.blur"] = "Размытие фона",
                    ["settings.wallpaper"] = "Обои",
                    ["settings.preview"] = "Предпросмотр",
                    ["settings.previewStatus"] = "Предпросмотр: {0}×{1} ({2} изображ.)",
                    ["settings.language"] = "Язык",
                    ["settings.localWallpaper"] = "Локальные обои",
                    ["settings.apply"] = "Применить",
                    ["settings.realmeAccount"] = "Аккаунт подписи realme",
                    ["settings.realmeToken"] = "Токен API realme",
                    ["settings.saveRealmeAuth"] = "Сохранить подпись realme",
                    ["settings.testRealmeAuth"] = "Проверить API подписи realme",
                    ["settings.realmeAuthSaved"] = "Настройки подписи realme сохранены",
                    ["settings.realmeAuthSaveFailed"] = "Не удалось сохранить настройки подписи realme: {0}",
                    ["settings.realmeAuthMissingAccount"] = "Сначала введите аккаунт подписи realme",
                    ["settings.realmeAuthMissingToken"] = "Сначала введите токен API realme",
                    ["settings.realmeAuthTesting"] = "Проверка API подписи realme...",
                    ["settings.realmeAuthTestStatus"] = "Статус теста realme: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "Ответ теста realme: {0}",
                    ["settings.realmeAuthTestSuccess"] = "API подписи realme вернул ответ",
                    ["settings.realmeAuthTestFailed"] = "Ошибка теста API подписи realme: {0}",
                    
                    ["qualcomm.cloudAuto"] = "Облачное автосопоставление",
                    ["qualcomm.autoDetect"] = "Автоопределение",
                    ["qualcomm.selectProgrammer"] = "Дважды щелкните для выбора программатора",
                    ["qualcomm.selectRawXml"] = "Выбрать Raw XML",
                    ["qualcomm.browse"] = "Обзор",
                    ["qualcomm.partTable"] = "Таблица разделов",
                    ["qualcomm.partition"] = "Раздел",
                    ["qualcomm.readPartTable"] = "Чтение таблицы разделов",
                    ["qualcomm.readPart"] = "Чтение раздела",
                    ["qualcomm.writePart"] = "Запись раздела",
                    ["qualcomm.erasePart"] = "Стирание раздела",
                    ["qualcomm.stop"] = "Стоп",
                    ["qualcomm.findPart"] = "Найти раздел",
                    
                    ["option.skipBoot"] = "Пропустить загрузку",
                    ["option.protectPart"] = "Защита разделов",
                    ["option.generateXml"] = "Создать XML",
                    ["option.autoReboot"] = "Авто перезагрузка",
                    ["option.selectAll"] = "Выбрать все",
                    ["option.keepData"] = "Сохранить данные",
                    
                    ["device.status"] = "Статус устройства",
                    ["device.noDevice"] = "Устройство не подключено",
                    ["device.info"] = "Информация",
                    ["device.brand"] = "Бренд",
                    ["device.chip"] = "Чип",
                    ["device.waiting"] = "Ожидание",
                    ["device.log"] = "Журнал",
                    
                    ["status.ready"] = "Операция: Готов",
                    ["status.operation"] = "Операция",
                    ["status.idle"] = "Готов",
                    ["status.speed"] = "Скорость",
                    ["status.time"] = "Время",
                    ["status.computer"] = "Компьютер",
                    ["status.bit"] = "-бит",
                    ["status.contactDev"] = "Связаться с разработчиком",
                    
                    ["log.loaded"] = "Загружено",
                    ["log.langChanged"] = "Язык изменен на: {0}",
                    ["log.qualcommInit"] = "Модуль Qualcomm инициализирован",
                    ["log.fastbootInit"] = "Модуль Fastboot инициализирован",
                    ["log.mtkInit"] = "Модуль MediaTek инициализирован",
                    ["log.selectLoader"] = "[Подсказка] Выберите облачный загрузчик из списка или укажите локальный файл",
                    
                    ["fastboot.execute"] = "Выполнить",
                    ["fastboot.readInfo"] = "Чтение инфо",
                    ["fastboot.fixFbd"] = "Исправить FBD",
                    ["fastboot.oplusFlash"] = "Oplus прошивка",
                    ["fastboot.lockBl"] = "Заблокировать BL",
                    ["fastboot.clearData"] = "Очистить данные",
                    ["fastboot.switchSlotA"] = "Переключить на слот A",
                    ["fastboot.extractImage"] = "Извлечь образ",
                    ["fastboot.fbdFlash"] = "FBD прошивка",
                    ["fastboot.selectFlashBat"] = "Выбрать flash Bat или путь",
                    ["fastboot.selectPayload"] = "Выбрать Payload или ввести URL",
                    ["fastboot.quickCommand"] = "Выполнить быструю команду",
                    
                    ["mtk.rebootDevice"] = "Перезагрузить устройство",
                    ["mtk.readImei"] = "Чтение IMEI",
                    ["mtk.writeImei"] = "Запись IMEI",
                    ["mtk.backupNvram"] = "Резервная копия NVRAM",
                    ["mtk.restoreNvram"] = "Восстановление NVRAM",
                    ["mtk.formatData"] = "Форматировать Data",
                    ["mtk.unlockBl"] = "Разблокировать BL",
                    ["mtk.exploit"] = "Запустить эксплойт",
                    ["mtk.connect"] = "Подключить",
                    ["mtk.disconnect"] = "Отключить",
                    ["mtk.auto"] = "Авто",
                    ["mtk.autoDetectBoot"] = "Автоопределение или выбор загрузки",
                    ["mtk.cloudMatch"] = "Облачный автоподбор (Рекомендуется)",
                    ["mtk.localSelect"] = "Локальный ручной выбор",
                    ["mtk.authMethod"] = "Метод авторизации",
                    ["mtk.normalAuth"] = "Обычная авторизация (Подписанный DA)",
                    ["mtk.realmeCloud"] = "Realme облачная подпись",
                    ["mtk.bypassAuth"] = "Обход авторизации (Эксплойт)",
                    
                    
                    ["table.operation"] = "Операция",
                    ["table.type"] = "Тип",
                    ["table.address"] = "Адрес",
                    ["table.fileName"] = "Имя файла",
                    ["table.loadAddr"] = "Адрес загрузки",
                    ["table.offset"] = "Смещение",
                    
                    ["settings.clearCache"] = "Очистить кэш и логи",
                    
                    ["dev.inProgress"] = "В разработке...",
                    
                    ["menu.edlFactoryReset"] = "EDL сброс до заводских",
                    
                    ["tab.partManage"] = "Управление разделами",
                    ["tab.fileManage"] = "Файловый менеджер",
                    
                    ["scrcpy.execute"] = "Выполнить",
                    ["scrcpy.startMirror"] = "Начать трансляцию",
                    ["scrcpy.fixMirror"] = "Исправить трансляцию",
                    ["scrcpy.flashZip"] = "Прошить ZIP",
                    ["scrcpy.screenOn"] = "Экран всегда включен",
                    ["scrcpy.audioForward"] = "Переадресация аудио",
                    ["scrcpy.autoReconnect"] = "Автопереподключение",
                    ["scrcpy.battery"] = "Батарея",
                    ["scrcpy.refreshRate"] = "Частота обновления",
                    ["scrcpy.resolution"] = "Разрешение",
                    ["scrcpy.powerKey"] = "Питание",
                    ["scrcpy.recentKey"] = "Недавние",
                    ["scrcpy.homeKey"] = "Домой",
                    ["scrcpy.backKey"] = "Назад",
                    
                    ["status.loading"] = "Загрузка...",
                    ["app.version"] = "v3.0 · Бесплатно навсегда"
                },
                
                ["es"] = new Dictionary<string, string>
                {
                    ["tab.autoRoot"] = "Auto Root",
                    ["tab.qualcomm"] = "Qualcomm",
                    ["tab.mtk"] = "MediaTek",
                    ["tab.fastboot"] = "Fastboot",
                    ["tab.settings"] = "Ajustes",
                    
                    ["qualcomm.autoDetectBoot"] = "Coincidencia automática en la nube",
                    
                    ["menu.quickRestart"] = "Reinicio rápido",
                    ["menu.edlOps"] = "Operaciones EDL",
                    ["menu.other"] = "Otros",
                    ["menu.rebootSystem"] = "Reiniciar sistema",
                    ["menu.deviceManager"] = "Administrador de dispositivos",
                    ["menu.viewLog"] = "Ver registro",
                    
                    ["settings.blur"] = "Desenfoque de fondo",
                    ["settings.wallpaper"] = "Fondo de pantalla",
                    ["settings.preview"] = "Vista previa",
                    ["settings.previewStatus"] = "Vista previa: {0}×{1} ({2} imágenes)",
                    ["settings.language"] = "Idioma",
                    ["settings.localWallpaper"] = "Fondo local",
                    ["settings.apply"] = "Aplicar",
                    ["settings.realmeAccount"] = "Cuenta de firma realme",
                    ["settings.realmeToken"] = "Token API de realme",
                    ["settings.saveRealmeAuth"] = "Guardar firma realme",
                    ["settings.testRealmeAuth"] = "Probar API de firma realme",
                    ["settings.realmeAuthSaved"] = "Configuración de firma realme guardada",
                    ["settings.realmeAuthSaveFailed"] = "No se pudo guardar la firma realme: {0}",
                    ["settings.realmeAuthMissingAccount"] = "Primero introduce la cuenta de firma realme",
                    ["settings.realmeAuthMissingToken"] = "Primero introduce el token API de realme",
                    ["settings.realmeAuthTesting"] = "Probando API de firma realme...",
                    ["settings.realmeAuthTestStatus"] = "Estado de prueba realme: HTTP {0} {1}",
                    ["settings.realmeAuthTestResponse"] = "Respuesta de prueba realme: {0}",
                    ["settings.realmeAuthTestSuccess"] = "La API de firma realme devolvió una respuesta",
                    ["settings.realmeAuthTestFailed"] = "Error al probar la API de firma realme: {0}",
                    
                    ["qualcomm.cloudAuto"] = "Coincidencia automática en la nube",
                    ["qualcomm.autoDetect"] = "Detección automática",
                    ["qualcomm.selectProgrammer"] = "Doble clic para seleccionar programador",
                    ["qualcomm.selectRawXml"] = "Seleccionar Raw XML",
                    ["qualcomm.browse"] = "Examinar",
                    ["qualcomm.partTable"] = "Tabla de particiones",
                    ["qualcomm.partition"] = "Partición",
                    ["qualcomm.readPartTable"] = "Leer tabla de particiones",
                    ["qualcomm.readPart"] = "Leer partición",
                    ["qualcomm.writePart"] = "Escribir partición",
                    ["qualcomm.erasePart"] = "Borrar partición",
                    ["qualcomm.stop"] = "Detener",
                    ["qualcomm.findPart"] = "Buscar partición",
                    
                    ["option.skipBoot"] = "Omitir arranque",
                    ["option.protectPart"] = "Proteger particiones",
                    ["option.generateXml"] = "Generar XML",
                    ["option.autoReboot"] = "Reinicio automático",
                    ["option.selectAll"] = "Seleccionar todo",
                    ["option.keepData"] = "Mantener datos",
                    
                    ["device.status"] = "Estado del dispositivo",
                    ["device.noDevice"] = "Ningún dispositivo conectado",
                    ["device.info"] = "Información",
                    ["device.brand"] = "Marca",
                    ["device.chip"] = "Chip",
                    ["device.waiting"] = "Esperando",
                    ["device.log"] = "Registro",
                    
                    ["status.ready"] = "Operación: Listo",
                    ["status.operation"] = "Operación",
                    ["status.idle"] = "Listo",
                    ["status.speed"] = "Velocidad",
                    ["status.time"] = "Tiempo",
                    ["status.computer"] = "Ordenador",
                    ["status.bit"] = " bits",
                    ["status.contactDev"] = "Contactar desarrollador",
                    
                    ["log.loaded"] = "Cargado",
                    ["log.langChanged"] = "Idioma cambiado a: {0}",
                    ["log.qualcommInit"] = "Módulo Qualcomm inicializado",
                    ["log.fastbootInit"] = "Módulo Fastboot inicializado",
                    ["log.mtkInit"] = "Módulo MediaTek inicializado",
                    ["log.selectLoader"] = "[Sugerencia] Seleccione el cargador en la nube del menú o examine el archivo local",
                    
                    ["fastboot.execute"] = "Ejecutar",
                    ["fastboot.readInfo"] = "Leer Info",
                    ["fastboot.fixFbd"] = "Reparar FBD",
                    ["fastboot.oplusFlash"] = "Flash Oplus",
                    ["fastboot.lockBl"] = "Bloquear BL",
                    ["fastboot.clearData"] = "Borrar Datos",
                    ["fastboot.switchSlotA"] = "Cambiar a Slot A",
                    ["fastboot.extractImage"] = "Extraer Imagen",
                    ["fastboot.fbdFlash"] = "Flash FBD",
                    ["fastboot.selectFlashBat"] = "Seleccionar flash Bat o ruta",
                    ["fastboot.selectPayload"] = "Seleccionar Payload o introducir URL",
                    ["fastboot.quickCommand"] = "Ejecutar comando rápido",
                    
                    ["mtk.rebootDevice"] = "Reiniciar Dispositivo",
                    ["mtk.readImei"] = "Leer IMEI",
                    ["mtk.writeImei"] = "Escribir IMEI",
                    ["mtk.backupNvram"] = "Copia de NVRAM",
                    ["mtk.restoreNvram"] = "Restaurar NVRAM",
                    ["mtk.formatData"] = "Formatear Data",
                    ["mtk.unlockBl"] = "Desbloquear BL",
                    ["mtk.exploit"] = "Ejecutar Exploit",
                    ["mtk.connect"] = "Conectar",
                    ["mtk.disconnect"] = "Desconectar",
                    ["mtk.auto"] = "Auto",
                    ["mtk.autoDetectBoot"] = "Autodetectar o seleccionar arranque",
                    ["mtk.cloudMatch"] = "Coincidencia automática en la nube (Recomendado)",
                    ["mtk.localSelect"] = "Selección manual local",
                    ["mtk.authMethod"] = "Método de autenticación",
                    ["mtk.normalAuth"] = "Auth normal (DA firmado)",
                    ["mtk.realmeCloud"] = "Firma en la nube Realme",
                    ["mtk.bypassAuth"] = "Bypass de auth (Exploit)",
                    
                    
                    ["table.operation"] = "Operación",
                    ["table.type"] = "Tipo",
                    ["table.address"] = "Dirección",
                    ["table.fileName"] = "Nombre de archivo",
                    ["table.loadAddr"] = "Dirección de carga",
                    ["table.offset"] = "Desplazamiento",
                    
                    ["settings.clearCache"] = "Limpiar caché y registros",
                    
                    ["dev.inProgress"] = "En desarrollo...",
                    
                    ["menu.edlFactoryReset"] = "EDL restablecimiento de fábrica",
                    
                    ["tab.partManage"] = "Gestor de particiones",
                    ["tab.fileManage"] = "Gestor de archivos",
                    
                    ["scrcpy.execute"] = "Ejecutar",
                    ["scrcpy.startMirror"] = "Iniciar espejo",
                    ["scrcpy.fixMirror"] = "Reparar espejo",
                    ["scrcpy.flashZip"] = "Flash ZIP",
                    ["scrcpy.screenOn"] = "Pantalla siempre encendida",
                    ["scrcpy.audioForward"] = "Reenvío de audio",
                    ["scrcpy.autoReconnect"] = "Reconexión automática",
                    ["scrcpy.battery"] = "Batería",
                    ["scrcpy.refreshRate"] = "Tasa de refresco",
                    ["scrcpy.resolution"] = "Resolución",
                    ["scrcpy.powerKey"] = "Encendido",
                    ["scrcpy.recentKey"] = "Recientes",
                    ["scrcpy.homeKey"] = "Inicio",
                    ["scrcpy.backKey"] = "Atrás",
                    
                    ["status.loading"] = "Cargando...",
                    ["app.version"] = "v3.0 · Gratis para siempre"
                }
            };
        }
    }
}
