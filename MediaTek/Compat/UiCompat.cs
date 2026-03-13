// Compatibility shims for Form1.MediaTek.cs and Form1.MediaTek.UI.cs
// These types bridge the old SakuraEDL UI layer to the new mtkclient-based backend.
// They will be fully wired up during UI integration (TODO item #15).

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;

// ============================================================================
// SakuraEDL.MediaTek.Models
// ============================================================================
namespace SakuraEDL.MediaTek.Models
{
    public enum MtkDeviceState
    {
        Disconnected,
        Handshaking,
        Brom,
        Preloader,
        Da1Loaded,
        Da2Loaded,
        Error
    }

    public class MtkChipInfo
    {
        public ushort HwCode { get; set; }
        public ushort HwVer { get; set; }
        public ushort SwVer { get; set; }
        public string ChipName { get; set; }
        public string DaMode { get; set; }
        public bool SupportsXFlash { get; set; }
        public bool HasExploit { get; set; }

        public string GetChipName()
        {
            if (!string.IsNullOrEmpty(ChipName)) return ChipName;
            return $"MT{HwCode:X4}";
        }
    }

    public class MtkDeviceInfo
    {
        public MtkChipInfo ChipInfo { get; set; }
        public byte[] MeId { get; set; }
        public byte[] SocId { get; set; }
        public string MeIdHex => MeId != null ? BitConverter.ToString(MeId).Replace("-", "") : null;
        public string SocIdHex => SocId != null ? BitConverter.ToString(SocId).Replace("-", "") : null;
    }

    public class MtkPartitionInfo
    {
        public string Name { get; set; }
        public string Type { get; set; } = "";
        public ulong Offset { get; set; }
        public ulong Size { get; set; }
        public ulong StartSector { get; set; }

        public override string ToString() => $"{Name} (0x{Offset:X}-0x{Offset + Size:X})";
    }
}

// ============================================================================
// SakuraEDL.MediaTek.Common
// ============================================================================
namespace SakuraEDL.MediaTek.Common
{
    public static class MtkChipAliases
    {
        private static readonly Dictionary<ushort, string[]> Aliases = new Dictionary<ushort, string[]>
        {
            { 0x0717, new[] { "Helio A20", "Helio P22" } },
            { 0x0766, new[] { "Helio P35", "Helio G35" } },
            { 0x0788, new[] { "Helio P65", "Helio G85" } },
            { 0x0725, new[] { "Helio P60", "Helio P70" } },
            { 0x0813, new[] { "Helio G90", "Helio G95" } },
            { 0x0886, new[] { "Dimensity 800" } },
            { 0x0816, new[] { "Dimensity 1000" } },
            { 0x0950, new[] { "Dimensity 1200" } },
            { 0x0900, new[] { "Dimensity 9000" } },
            { 0x1296, new[] { "Dimensity 9200" } },
        };

        public static string[] GetAliases(ushort hwCode)
        {
            return Aliases.TryGetValue(hwCode, out var aliases) ? aliases : null;
        }
    }

    public class MtkPortInfo
    {
        public string ComPort { get; set; }
        public ushort Vid { get; set; }
        public ushort Pid { get; set; }
    }

    public class MtkPortDetector : IDisposable
    {
        // MediaTek USB VID
        private const ushort MTK_VID = 0x0E8D;
        // Known PIDs: BROM=0x0003, Preloader=0x2000/0x2001, DA=0x20FF
        private static readonly ushort[] MTK_PIDS = { 0x0003, 0x2000, 0x2001, 0x20FF };

        /// <summary>
        /// 轮询检测 MediaTek USB VCOM 端口，超时返回 null。
        /// </summary>
        public async Task<MtkPortInfo> WaitForDeviceAsync(int timeoutMs, CancellationToken ct)
        {
            int elapsed = 0;
            const int pollInterval = 500;

            while (elapsed < timeoutMs && !ct.IsCancellationRequested)
            {
                var port = ScanForMtkPort();
                if (port != null) return port;

                await Task.Delay(pollInterval, ct).ConfigureAwait(false);
                elapsed += pollInterval;
            }
            return null;
        }

        /// <summary>
        /// 通过 WMI 查询 Win32_PnPEntity 检测 MTK USB 串口。
        /// </summary>
        private MtkPortInfo ScanForMtkPort()
        {
            try
            {
                using (var searcher = new System.Management.ManagementObjectSearcher(
                    "SELECT * FROM Win32_PnPEntity WHERE Caption LIKE '%(COM%'"))
                {
                    foreach (var obj in searcher.Get())
                    {
                        string deviceId = obj["DeviceID"]?.ToString() ?? "";
                        string caption = obj["Caption"]?.ToString() ?? "";

                        // 检查是否为 MediaTek VID
                        string vidStr = $"VID_{MTK_VID:X4}";
                        if (deviceId.IndexOf(vidStr, StringComparison.OrdinalIgnoreCase) < 0)
                            continue;

                        // 提取 PID
                        ushort pid = 0;
                        int pidIdx = deviceId.IndexOf("PID_", StringComparison.OrdinalIgnoreCase);
                        if (pidIdx >= 0 && pidIdx + 8 <= deviceId.Length)
                        {
                            ushort.TryParse(deviceId.Substring(pidIdx + 4, 4),
                                System.Globalization.NumberStyles.HexNumber, null, out pid);
                        }

                        // 提取 COM 端口号
                        string comPort = null;
                        int comStart = caption.LastIndexOf("(COM", StringComparison.OrdinalIgnoreCase);
                        if (comStart >= 0)
                        {
                            int comEnd = caption.IndexOf(')', comStart);
                            if (comEnd > comStart)
                                comPort = caption.Substring(comStart + 1, comEnd - comStart - 1);
                        }

                        if (!string.IsNullOrEmpty(comPort))
                        {
                            return new MtkPortInfo
                            {
                                ComPort = comPort,
                                Vid = MTK_VID,
                                Pid = pid
                            };
                        }
                    }
                }
            }
            catch
            {
                // WMI 不可用时回退到串口枚举
                return ScanFallback();
            }
            return null;
        }

        /// <summary>
        /// WMI 不可用时的回退方案：枚举所有串口尝试握手。
        /// </summary>
        private MtkPortInfo ScanFallback()
        {
            foreach (string port in System.IO.Ports.SerialPort.GetPortNames())
            {
                try
                {
                    using (var sp = new System.IO.Ports.SerialPort(port, 115200))
                    {
                        sp.ReadTimeout = 200;
                        sp.WriteTimeout = 200;
                        sp.Open();
                        // 发送 MTK BROM 握手字节 0xA0
                        sp.Write(new byte[] { 0xA0 }, 0, 1);
                        byte[] buf = new byte[1];
                        int read = sp.Read(buf, 0, 1);
                        if (read == 1 && buf[0] == 0x5F) // BROM ACK
                        {
                            return new MtkPortInfo { ComPort = port, Vid = MTK_VID, Pid = 0x0003 };
                        }
                    }
                }
                catch { /* 端口不可用或超时 */ }
            }
            return null;
        }

        public void Dispose() { }
    }

    public static class RealmeAuthService
    {
        public static string FindAllInOneSignature(string firmwarePath)
        {
            if (string.IsNullOrEmpty(firmwarePath)) return null;
            string sigFile = System.IO.Path.Combine(firmwarePath, "all-in-one-signature.bin");
            return System.IO.File.Exists(sigFile) ? sigFile : null;
        }
    }
}

// ============================================================================
// SakuraEDL.MediaTek.Database
// ============================================================================
namespace SakuraEDL.MediaTek.Database
{
    public class MtkChipEntry
    {
        public ushort HwCode { get; set; }
        public string ChipName { get; set; }
        public string Description { get; set; } = "";
        public bool HasExploit { get; set; }
    }

    public static class MtkChipDatabase
    {
        public static List<MtkChipEntry> GetAllChips()
        {
            var chips = new List<MtkChipEntry>();
            foreach (var kv in Config.BromConfig.HwConfig)
            {
                chips.Add(new MtkChipEntry
                {
                    HwCode = kv.Key,
                    ChipName = kv.Value.Name,
                    HasExploit = kv.Value.Blacklist != null && kv.Value.Blacklist.Length > 0
                });
            }
            return chips;
        }

        public static MtkChipEntry GetChip(ushort hwCode)
        {
            if (Config.BromConfig.HwConfig.TryGetValue(hwCode, out var cfg))
            {
                return new MtkChipEntry
                {
                    HwCode = hwCode,
                    ChipName = cfg.Name,
                    HasExploit = cfg.Blacklist != null && cfg.Blacklist.Length > 0
                };
            }
            return null;
        }

        public static string GetExploitType(ushort hwCode)
        {
            if (Config.BromConfig.HwConfig.TryGetValue(hwCode, out var cfg))
            {
                if (cfg.DaMode == Config.DAmodes.XML) return "Carbonara";
                if (cfg.Blacklist != null && cfg.Blacklist.Length > 0) return "Kamakiri2";
            }
            return "None";
        }

        public static bool IsAllinoneSignatureSupported(ushort hwCode)
        {
            // AllinoneSignature was removed per user request
            return false;
        }

        public static List<MtkChipEntry> GetAllinoneSignatureChips()
        {
            return new List<MtkChipEntry>();
        }
    }

    public static class MtkDaDatabase
    {
        public static bool SupportsExploit(ushort hwCode)
        {
            if (Config.BromConfig.HwConfig.TryGetValue(hwCode, out var cfg))
                return cfg.Blacklist != null && cfg.Blacklist.Length > 0;
            return false;
        }
    }
}

// ============================================================================
// SakuraEDL.MediaTek.Services
// ============================================================================
namespace SakuraEDL.MediaTek.Services
{
    using SakuraEDL.MediaTek.Models;

    public class MediatekService
    {
        private MtkClass _mtk;

        public MtkDeviceState State { get; set; } = MtkDeviceState.Disconnected;
        public MtkDeviceInfo CurrentDevice { get; set; }
        public MtkChipInfo ChipInfo => CurrentDevice?.ChipInfo;
        public bool IsXFlashMode => _mtk?.Config?.ChipConfig?.DaMode == Config.DAmodes.XFLASH;

        public event Action<int, int> OnProgress;
        public event Action<MtkDeviceState> OnStateChanged;
        public event Action<string, Color> OnLog;

        public void SetDaFilePath(string path) { if (_mtk?.Config != null) _mtk.Config.Loader = path; }
        public void SetCustomDa1(string path) { /* TODO */ }
        public void SetCustomDa2(string path) { /* TODO */ }
        public byte[] SignatureData { get; set; }

        public void ConfigureRealmeAuth(string apiUrl, string apiKey, string account, SakuraEDL.MediaTek.Auth.SignServerType serverType)
        {
            // TODO: Wire up Realme cloud auth configuration
        }

        public async Task<bool> AuthWithAllInOneSignatureAsync(string sigPath, CancellationToken ct)
        {
            // AllinoneSignature removed per user request
            return await Task.FromResult(false);
        }

        public async Task<byte[]> GetGsmFutureSignatureAsync(string projectNo, string nvCode, string newSw, string oldSw, CancellationToken ct)
        {
            // TODO: Implement cloud signing API call
            return await Task.FromResult<byte[]>(null);
        }

        public async Task<bool> ExecuteRealmeAuthWithSignatureAsync(byte[] signature, CancellationToken ct)
        {
            // TODO: Execute auth with pre-fetched signature
            return await Task.FromResult(false);
        }

        public async Task<bool> ConnectAsync(string comPort, int baudRate, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                try
                {
                    _mtk = new MtkClass();
                    _mtk.OnInfo = msg => OnLog?.Invoke($"[MTK] {msg}", Color.Cyan);
                    _mtk.OnError = msg => OnLog?.Invoke($"[MTK] {msg}", Color.Red);
                    _mtk.OnWarning = msg => OnLog?.Invoke($"[MTK] {msg}", Color.Orange);

                    State = MtkDeviceState.Handshaking;
                    OnStateChanged?.Invoke(State);

                    _mtk.Port.PortName = comPort;
                    if (!_mtk.Init(comPort))
                    {
                        State = MtkDeviceState.Error;
                        OnStateChanged?.Invoke(State);
                        return false;
                    }

                    CurrentDevice = new MtkDeviceInfo
                    {
                        ChipInfo = new MtkChipInfo
                        {
                            HwCode = _mtk.Config.HwCode,
                            HwVer = _mtk.Config.HwVer,
                            ChipName = _mtk.Config.ChipConfig?.Name ?? $"MT{_mtk.Config.HwCode:X4}",
                            DaMode = _mtk.Config.ChipConfig?.DaMode.ToString() ?? "Unknown",
                            SupportsXFlash = _mtk.Config.ChipConfig?.DaMode == Config.DAmodes.XFLASH,
                            HasExploit = _mtk.Config.ChipConfig?.Blacklist?.Length > 0
                        },
                        MeId = _mtk.Config.Meid,
                        SocId = _mtk.Config.SocId
                    };

                    State = _mtk.Config.IsBrom ? MtkDeviceState.Brom : MtkDeviceState.Preloader;
                    OnStateChanged?.Invoke(State);
                    return true;
                }
                catch (Exception ex)
                {
                    OnLog?.Invoke($"[MTK] Connect error: {ex.Message}", Color.Red);
                    State = MtkDeviceState.Error;
                    OnStateChanged?.Invoke(State);
                    return false;
                }
            }, ct);
        }

        public async Task<bool> LoadDaAsync(CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk == null) return false;
                State = MtkDeviceState.Da1Loaded;
                OnStateChanged?.Invoke(State);

                bool ok = _mtk.UploadDa();
                State = ok ? MtkDeviceState.Da2Loaded : MtkDeviceState.Error;
                OnStateChanged?.Invoke(State);
                return ok;
            }, ct);
        }

        public async Task<List<MtkPartitionInfo>> ReadPartitionTableAsync(CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                var result = new List<MtkPartitionInfo>();
                if (_mtk?.DaHandler == null)
                {
                    OnLog?.Invoke("[MTK] DaHandler not initialized", Color.Red);
                    return result;
                }
                var gpt = _mtk.DaHandler.ReadGpt();
                if (gpt != null)
                {
                    foreach (var entry in gpt)
                    {
                        result.Add(new MtkPartitionInfo
                        {
                            Name = entry.Name,
                            Offset = entry.StartLba * 512,
                            Size = entry.SizeBytes,
                            StartSector = entry.StartLba
                        });
                    }
                }
                return result;
            }, ct);
        }

        public async Task<bool> ReadPartitionAsync(string name, string outputPath, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk?.DaHandler == null) return false;
                return _mtk.DaHandler.ReadPartition(name, outputPath,
                    pct => OnProgress?.Invoke(pct, 100));
            }, ct);
        }

        public async Task<bool> ReadPartitionAsync(string name, string outputPath, ulong size, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk?.DaHandler == null) return false;
                return _mtk.DaHandler.ReadPartition(name, outputPath,
                    pct => OnProgress?.Invoke(pct, 100));
            }, ct);
        }

        public async Task<bool> WritePartitionAsync(string name, string imagePath, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk?.DaHandler == null) return false;
                return _mtk.DaHandler.WritePartition(name, imagePath,
                    pct => OnProgress?.Invoke(pct, 100));
            }, ct);
        }

        public async Task<bool> ErasePartitionAsync(string name, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk?.DaHandler == null) return false;
                return _mtk.DaHandler.ErasePartition(name);
            }, ct);
        }

        public void Dispose()
        {
            _mtk?.Dispose();
            _mtk = null;
        }

        public async Task<bool> RebootAsync(CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                if (_mtk?.DaLoader?.Xft != null)
                    return _mtk.DaLoader.Xft.Shutdown();
                return false;
            }, ct);
        }

        public async Task<bool> RunAllinoneSignatureExploitAsync(
            string daPath = null, string sigPath = null, CancellationToken ct = default)
        {
            // AllinoneSignature removed per user request
            return await Task.FromResult(false);
        }

        public SakuraEDL.MediaTek.Auth.RealmSignRequest GetRealmeSignRequest()
        {
            if (CurrentDevice?.ChipInfo == null) return null;
            return new SakuraEDL.MediaTek.Auth.RealmSignRequest
            {
                HwCode = CurrentDevice.ChipInfo.HwCode,
                MeId = CurrentDevice.MeIdHex,
                SocId = CurrentDevice.SocIdHex
            };
        }
    }
}

// ============================================================================
// SakuraEDL.MediaTek.UI
// ============================================================================
namespace SakuraEDL.MediaTek.UI
{
    using SakuraEDL.MediaTek.Models;

    public class MediatekUIController
    {
        private readonly Action<string, Color> _log;
        private readonly Action<string> _debug;

        public event Action<int, int> OnProgress;
        public event Action<MtkDeviceState> OnStateChanged;
        public event Action<MtkDeviceInfo> OnDeviceConnected;
        public event Action<MtkDeviceInfo> OnDeviceDisconnected;
        public event Action<List<MtkPartitionInfo>> OnPartitionTableLoaded;

        public MediatekUIController(Action<string, Color> log, Action<string> debug)
        {
            _log = log;
            _debug = debug;
        }

        public void ReportProgress(int current, int total) => OnProgress?.Invoke(current, total);
        public void ReportState(MtkDeviceState state) => OnStateChanged?.Invoke(state);
        public void ReportDeviceConnected(MtkDeviceInfo device) => OnDeviceConnected?.Invoke(device);
        public void ReportDeviceDisconnected(MtkDeviceInfo device) => OnDeviceDisconnected?.Invoke(device);
        public void ReportPartitions(List<MtkPartitionInfo> parts) => OnPartitionTableLoaded?.Invoke(parts);

        public void Dispose() { }
        public void StopPortMonitoring() { }
        public void StartPortMonitoring() { }
    }
}

// ============================================================================
// SakuraEDL.MediaTek.Auth - SignServerType & RealmSignRequest
// ============================================================================
namespace SakuraEDL.MediaTek.Auth
{
    public enum SignServerType
    {
        Realme,
        Oppo,
        OnePlus
    }

    public class RealmSignRequest
    {
        public ushort HwCode { get; set; }
        public string MeId { get; set; }
        public string SocId { get; set; }
        public string Platform { get; set; }
        public string Chipset { get; set; }
        public string SerialNumber { get; set; }
        public string Challenge { get; set; }
        public string ProjectNo { get; set; }
        public string NvCode { get; set; }
    }
}
