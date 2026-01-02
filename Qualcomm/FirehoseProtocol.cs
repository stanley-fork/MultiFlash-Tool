using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;

namespace OPFlashTool.Qualcomm
{
    #region High-Speed USB Writer (based on UnlockTool DiskWriter)
    
    /// <summary>
    /// 高速 USB 通信层 - 直接使用 kernel32.dll API
    /// 相比 SerialPort 类有更高的传输性能
    /// </summary>
    public class HighSpeedUsbWriter : IDisposable
    {
        #region P/Invoke Declarations
        
        private const uint ERROR_IO_PENDING = 997;
        private const uint PURGE_TXABORT = 0x0001;
        private const uint PURGE_RXABORT = 0x0002;
        private const uint PURGE_TXCLEAR = 0x0004;
        private const uint PURGE_RXCLEAR = 0x0008;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string lpFileName,
            [MarshalAs(UnmanagedType.U4)] FileAccess dwDesiredAccess,
            [MarshalAs(UnmanagedType.U4)] FileShare dwShareMode,
            IntPtr lpSecurityAttributes,
            [MarshalAs(UnmanagedType.U4)] FileMode dwCreationDisposition,
            [MarshalAs(UnmanagedType.U4)] FileAttributes dwFlagsAndAttributes,
            IntPtr hTemplateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ClearCommError(IntPtr hFile, out uint lpErrors, out COMSTAT lpStat);

        [StructLayout(LayoutKind.Sequential)]
        private struct COMSTAT
        {
            public uint Flags;
            public uint cbInQue;
            public uint cbOutQue;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetupComm(IntPtr hFile, uint dwInQueue, uint dwOutQueue);

        [StructLayout(LayoutKind.Sequential)]
        private struct DCB
        {
            public uint DCBlength;
            public uint BaudRate;
            public uint Flags;
            public ushort wReserved;
            public ushort XonLim;
            public ushort XoffLim;
            public byte ByteSize;
            public byte Parity;
            public byte StopBits;
            public byte XonChar;
            public byte XoffChar;
            public byte ErrorChar;
            public byte EofChar;
            public byte EvtChar;
            public ushort wReserved1;

            public uint fBinary { get => Flags & 0x1; set => Flags = (Flags & ~0x1U) | (value & 0x1); }
            public uint fParity { get => (Flags >> 1) & 0x1; set => Flags = (Flags & ~0x2U) | ((value & 0x1) << 1); }
            public uint fOutX { get => (Flags >> 8) & 0x1; set => Flags = (Flags & ~0x100U) | ((value & 0x1) << 8); }
            public uint fInX { get => (Flags >> 9) & 0x1; set => Flags = (Flags & ~0x200U) | ((value & 0x1) << 9); }
            public uint fAbortOnError { get => (Flags >> 14) & 0x1; set => Flags = (Flags & ~0x4000U) | ((value & 0x1) << 14); }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetCommState(IntPtr hFile, out DCB lpDCB);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommState(IntPtr hFile, ref DCB lpDCB);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ClearCommBreak(IntPtr hFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool EscapeCommFunction(IntPtr hFile, uint dwFunc);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushFileBuffers(IntPtr hFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool WriteFile(IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToWrite, out uint lpNumberOfBytesWritten, IntPtr lpOverlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(IntPtr hFile, byte[] lpBuffer, uint nNumberOfBytesToRead, out uint lpNumberOfBytesRead, IntPtr lpOverlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool PurgeComm(IntPtr hFile, uint dwFlags);

        [StructLayout(LayoutKind.Sequential)]
        private struct COMMTIMEOUTS
        {
            public uint ReadIntervalTimeout;
            public uint ReadTotalTimeoutMultiplier;
            public uint ReadTotalTimeoutConstant;
            public uint WriteTotalTimeoutMultiplier;
            public uint WriteTotalTimeoutConstant;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetCommTimeouts(IntPtr hFile, out COMMTIMEOUTS lpCommTimeouts);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetCommTimeouts(IntPtr hFile, ref COMMTIMEOUTS lpCommTimeouts);

        #endregion

        private string _portName;
        private SafeFileHandle _hSerial;
        private COMSTAT _portStat;
        private DCB _dcbParams;
        private int _maxPayloadSize = 1048576; // 1MB default

        public string PortName
        {
            get => _portName;
            set => _portName = value;
        }

        public bool IsOpen => _hSerial != null && !_hSerial.IsInvalid && !_hSerial.IsClosed;

        public int BytesToRead
        {
            get
            {
                if (!IsOpen) return 0;
                if (!ClearCommError(_hSerial.DangerousGetHandle(), out _, out _portStat))
                    return 0;
                return (int)_portStat.cbInQue;
            }
        }

        public int MaxPayloadSize
        {
            get => _maxPayloadSize;
            set => _maxPayloadSize = value;
        }

        public bool Open()
        {
            if (string.IsNullOrEmpty(_portName))
                throw new Exception("[HighSpeed] 端口号为空");

            _hSerial = CreateFile(_portName, FileAccess.ReadWrite, FileShare.None, IntPtr.Zero, FileMode.Open, FileAttributes.Normal, IntPtr.Zero);

            if (!IsOpen)
                throw new Exception($"[HighSpeed] 无法打开端口 {_portName}");

            return Reconfigure();
        }

        public bool Reconfigure()
        {
            if (!IsOpen) return false;

            IntPtr handle = _hSerial.DangerousGetHandle();

            // 清除错误
            ClearCommError(handle, out _, out _portStat);

            // 设置大缓冲区 (2MB) - 关键优化点
            if (!SetupComm(handle, 0x200000, 0x200000))
            {
                System.Diagnostics.Debug.WriteLine("SetupComm Failed");
            }

            // 获取并设置串口参数
            if (!GetCommState(handle, out _dcbParams))
            {
                System.Diagnostics.Debug.WriteLine("GetCommState Failed");
            }

            _dcbParams.BaudRate = 115200;
            _dcbParams.fAbortOnError = 1;
            _dcbParams.fBinary = 1;
            _dcbParams.ByteSize = 8;
            _dcbParams.StopBits = 0;
            _dcbParams.fParity = 0;
            _dcbParams.Parity = 0;
            _dcbParams.fOutX = 0;
            _dcbParams.fInX = 0;
            _dcbParams.DCBlength = (uint)Marshal.SizeOf<DCB>();

            if (!SetCommState(handle, ref _dcbParams))
            {
                System.Diagnostics.Debug.WriteLine("SetCommState Failed");
                return false;
            }

            ClearCommBreak(handle);
            EscapeCommFunction(handle, 0x3); // SETDTR
            EscapeCommFunction(handle, 0x5); // SETRTS

            // 设置超时
            var timeouts = new COMMTIMEOUTS
            {
                ReadIntervalTimeout = 0,
                ReadTotalTimeoutMultiplier = 0,
                ReadTotalTimeoutConstant = 3000, // 3秒读取超时
                WriteTotalTimeoutMultiplier = 0,
                WriteTotalTimeoutConstant = 3000  // 3秒写入超时
            };
            SetCommTimeouts(handle, ref timeouts);

            return true;
        }

        public int Write(byte[] data)
        {
            if (!IsOpen && !Open())
                throw new Exception("[HighSpeed] 写入失败: 端口未打开");

            FlushFileBuffers(_hSerial.DangerousGetHandle());

            bool success = WriteFile(_hSerial.DangerousGetHandle(), data, (uint)data.Length, out var bytesWritten, IntPtr.Zero);

            if (!success && Marshal.GetLastWin32Error() != ERROR_IO_PENDING)
                throw new Exception($"[HighSpeed] 写入失败: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");

            return (int)bytesWritten;
        }

        public byte[] Read(int count = -1)
        {
            if (!IsOpen && !Open())
                throw new Exception("[HighSpeed] 读取失败: 端口未打开");

            FlushFileBuffers(_hSerial.DangerousGetHandle());

            int len = count <= 0 ? _maxPayloadSize : count;
            byte[] buffer = new byte[len];

            bool success = ReadFile(_hSerial.DangerousGetHandle(), buffer, (uint)len, out var bytesRead, IntPtr.Zero);

            if (!success && Marshal.GetLastWin32Error() != ERROR_IO_PENDING)
                throw new Exception($"[HighSpeed] 读取失败: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");

            if (bytesRead > 0)
            {
                byte[] result = new byte[bytesRead];
                Array.Copy(buffer, result, bytesRead);
                return result;
            }

            return Array.Empty<byte>();
        }

        public void Purge()
        {
            if (IsOpen)
            {
                PurgeComm(_hSerial.DangerousGetHandle(), PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
            }
        }

        public void Close()
        {
            if (IsOpen)
            {
                Purge();
                _hSerial.Close();
                _hSerial = null;
            }
        }

        public void Dispose()
        {
            Close();
            GC.SuppressFinalize(this);
        }
    }
    
    #endregion

    #region Firehose XML Packet Builder

        /// <summary>
    /// Firehose XML 包构建器 (基于 UnlockTool Firehose_pkts)
        /// </summary>
    public static class FirehosePackets
    {
        public struct FirehoseConfig
        {
            public int Version;
            public string MemoryName;
            public int SkipWrite;
            public int SkipStorageInit;
            public int ZLPAwareHost;
            public int ActivePartition;
            public int MaxPayloadSizeToTargetInBytes;
            public int AckRawDataEveryNumPackets;
        }

        /// <summary>
        /// 生成配置命令 (增强版 - 支持更多参数)
        /// </summary>
        public static string Configure(string memoryName, int maxPayload = 1048576, 
            int zlpAwareHost = 1, int skipStorageInit = 0, int skipWrite = 0,
            int maxDigestTableSize = 2048, int verbose = 0)
        {
            return $"<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data><configure " +
                   $"MemoryName=\"{memoryName}\" " +
                   $"ZLPAwareHost=\"{zlpAwareHost}\" " +
                   $"SkipStorageInit=\"{skipStorageInit}\" " +
                   $"SkipWrite=\"{skipWrite}\" " +
                   $"MaxPayloadSizeToTargetInBytes=\"{maxPayload}\" " +
                   $"MaxDigestTableSizeInBytes=\"{maxDigestTableSize}\" " +
                   $"AlwaysValidate=\"0\" Verbose=\"{verbose}\"/></data>";
        }

        /// <summary>
        /// 生成配置命令 (简化版)
        /// </summary>
        public static string ConfigureSimple(string memoryName, int maxPayload = 1048576)
        {
            return $"<?xml version=\"1.0\" ?><data><configure MemoryName=\"{memoryName}\" " +
                   $"ZLPAwareHost=\"1\" SkipStorageInit=\"0\" SkipWrite=\"0\" " +
                   $"MaxPayloadSizeToTargetInBytes=\"{maxPayload}\" " +
                   $"AlwaysValidate=\"0\" Verbose=\"0\" EnableFlash=\"1\" /></data>";
        }

        public static string SetAckRawData(int value)
        {
            return $"<?xml version=\"1.0\" ?><data><configure AckRawData=\"{value}\"/></data>";
        }

        public static string GetStorageInfo(int physicalPartition = 0)
        {
            return $"<?xml version=\"1.0\" ?><data><getstorageinfo physical_partition_number=\"{physicalPartition}\"/></data>";
        }

        public static string Read(int sectorSize, long numSectors, int physicalPartition, string startSector, string filename = "image")
        {
            return $"<?xml version=\"1.0\" ?><data><read SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{physicalPartition}\" " +
                   $"start_sector=\"{startSector}\" filename=\"{filename}\" sparse=\"false\" /></data>";
        }

        public static string Program(int sectorSize, long numSectors, int physicalPartition, string startSector, string filename = "image")
        {
            return $"<?xml version=\"1.0\" ?><data><program SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"file_sector_offset=\"0\" num_partition_sectors=\"{numSectors}\" " +
                   $"physical_partition_number=\"{physicalPartition}\" start_sector=\"{startSector}\" " +
                   $"filename=\"{filename}\" sparse=\"false\" /></data>";
        }

        public static string Erase(int sectorSize, long numSectors, int physicalPartition, string startSector)
        {
            return $"<?xml version=\"1.0\" ?><data><erase SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{physicalPartition}\" " +
                   $"start_sector=\"{startSector}\" /></data>";
        }

        public static string Patch(int sectorSize, long byteOffset, int physicalPartition, int sizeInBytes, string startSector, string value, string filename = "DISK")
        {
            return $"<?xml version=\"1.0\" ?><data><patch SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"byte_offset=\"{byteOffset}\" filename=\"{filename}\" " +
                   $"physical_partition_number=\"{physicalPartition}\" size_in_bytes=\"{sizeInBytes}\" " +
                   $"start_sector=\"{startSector}\" value=\"{value}\" /></data>";
        }

        public static string Peek(ulong address64, int sizeInBytes)
        {
            return $"<?xml version=\"1.0\" ?><data><peek address64=\"{address64}\" SizeInBytes=\"{sizeInBytes}\"/></data>";
        }

        public static string Poke(ulong address64, int sizeInBytes, string value)
        {
            return $"<?xml version=\"1.0\" ?><data><poke address64=\"{address64}\" SizeInBytes=\"{sizeInBytes}\" value=\"{value}\"/></data>";
        }

        public static string Nop()
        {
            return "<?xml version=\"1.0\" ?><data><nop /></data>";
        }

        public static string Reset()
        {
            return "<?xml version=\"1.0\" ?><data><power value=\"reset\"/></data>";
        }

        public static string Power(string mode)
        {
            return $"<?xml version=\"1.0\" ?><data><power value=\"{mode}\"/></data>";
        }

        public static string SetBootableStorageDrive(int value)
        {
            return $"<?xml version=\"1.0\" ?><data><setbootablestoragedrive value=\"{value}\" /></data>";
        }

        public static string GetSecureBootStatus()
        {
            return "<?xml version=\"1.0\" ?><data><getSecureBootStatus/></data>";
        }

        public static string GetDeviceInfo()
        {
            return "<?xml version=\"1.0\" ?><data><getdevinfo /></data>";
        }

        public static string Signature(int sizeInBytes)
        {
            return $"<?xml version=\"1.0\" ?><data><sig TargetName=\"sig\" size_in_bytes=\"{sizeInBytes}\" verbose=\"1\"/></data>";
        }

        public static string Sha256Init()
        {
            return "<?xml version=\"1.0\" ?><data><sha256init Verbose=\"1\"/></data>";
        }

        public static string Verify()
        {
            return "<?xml version=\"1.0\" ?><data><verify value=\"ping\" EnableVip=\"1\"/></data>";
        }

        public static string GetSha256Digest(int sectorSize, long numSectors, int physicalPartition, string startSector)
        {
            return $"<?xml version=\"1.0\" ?><data><getsha256digest SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{physicalPartition}\" " +
                   $"start_sector=\"{startSector}\" /></data>";
        }

        /// <summary>
        /// 写入 IMEI (小米/OPPO 设备支持)
        /// </summary>
        public static string WriteIMEI(int len = 16)
        {
            return $"<?xml version=\"1.0\" ?><data><writeIMEI len=\"{len}\"/></data>";
        }

        /// <summary>
        /// Benchmark 测试
        /// </summary>
        public static string Benchmark(int trials = 1000)
        {
            return $"<?xml version=\"1.0\" ?><data><benchmark trials=\"{trials}\"/></data>";
        }

        /// <summary>
        /// 获取 CRC16 摘要
        /// </summary>
        public static string GetCrc16Digest(int sectorSize, long numSectors, int physicalPartition, string startSector)
        {
            return $"<?xml version=\"1.0\" ?><data><getcrc16digest SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{physicalPartition}\" " +
                   $"start_sector=\"{startSector}\" /></data>";
        }

        /// <summary>
        /// Firmware 写入
        /// </summary>
        public static string FirmwareWrite(int sectorSize, long numSectors, int physicalPartition, string startSector)
        {
            return $"<?xml version=\"1.0\" ?><data><firmwarewrite SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" " +
                   $"num_partition_sectors=\"{numSectors}\" physical_partition_number=\"{physicalPartition}\" " +
                   $"start_sector=\"{startSector}\" /></data>";
        }

        /// <summary>
        /// 小米 Demacia 认证
        /// </summary>
        public static string MiDemacia(string token, string pk = "")
        {
            if (string.IsNullOrEmpty(pk))
                return $"<?xml version=\"1.0\" ?><data><demacia token=\"{token}\" /></data>";
            else
                return $"<?xml version=\"1.0\" ?><data><demacia token=\"{token}\" pk=\"{pk}\" /></data>";
        }

        /// <summary>
        /// 小米项目模型设置
        /// </summary>
        public static string MiSetProjModel(string projId)
        {
            return $"<?xml version=\"1.0\" ?><data><setprojmodel token=\"{projId}\" /></data>";
        }

        /// <summary>
        /// 小米 SW 项目模型设置
        /// </summary>
        public static string MiSetSwProjModel(string projId, string token)
        {
            return $"<?xml version=\"1.0\" ?><data><setswprojmodel projid=\"{projId}\" token=\"{token}\" /></data>";
        }

        /// <summary>
        /// Nothing Phone 认证
        /// </summary>
        public static string CheckNtFeature(string projId)
        {
            return $"<?xml version=\"1.0\" ?><data><checkntfeature projid=\"{projId}\" /></data>";
        }

        /// <summary>
        /// 原始 XML 命令
        /// </summary>
        public static string RawXml(string content)
        {
            return $"<?xml version=\"1.0\" ?><data><{content} /></data>";
        }
    }
    
    #endregion

    /// <summary>
    /// 优化版 Firehose 客户端
    /// 整合 UnlockTool 的高速传输和完整命令集
    /// 基于 bkerler/edl 优化
    /// </summary>
    public class FirehoseClient : IDisposable
    {
        // 双模式支持: SerialPort (兼容) / HighSpeedUsbWriter (高性能)
        private SerialPort _serialPort;
        private HighSpeedUsbWriter _highSpeedWriter;
        private bool _useHighSpeed = false;
        
        private Action<string> _log;
        private int _sectorSize = 4096;
        private int _maxPayloadSize = 1048576;
        private int _maxPayloadSizeFromTarget = 8192;
        private int _maxXmlSize = 4096;
        private readonly StringBuilder _rxBuffer = new StringBuilder();

        // 配置参数
        public string StorageType { get; private set; } = "ufs";
        public string TargetName { get; private set; } = "Unknown";
        public string Version { get; private set; } = "1";
        public int SectorSize => _sectorSize;
        public int MaxPayloadSize => _maxPayloadSize;
        public int MaxPayloadSizeFromTarget => _maxPayloadSizeFromTarget;
        public int MaxXmlSize => _maxXmlSize;

        // 重试配置
        public int MaxRetries { get; set; } = 3;
        public int RetryDelayMs { get; set; } = 500;
        public int ReadTimeoutMs { get; set; } = 5000;

        // 状态
        public bool IsConfigured { get; private set; }
        public DateTime LastActivity { get; private set; }

        // Sparse Image Constants
        private const uint SPARSE_HEADER_MAGIC = 0xED26FF3A;
        private const ushort CHUNK_TYPE_RAW = 0xCAC1;
        private const ushort CHUNK_TYPE_FILL = 0xCAC2;
        private const ushort CHUNK_TYPE_DONT_CARE = 0xCAC3;
        private const ushort CHUNK_TYPE_CRC32 = 0xCAC4;

        /// <summary>
        /// 构造函数 - SerialPort 模式 (兼容性)
        /// </summary>
        public FirehoseClient(SerialPort port, Action<string> logger)
        {
            _serialPort = port;
            _log = logger;
            _useHighSpeed = false;

            try
            {
                if (_serialPort.ReadBufferSize < 1024 * 1024) _serialPort.ReadBufferSize = 1024 * 1024;
                if (_serialPort.WriteBufferSize < 1024 * 1024) _serialPort.WriteBufferSize = 1024 * 1024;
                _serialPort.DtrEnable = true;
                _serialPort.RtsEnable = true;
            }
            catch { /* 忽略不支持的驱动 */ }
        }

        /// <summary>
        /// 构造函数 - 高速模式
        /// </summary>
        public FirehoseClient(string portName, Action<string> logger)
        {
            _highSpeedWriter = new HighSpeedUsbWriter { PortName = portName };
            _log = logger;
            _useHighSpeed = true;
        }

        /// <summary>
        /// 启用高速模式 (切换到 kernel32 直接调用)
        /// </summary>
        public bool EnableHighSpeedMode(string portName)
        {
            try
            {
                // 关闭现有 SerialPort
                if (_serialPort != null && _serialPort.IsOpen)
                {
                    _serialPort.Close();
                }

                _highSpeedWriter = new HighSpeedUsbWriter { PortName = portName };
                if (_highSpeedWriter.Open())
                {
                    _useHighSpeed = true;
                    _log?.Invoke("[HighSpeed] 高速 USB 模式已启用");
                return true;
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[HighSpeed] 启用失败: {ex.Message}");
            }
                return false;
        }

        #region 底层通信方法

        private void WriteBytes(byte[] data)
        {
            if (_useHighSpeed && _highSpeedWriter != null)
            {
                _highSpeedWriter.Write(data);
            }
            else if (_serialPort != null)
            {
                _serialPort.Write(data, 0, data.Length);
            }
        }

        private byte[] ReadBytes(int count = -1)
        {
            if (_useHighSpeed && _highSpeedWriter != null)
            {
                return _highSpeedWriter.Read(count);
            }
            else if (_serialPort != null)
            {
                int len = count <= 0 ? _maxPayloadSize : count;
                byte[] buffer = new byte[len];
                int read = _serialPort.Read(buffer, 0, len);
                if (read > 0)
                {
                    byte[] result = new byte[read];
                    Array.Copy(buffer, result, read);
                    return result;
                }
            }
            return Array.Empty<byte>();
        }

        private void PurgeBuffer()
        {
            try
            {
                if (_useHighSpeed && _highSpeedWriter != null)
                {
                    _highSpeedWriter.Purge();
                }
                else if (_serialPort != null)
                {
                    _serialPort.DiscardInBuffer();
                    _serialPort.DiscardOutBuffer();

                    // 手动清空剩余数据
                    int oldTimeout = _serialPort.ReadTimeout;
                    _serialPort.ReadTimeout = 10;
                    try
                    {
                        byte[] trash = new byte[4096];
                        while (_serialPort.Read(trash, 0, trash.Length) > 0) { }
                    }
                    catch (TimeoutException) { }
                    finally
                    {
                        _serialPort.ReadTimeout = oldTimeout;
                    }
                }
                _rxBuffer.Clear();
            }
            catch { }
        }

        #endregion

        #region 配置和初始化

        /// <summary>
        /// 配置 Firehose (支持高速传输协商 + 自动重试)
        /// 基于 bkerler/edl 优化
        /// </summary>
        public bool Configure(string storageType = "ufs", int requestedPayloadSize = 1048576)
        {
            return ConfigureWithRetry(storageType, requestedPayloadSize, 0);
        }

        /// <summary>
        /// 带重试的智能配置
        /// </summary>
        private bool ConfigureWithRetry(string storageType, int requestedPayloadSize, int retryLevel)
        {
            if (retryLevel > 3)
            {
                _log?.Invoke("[Config] 配置重试次数过多，放弃");
                return false;
            }

            StorageType = storageType.ToLower();
            _sectorSize = (StorageType == "emmc") ? 512 : 4096;

            string xml = FirehosePackets.Configure(storageType, requestedPayloadSize);
            WriteBytes(Encoding.UTF8.GetBytes(xml));
            LastActivity = DateTime.Now;

            int maxRetries = 50;
            while (maxRetries-- > 0)
            {
                var response = CheckConfigResponse();
                if (response.success)
                {
                    ApplyConfigResponse(response);
                    IsConfigured = true;
                    PurgeBuffer();
                    return true;
                }
                
                if (response.needsAuth)
                {
                    _log?.Invoke("[Config] 设备需要认证");
                    return false;
                }

                // 处理配置失败 - 尝试其他存储类型
                if (response.errorMsg != null)
                {
                    if (response.errorMsg.Contains("Not support configure MemoryName eMMC"))
                    {
                        _log?.Invoke("[Config] eMMC 不支持，尝试 UFS...");
                        return ConfigureWithRetry("UFS", requestedPayloadSize, retryLevel + 1);
                    }
                    else if (response.errorMsg.Contains("Failed to open the SDCC Device") ||
                             response.errorMsg.Contains("Not support configure MemoryName UFS"))
                    {
                        _log?.Invoke("[Config] UFS 不支持，尝试 eMMC...");
                        return ConfigureWithRetry("eMMC", requestedPayloadSize, retryLevel + 1);
                    }
                    else if (response.errorMsg.Contains("Failed to set the IO options"))
                    {
                        _log?.Invoke("[Config] IO 选项失败，尝试 NAND...");
                        return ConfigureWithRetry("nand", requestedPayloadSize, retryLevel + 1);
                    }
                    else if (response.errorMsg.Contains("SECTOR_SIZE_IN_BYTES") && response.errorMsg.Contains("512"))
                    {
                        _log?.Invoke("[Config] 扇区大小不匹配，切换到 512...");
                        _sectorSize = 512;
                        return ConfigureWithRetry(storageType, requestedPayloadSize, retryLevel + 1);
                    }
                    else if (response.errorMsg.Contains("SECTOR_SIZE_IN_BYTES") && response.errorMsg.Contains("4096"))
                    {
                        _log?.Invoke("[Config] 扇区大小不匹配，切换到 4096...");
                        _sectorSize = 4096;
                        return ConfigureWithRetry(storageType, requestedPayloadSize, retryLevel + 1);
                    }
                }
            }
            return false;
        }

        /// <summary>
        /// 应用配置响应
        /// </summary>
        private void ApplyConfigResponse((bool success, bool needsAuth, int maxPayloadSize, int sectorSize, 
            int maxPayloadFrom, int maxXml, string targetName, string version, string memoryName, string errorMsg) response)
        {
            if (response.maxPayloadSize > 0)
            {
                _maxPayloadSize = response.maxPayloadSize;
                if (_highSpeedWriter != null)
                    _highSpeedWriter.MaxPayloadSize = _maxPayloadSize;
            }
            if (response.sectorSize > 0)
            {
                _sectorSize = response.sectorSize;
            }
            if (response.maxPayloadFrom > 0)
            {
                _maxPayloadSizeFromTarget = response.maxPayloadFrom;
            }
            if (response.maxXml > 0)
            {
                _maxXmlSize = response.maxXml;
            }
            if (!string.IsNullOrEmpty(response.targetName))
            {
                TargetName = response.targetName;
                if (!TargetName.StartsWith("MSM")) TargetName = "MSM" + TargetName;
            }
            if (!string.IsNullOrEmpty(response.version))
            {
                Version = response.version;
            }
            if (!string.IsNullOrEmpty(response.memoryName))
            {
                StorageType = response.memoryName.ToLower();
            }

            _log?.Invoke($"[Config] 配置成功");
            _log?.Invoke($"  Target: {TargetName}");
            _log?.Invoke($"  Memory: {StorageType}");
            _log?.Invoke($"  SectorSize: {_sectorSize}");
            _log?.Invoke($"  MaxPayload: {_maxPayloadSize / 1024}KB");

            // 如果支持更高的 payload，尝试启用高速模式
            if (_maxPayloadSize >= 524288) // 512KB+
            {
                _log?.Invoke("[Config] 高速传输模式已激活");
            }
        }

        private (bool success, bool needsAuth, int maxPayloadSize, int sectorSize, 
            int maxPayloadFrom, int maxXml, string targetName, string version, string memoryName, string errorMsg) CheckConfigResponse()
        {
            try
            {
                byte[] buffer = ReadBytes(4096);
                string response = Encoding.UTF8.GetString(buffer);

                int maxPayload = 0, sectorSize = 0, maxPayloadFrom = 0, maxXml = 0;
                string targetName = null, version = null, memoryName = null, errorMsg = null;

                if (response.Contains("\"ACK\"") || response.Contains("value=\"true\""))
                {
                    // 解析 MaxPayloadSizeToTargetInBytes
                    var match = System.Text.RegularExpressions.Regex.Match(response, @"MaxPayloadSizeToTargetInBytes[Supported]*=""(\d+)""");
                    if (match.Success) int.TryParse(match.Groups[1].Value, out maxPayload);

                    // 解析 MaxPayloadSizeToTargetInBytesSupported
                    match = System.Text.RegularExpressions.Regex.Match(response, @"MaxPayloadSizeToTargetInBytesSupported=""(\d+)""");
                    if (match.Success && int.TryParse(match.Groups[1].Value, out int supported))
                    {
                        if (supported > maxPayload) maxPayload = supported;
                    }

                    // 解析 MaxPayloadSizeFromTargetInBytes
                    match = System.Text.RegularExpressions.Regex.Match(response, @"MaxPayloadSizeFromTargetInBytes=""(\d+)""");
                    if (match.Success) int.TryParse(match.Groups[1].Value, out maxPayloadFrom);

                    // 解析 MaxXMLSizeInBytes
                    match = System.Text.RegularExpressions.Regex.Match(response, @"MaxXMLSizeInBytes=""(\d+)""");
                    if (match.Success) int.TryParse(match.Groups[1].Value, out maxXml);

                    // 解析 SectorSizeInBytes
                    match = System.Text.RegularExpressions.Regex.Match(response, @"SectorSizeInBytes=""(\d+)""");
                    if (match.Success) int.TryParse(match.Groups[1].Value, out sectorSize);

                    // 解析 TargetName
                    match = System.Text.RegularExpressions.Regex.Match(response, @"TargetName=""([^""]*)""");
                    if (match.Success) targetName = match.Groups[1].Value;

                    // 解析 Version
                    match = System.Text.RegularExpressions.Regex.Match(response, @"Version=""([^""]*)""");
                    if (match.Success) version = match.Groups[1].Value;

                    // 解析 MemoryName
                    match = System.Text.RegularExpressions.Regex.Match(response, @"MemoryName=""([^""]*)""");
                    if (match.Success) memoryName = match.Groups[1].Value;

                    return (true, false, maxPayload, sectorSize, maxPayloadFrom, maxXml, targetName, version, memoryName, null);
                }

                if (response.Contains("\"NAK\""))
                {
                    bool needsAuth = response.Contains("Authenticate") || response.Contains("Auth") ||
                                    response.Contains("Only nop and sig tag can be");
                    
                    // 提取错误消息
                    var logMatch = System.Text.RegularExpressions.Regex.Match(response, @"<log\s+value=""([^""]*)""\s*/>");
                    if (logMatch.Success) errorMsg = logMatch.Groups[1].Value;

                    return (false, needsAuth, 0, 0, 0, 0, null, null, null, errorMsg);
                }
            }
            catch { }

            return (false, false, 0, 0, 0, 0, null, null, null, null);
        }

        #endregion

        #region ACK 检测方法

        /// <summary>
        /// 快速 ACK 检测 (不输出日志)
        /// </summary>
        public bool IsAckFast()
        {
            return IsAck(silent: true);
        }

        /// <summary>
        /// ACK 检测 (基于 UnlockTool 实现)
        /// </summary>
        public bool IsAck(bool silent = false)
        {
            try
            {
                StringBuilder responseBuffer = new StringBuilder();
                int retries = 0;
                int maxRetries = 50;

                while (retries < maxRetries)
                {
                    byte[] buffer = ReadBytes(4096);
                    if (buffer.Length == 0)
                    {
                        retries++;
                        Thread.Sleep(50);
                        continue;
                    }

                    responseBuffer.Append(Encoding.UTF8.GetString(buffer));
                    string response = responseBuffer.ToString();

                    if (response.Contains("restricted"))
                    {
                        if (response.EndsWith("</data>"))
                        {
                            if (!silent) _log?.Invoke("[ACK] 操作受限");
                            return false;
                        }
                    }

                    if (response.Contains("\"ACK\"") && response.EndsWith("</data>"))
                    {
            return true;
        }

                    if (response.Contains("\"NAK\"") && response.EndsWith("</data>"))
                    {
                        if (!silent)
                        {
                            // 提取错误信息
                            var match = System.Text.RegularExpressions.Regex.Match(response, @"value=""NAK""[^>]*>([^<]*)");
                            if (match.Success && !string.IsNullOrEmpty(match.Groups[1].Value))
                            {
                                _log?.Invoke($"[NAK] {match.Groups[1].Value}");
                            }
                            else
                            {
                                _log?.Invoke("[NAK] 命令被拒绝");
                            }
                        }
                        return false;
                    }

                    // rawmode="true" 也算成功
                    if (response.Contains("rawmode=\"true\"") && response.EndsWith("</data>"))
                    {
                        return true;
                    }
                }
            }
            catch (Exception ex)
            {
                if (!silent) _log?.Invoke($"[ACK] 检测异常: {ex.Message}");
            }

            return false;
        }

        private bool WaitForAck() => IsAck(silent: false);

        #endregion

        #region XML 命令发送

        /// <summary>
        /// 发送 XML 命令
        /// </summary>
        public bool SendXmlCommand(string xml, bool ignoreResponse = false)
        {
            WriteBytes(Encoding.UTF8.GetBytes(xml));
            
            if (ignoreResponse)
            {
                // 快速读取避免缓冲区堆积
                try { ReadBytes(4096); } catch { }
                return true;
            }

            return WaitForAck();
        }

        /// <summary>
        /// 发送 XML 并获取特定属性值 (兼容旧接口名称)
        /// </summary>
        public string SendXmlCommandWithAttributeResponse(string xml, string attributeName, int maxRetries = 50)
        {
            return SendXmlWithAttributeResponse(xml, attributeName, maxRetries);
        }

        /// <summary>
        /// 发送 XML 并获取特定属性值
        /// </summary>
        public string SendXmlWithAttributeResponse(string xml, string attributeName, int maxRetries = 50)
        {
            try
            {
                PurgeBuffer();
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                int currentTry = 0;
                bool hasReceivedAck = false;

                while (currentTry < maxRetries)
                {
                    currentTry++;
                    byte[] buffer = ReadBytes(4096);
                    if (buffer.Length == 0)
                    {
                        Thread.Sleep(hasReceivedAck ? 200 : 50);
                        continue;
                    }

                    string response = Encoding.UTF8.GetString(buffer);

                    // 检查目标属性
                    var match = System.Text.RegularExpressions.Regex.Match(response, $@"{attributeName}=""([^""]*)""");
                    if (match.Success)
                    {
                        string val = match.Groups[1].Value;
                        if (val.Equals("ACK", StringComparison.OrdinalIgnoreCase) ||
                            val.Equals("true", StringComparison.OrdinalIgnoreCase))
                        {
                            hasReceivedAck = true;
                            if ((maxRetries - currentTry) < 10)
                                maxRetries += 5;
                            continue;
                        }
                        return val;
                    }

                    if (response.Contains("value=\"NAK\""))
                        return null;
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] XML 响应异常: {ex.Message}");
            }
            return null;
        }

        #endregion

        #region 核心读写操作

        /// <summary>
        /// 读取分区
        /// </summary>
        public async Task<bool> ReadPartitionAsync(string savePath, string startSector, long numSectors, string lun = "0", 
            Action<long, long> progress = null, CancellationToken ct = default, string label = "dump", 
            string overrideFilename = null, bool append = false, bool suppressError = false)
        {
            long totalBytes = numSectors * _sectorSize;
            string filename = overrideFilename ?? Path.GetFileName(savePath);
            
            _log?.Invoke($"[Read] 读取 {label} @ LUN{lun} Sec {startSector} ({numSectors} sectors)");

            string xml = FirehosePackets.Read(_sectorSize, numSectors, int.Parse(lun), startSector, filename);
            
            PurgeBuffer();
            WriteBytes(Encoding.UTF8.GetBytes(xml));

            if (!await ReceiveRawDataToFileAsync(savePath, totalBytes, progress, ct))
                return false;

            return WaitForAck();
        }

        /// <summary>
        /// 写入分区
        /// </summary>
        public async Task<bool> FlashPartitionAsync(string filePath, string startSector, long numSectors, string lun = "0",
            Action<long, long> progress = null, CancellationToken ct = default, string label = "image", 
            string overrideFilename = null, long fileOffsetBytes = 0)
        {
            if (!File.Exists(filePath)) return false;

            FileInfo fileInfo = new FileInfo(filePath);
            bool isSparse = await IsSparseImageAsync(filePath);
            
            long finalNumSectors = numSectors;
            if (numSectors <= 0)
            {
                if (isSparse)
                {
                    long expandedSize = await GetSparseExpandedSizeAsync(filePath);
                    finalNumSectors = (expandedSize + _sectorSize - 1) / _sectorSize;
                            }
                            else
                            {
                    finalNumSectors = (fileInfo.Length + _sectorSize - 1) / _sectorSize;
                }
            }

            string filename = overrideFilename ?? Path.GetFileName(filePath);
            _log?.Invoke($"[Write] 写入 {label} @ LUN{lun} Sec {startSector} ({finalNumSectors} sectors)");

            string xml = FirehosePackets.Program(_sectorSize, finalNumSectors, int.Parse(lun), startSector, filename);
            
            PurgeBuffer();
            WriteBytes(Encoding.UTF8.GetBytes(xml));

            if (!IsAckFast())
            {
                _log?.Invoke("[Error] 写入握手失败");
                return false;
            }

            // 发送数据
            long totalBytes = finalNumSectors * _sectorSize;
            if (isSparse)
            {
                if (!await SendSparseDataAsync(filePath, progress, ct))
                    return false;
            }
            else
            {
                if (!await SendRawDataAsync(filePath, totalBytes, progress, ct))
                    return false;
            }

            return WaitForAck();
        }

        /// <summary>
        /// 分块读取分区 (用于大分区和特殊伪装需求)
        /// </summary>
        public async Task<bool> ReadPartitionChunkedAsync(string savePath, string startSector, long numSectors, string lun = "0",
            Action<long, long> progress = null, CancellationToken ct = default, string label = "dump", 
            string forceFilename = null, bool append = false, bool suppressError = false)
        {
            long totalBytes = numSectors * _sectorSize;
            long sectorsPerChunk = 8192; // 32MB per chunk (8192 * 4096)
            long currentSector = long.Parse(startSector);
            long remainingSectors = numSectors;
            long totalReadBytes = 0;

            // 创建或清空文件
            if (!append)
            {
                using (var fs = new FileStream(savePath, FileMode.Create, FileAccess.Write, FileShare.None)) { }
            }

            while (remainingSectors > 0)
            {
                if (ct.IsCancellationRequested) return false;

                long chunkSectors = Math.Min(remainingSectors, sectorsPerChunk);
                long chunkBytes = chunkSectors * _sectorSize;
                string filename = forceFilename ?? "gpt_backup0.bin";

                string xml = FirehosePackets.Read(_sectorSize, chunkSectors, int.Parse(lun), currentSector.ToString(), filename);

                PurgeBuffer();
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                // 接收并追加到文件
                if (!await ReceiveRawDataToFileChunkAsync(savePath, chunkBytes, (c, t) =>
                {
                    progress?.Invoke(totalReadBytes + c, totalBytes);
                }, ct, true, suppressError))
                {
                    if (!suppressError) _log?.Invoke($"[Error] 分块读取失败 @ Sector {currentSector}");
                    return false;
                }

                if (!WaitForAck()) return false;

                remainingSectors -= chunkSectors;
                currentSector += chunkSectors;
                totalReadBytes += chunkBytes;
            }
            return true;
        }

        private async Task<bool> ReceiveRawDataToFileChunkAsync(string savePath, long chunkBytes, Action<long, long> progress,
            CancellationToken ct, bool append = true, bool suppressError = false)
        {
            try
            {
                FileMode mode = append ? FileMode.Append : FileMode.Create;
                using (var fs = new FileStream(savePath, mode, FileAccess.Write, FileShare.None, _maxPayloadSize, true))
                {
                    byte[] buffer = new byte[_maxPayloadSize];
                    long received = 0;
                    bool headerFound = false;
                    MemoryStream headerBuffer = new MemoryStream();

                    while (received < chunkBytes)
                    {
                        if (ct.IsCancellationRequested) return false;

                        int requestSize = headerFound ? (int)Math.Min(buffer.Length, chunkBytes - received) : 4096;
                        byte[] readData = ReadBytes(requestSize);

                        if (readData.Length == 0)
                            throw new TimeoutException("读取超时");

                        if (!headerFound)
                        {
                            headerBuffer.Write(readData, 0, readData.Length);
                            string content = Encoding.UTF8.GetString(headerBuffer.ToArray());

                            int ackIndex = content.IndexOf("rawmode=\"true\"", StringComparison.OrdinalIgnoreCase);
                            if (ackIndex >= 0)
                            {
                                int xmlEndIndex = content.IndexOf("</data>", ackIndex);
                                if (xmlEndIndex >= 0)
                                {
                                    int dataStart = xmlEndIndex + 7;
                                    byte[] hdrBytes = headerBuffer.ToArray();
                                    int remaining = hdrBytes.Length - dataStart;
                                    if (remaining > 0)
                                    {
                                        await fs.WriteAsync(hdrBytes, dataStart, remaining, ct);
                                        received += remaining;
                                        progress?.Invoke(received, chunkBytes);
                                        }
                                    headerFound = true;
                                    headerBuffer.Dispose();
                                    continue;
                                }
                            }

                            if (content.Contains("value=\"NAK\"") || content.Contains("Failed to run"))
                            {
                                if (!suppressError) _log?.Invoke($"[Error] 读取被拒绝");
                                return false;
                            }

                            if (headerBuffer.Length > 64 * 1024)
                                throw new Exception("无效的 Firehose 头");

                            continue;
                        }

                        await fs.WriteAsync(readData, 0, readData.Length, ct);
                        received += readData.Length;

                        if (received % (5 * 1024 * 1024) < readData.Length || received >= chunkBytes)
                        {
                            progress?.Invoke(received, chunkBytes);
                        }
                    }
                }
                return true;
            }
            catch (Exception ex)
            {
                if (!suppressError) _log?.Invoke($"[Error] 分块写入失败: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 分块写入分区 (用于超大分区如 Super)
        /// </summary>
        public async Task<bool> FlashPartitionChunkedAsync(string filePath, long startSector, string lun,
            Action<long, long> progress = null, CancellationToken ct = default, string label = "BackupGPT",
            string overrideFilename = null)
        {
            if (!File.Exists(filePath)) return false;
            
            long totalFileSize = new FileInfo(filePath).Length;
            int chunkSizeInSectors = 16384; // 64MB
            long chunkSizeInBytes = chunkSizeInSectors * _sectorSize;
            long currentFileOffset = 0;
            long currentTargetSector = startSector;

            string filename = overrideFilename ?? $"gpt_backup{lun}.bin";
            
            using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                while (currentFileOffset < totalFileSize)
                {
                    if (ct.IsCancellationRequested) return false;

                    long bytesRemaining = totalFileSize - currentFileOffset;
                    long bytesToWrite = Math.Min(bytesRemaining, chunkSizeInBytes);
                    long sectorsToWrite = (bytesToWrite + _sectorSize - 1) / _sectorSize;

                    string xml = FirehosePackets.Program(_sectorSize, sectorsToWrite, int.Parse(lun), 
                        currentTargetSector.ToString(), filename);

                    PurgeBuffer();
                    WriteBytes(Encoding.UTF8.GetBytes(xml));

                    if (!WaitForAck()) return false;

                    // 发送数据块
                    if (!await SendRawDataChunkAsync(fs, currentFileOffset, bytesToWrite, progress, totalFileSize, ct))
                        return false;

                    if (!WaitForAck()) return false;

                    await Task.Delay(200); // 让设备休息一下

                    currentFileOffset += bytesToWrite;
                    currentTargetSector += sectorsToWrite;
                }
            }
            return true;
        }

        private async Task<bool> SendRawDataChunkAsync(FileStream fs, long offset, long length,
            Action<long, long> progress, long totalSize, CancellationToken ct)
        {
            fs.Seek(offset, SeekOrigin.Begin);
            byte[] buffer = new byte[_maxPayloadSize];
            long sent = 0;
            
            while (sent < length)
            {
                int toRead = (int)Math.Min(buffer.Length, length - sent);
                int read = await fs.ReadAsync(buffer, 0, toRead, ct);
                if (read == 0) break;
                
                WriteBytes(buffer.Take(read).ToArray());
                sent += read;
                progress?.Invoke(offset + sent, totalSize);
            }
            
            // 对齐到扇区
            if (sent % _sectorSize != 0)
            {
                int pad = _sectorSize - (int)(sent % _sectorSize);
                WriteBytes(new byte[pad]);
            }
            return true;
        }

        /// <summary>
        /// 擦除分区
        /// </summary>
        public bool ErasePartition(string startSector, long numSectors, string lun = "0")
        {
            _log?.Invoke($"[Erase] 擦除 @ LUN{lun} Sec {startSector} ({numSectors} sectors)");
            string xml = FirehosePackets.Erase(_sectorSize, numSectors, int.Parse(lun), startSector);
            return SendXmlCommand(xml);
        }

        /// <summary>
        /// 应用 Patch
        /// </summary>
        public bool ApplyPatch(string sector, long byteOffset, string value, int sizeInBytes, string lun = "0")
        {
            _log?.Invoke($"[Patch] LUN{lun} Sec:{sector} Off:{byteOffset} Val:{value}");
            string xml = FirehosePackets.Patch(_sectorSize, byteOffset, int.Parse(lun), sizeInBytes, sector, value);
            return SendXmlCommand(xml);
        }

        /// <summary>
        /// 应用 Patch (从 XML 内容解析)
        /// </summary>
        public bool ApplyPatch(string xmlContent)
        {
            try
            {
                string xmlToParse = xmlContent.Trim();

                // 移除 XML 声明
                if (xmlToParse.StartsWith("<?xml"))
                {
                    int endDecl = xmlToParse.IndexOf("?>");
                    if (endDecl > 0)
                        xmlToParse = xmlToParse.Substring(endDecl + 2).Trim();
                }

                // 包裹根节点
                if (!xmlToParse.StartsWith("<root>") && !xmlToParse.StartsWith("<data>") && !xmlToParse.StartsWith("<patches>"))
                {
                    xmlToParse = $"<patches>{xmlToParse}</patches>";
                }

                var doc = System.Xml.Linq.XDocument.Parse(xmlToParse);
                bool allSuccess = true;

                foreach (var patch in doc.Descendants("patch"))
                {
                    string sector = patch.Attribute("start_sector")?.Value ?? "0";
                    long byteOffset = long.Parse(patch.Attribute("byte_offset")?.Value ?? "0");
                    string value = patch.Attribute("value")?.Value ?? "";
                    int size = int.Parse(patch.Attribute("size_in_bytes")?.Value ?? "4");
                    string lun = patch.Attribute("physical_partition_number")?.Value ?? "0";

                    if (!ApplyPatch(sector, byteOffset, value, size, lun))
                    {
                        allSuccess = false;
                        _log?.Invoke($"[Error] Patch 失败 @ sector {sector}");
                    }
                }
                return allSuccess;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] 解析 Patch XML 失败: {ex.Message}");
                return false;
            }
        }

        #endregion

        #region 数据传输辅助方法

        private async Task<bool> SendRawDataAsync(string filePath, long totalBytes, Action<long, long> progress, CancellationToken ct)
        {
            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read, _maxPayloadSize, true))
                {
                    byte[] buffer = new byte[_maxPayloadSize];
                    long totalSent = 0;
                    long lastReported = 0;

                    while (totalSent < totalBytes)
                    {
                        if (ct.IsCancellationRequested) return false;

                        long remaining = totalBytes - totalSent;
                        int toRead = (int)Math.Min(buffer.Length, remaining);
                        int read = await fs.ReadAsync(buffer, 0, toRead, ct);

                        if (read == 0)
                        {
                            // 补零对齐
                            if (totalSent < totalBytes)
                            {
                                int padding = (int)(totalBytes - totalSent);
                                Array.Clear(buffer, 0, padding);
                                WriteBytes(buffer.Take(padding).ToArray());
                                totalSent = totalBytes;
                            }
                            break;
                        }

                        // 最后一块需要补零对齐到扇区边界
                        if (read < toRead)
                        {
                            Array.Clear(buffer, read, toRead - read);
                            read = toRead;
                        }

                        WriteBytes(buffer.Take(read).ToArray());
                        totalSent += read;

                        if ((totalSent - lastReported) >= 5 * 1024 * 1024 || totalSent >= totalBytes)
                        {
                            progress?.Invoke(totalSent, totalBytes);
                            lastReported = totalSent;
                        }
                    }
                }
                return true;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] 数据发送失败: {ex.Message}");
                return false;
            }
        }

        private async Task<bool> SendSparseDataAsync(string filePath, Action<long, long> progress, CancellationToken ct)
        {
            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    // 读取 Sparse 头
                    uint magic = br.ReadUInt32();
                    if (magic != SPARSE_HEADER_MAGIC)
                        throw new Exception("不是有效的 Sparse 镜像");

                    br.ReadUInt16(); // major version
                    br.ReadUInt16(); // minor version
                    ushort fileHdrSize = br.ReadUInt16();
                    ushort chunkHdrSize = br.ReadUInt16();
                    uint blockSize = br.ReadUInt32();
                    uint totalBlocks = br.ReadUInt32();
                    uint totalChunks = br.ReadUInt32();
                    br.ReadUInt32(); // checksum

                    fs.Seek(fileHdrSize, SeekOrigin.Begin);

                    long totalExpandedBytes = (long)totalBlocks * blockSize;
                    long currentExpanded = 0;
                    byte[] transferBuffer = new byte[_maxPayloadSize];

                    for (int i = 0; i < totalChunks; i++)
                    {
                        if (ct.IsCancellationRequested) return false;

                        ushort chunkType = br.ReadUInt16();
                        br.ReadUInt16(); // reserved
                        uint chunkBlocks = br.ReadUInt32();
                        uint totalSize = br.ReadUInt32();

                        long chunkDataBytes = (long)chunkBlocks * blockSize;

                        switch (chunkType)
                        {
                            case CHUNK_TYPE_RAW:
                                long bytesToRead = totalSize - chunkHdrSize;
                                long bytesSent = 0;
                                while (bytesSent < bytesToRead)
                                {
                                    int toRead = (int)Math.Min(transferBuffer.Length, bytesToRead - bytesSent);
                                    int read = fs.Read(transferBuffer, 0, toRead);
                                    if (read == 0) break;
                                    WriteBytes(transferBuffer.Take(read).ToArray());
                                    bytesSent += read;
                                    currentExpanded += read;
                                    progress?.Invoke(currentExpanded, totalExpandedBytes);
                                }
                                break;

                            case CHUNK_TYPE_FILL:
                                uint fillValue = br.ReadUInt32();
                                byte[] fillBytes = BitConverter.GetBytes(fillValue);
                                for (int k = 0; k < transferBuffer.Length; k += 4)
                                    Array.Copy(fillBytes, 0, transferBuffer, k, Math.Min(4, transferBuffer.Length - k));

                                long remainingFill = chunkDataBytes;
                                while (remainingFill > 0)
                                {
                                    int toWrite = (int)Math.Min(remainingFill, transferBuffer.Length);
                                    WriteBytes(transferBuffer.Take(toWrite).ToArray());
                                    remainingFill -= toWrite;
                                    currentExpanded += toWrite;
                                    progress?.Invoke(currentExpanded, totalExpandedBytes);
                                }
                                break;

                            case CHUNK_TYPE_DONT_CARE:
                                Array.Clear(transferBuffer, 0, transferBuffer.Length);
                                long remainingSkip = chunkDataBytes;
                                while (remainingSkip > 0)
                                {
                                    int toWrite = (int)Math.Min(remainingSkip, transferBuffer.Length);
                                    WriteBytes(transferBuffer.Take(toWrite).ToArray());
                                    remainingSkip -= toWrite;
                                    currentExpanded += toWrite;
                                    progress?.Invoke(currentExpanded, totalExpandedBytes);
                                }
                                break;

                            case CHUNK_TYPE_CRC32:
                                br.ReadUInt32(); // Skip CRC
                                break;
                        }
                    }

                    // 对齐
                    if (currentExpanded % _sectorSize != 0)
                    {
                        int pad = _sectorSize - (int)(currentExpanded % _sectorSize);
                        WriteBytes(new byte[pad]);
                    }
                }
                return true;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] Sparse 发送失败: {ex.Message}");
                return false;
            }
        }

        private async Task<bool> ReceiveRawDataToFileAsync(string savePath, long totalBytes, Action<long, long> progress, CancellationToken ct)
            {
                try
                {
                using (var fs = new FileStream(savePath, FileMode.Create, FileAccess.Write, FileShare.None, _maxPayloadSize, true))
                {
                    byte[] buffer = new byte[_maxPayloadSize];
                    long received = 0;
                    bool headerFound = false;
                    MemoryStream headerBuffer = new MemoryStream();

                    while (received < totalBytes)
                    {
                        if (ct.IsCancellationRequested) return false;

                        int requestSize = headerFound ? (int)Math.Min(buffer.Length, totalBytes - received) : 4096;
                        byte[] readData = ReadBytes(requestSize);
                        
                        if (readData.Length == 0)
                            throw new TimeoutException("读取超时");

                        if (!headerFound)
                        {
                            headerBuffer.Write(readData, 0, readData.Length);
                            string content = Encoding.UTF8.GetString(headerBuffer.ToArray());
                            
                            int ackIndex = content.IndexOf("rawmode=\"true\"", StringComparison.OrdinalIgnoreCase);
                            if (ackIndex >= 0)
                            {
                                int xmlEndIndex = content.IndexOf("</data>", ackIndex);
                                if (xmlEndIndex >= 0)
                                {
                                    int dataStart = xmlEndIndex + 7;
                                    byte[] hdrBytes = headerBuffer.ToArray();
                                    int remaining = hdrBytes.Length - dataStart;
                                    if (remaining > 0)
                                    {
                                        await fs.WriteAsync(hdrBytes, dataStart, remaining, ct);
                                        received += remaining;
                                        progress?.Invoke(received, totalBytes);
                                    }
                                    headerFound = true;
                                    headerBuffer.Dispose();
                                    continue;
                                }
                            }

                            if (content.Contains("value=\"NAK\""))
                            {
                                _log?.Invoke("[Error] 读取被拒绝");
                                return false;
                            }

                            if (headerBuffer.Length > 64 * 1024)
                                throw new Exception("无效的 Firehose 头");

                                continue; 
                        }

                        await fs.WriteAsync(readData, 0, readData.Length, ct);
                        received += readData.Length;

                        if (received % (5 * 1024 * 1024) < readData.Length || received >= totalBytes)
                        {
                            progress?.Invoke(received, totalBytes);
                        }
                        }
                    }
                    return true;
                }
                catch (Exception ex)
                {
                _log?.Invoke($"[Error] 文件写入失败: {ex.Message}");
                    return false;
                }
        }

        #endregion

        #region 辅助功能

        /// <summary>
        /// 获取存储信息
        /// </summary>
        public string GetStorageInfo(int lun = 0)
        {
            string xml = FirehosePackets.GetStorageInfo(lun);
            WriteBytes(Encoding.UTF8.GetBytes(xml));

            StringBuilder result = new StringBuilder();
            int maxRetries = 50;

            while (maxRetries-- > 0)
            {
                byte[] buffer = ReadBytes(4096);
                if (buffer.Length == 0) continue;

                string response = Encoding.UTF8.GetString(buffer);
                
                // 提取 log 消息
                var logMatches = System.Text.RegularExpressions.Regex.Matches(response, @"<log value=""([^""]*)""\s*/>");
                foreach (System.Text.RegularExpressions.Match match in logMatches)
                {
                    result.AppendLine(match.Groups[1].Value);
                }

                if (response.Contains("\"ACK\"") || response.Contains("\"NAK\""))
                    break;
            }

            return result.ToString();
        }

        /// <summary>
        /// 电源控制
        /// </summary>
        public bool Power(string mode)
        {
            _log?.Invoke($"[Power] {mode}...");
            return SendXmlCommand(FirehosePackets.Power(mode), true);
        }

        /// <summary>
        /// 电源控制 (兼容旧接口名称)
        /// </summary>
        public bool PowerCommand(string mode)
        {
            return Power(mode);
        }

        /// <summary>
        /// Ping (NOP)
        /// </summary>
        public bool Ping()
        {
            return SendXmlCommand(FirehosePackets.Nop());
        }

        /// <summary>
        /// 设置启动分区
        /// </summary>
        public bool SetBootableStorageDrive(int value)
        {
            _log?.Invoke($"[Config] 设置启动分区: {value}");
            return SendXmlCommand(FirehosePackets.SetBootableStorageDrive(value));
        }

        /// <summary>
        /// 设置启动 LUN (兼容旧接口名称)
        /// </summary>
        public bool SetBootLun(int lun)
        {
            return SetBootableStorageDrive(lun);
        }

        /// <summary>
        /// 设置传输窗口大小 (优化传输速度)
        /// </summary>
        public bool SetTransferWindow(int size)
        {
            string xml = FirehosePackets.Configure(StorageType, size);
            if (SendXmlCommand(xml, true))
            {
                _maxPayloadSize = size;
                _log?.Invoke($"[Config] 传输窗口设置为: {size / 1024}KB");
                return true;
            }
            return false;
        }

        /// <summary>
        /// 获取 SHA256 哈希
        /// </summary>
        public string GetSha256(int lun, long startSector, long numSectors)
        {
            try
            {
                string xml = FirehosePackets.GetSha256Digest(_sectorSize, numSectors, lun, startSector.ToString());
                
                PurgeBuffer();
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                int maxRetries = 50;
                while (maxRetries-- > 0)
                {
                    byte[] buffer = ReadBytes(4096);
                    if (buffer.Length == 0) continue;

                    string response = Encoding.UTF8.GetString(buffer);
                    
                    var match = System.Text.RegularExpressions.Regex.Match(response, @"Digest=""([^""]*)""");
                    if (match.Success)
                        return match.Groups[1].Value;

                    if (response.Contains("\"NAK\""))
                    {
                        _log?.Invoke("[SHA256] 设备不支持");
                        return null;
                    }
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[SHA256] 获取失败: {ex.Message}");
            }
            return null;
        }

        /// <summary>
        /// 内存转储 (Memory Dump)
        /// </summary>
        public async Task<bool> DumpMemoryAsync(string savePath, ulong startAddress, ulong size, 
            Action<long, long> progress = null, CancellationToken ct = default)
        {
            try
            {
                _log?.Invoke($"[Dump] 转储内存 0x{startAddress:X} - 0x{startAddress + size:X}");

                const int chunkSize = 1024 * 1024; // 1MB
                ulong remaining = size;
                ulong currentAddr = startAddress;
                long totalWritten = 0;

                using (var fs = new FileStream(savePath, FileMode.Create, FileAccess.Write))
                {
                    while (remaining > 0 && !ct.IsCancellationRequested)
                    {
                        int toRead = (int)Math.Min(remaining, chunkSize);
                        byte[] chunk = PeekMemory(currentAddr, toRead);

                        if (chunk == null)
                        {
                            _log?.Invoke($"[Dump] 读取失败 @ 0x{currentAddr:X}");
                            return false;
                        }

                        await fs.WriteAsync(chunk, 0, chunk.Length, ct);
                        totalWritten += chunk.Length;
                        remaining -= (ulong)chunk.Length;
                        currentAddr += (ulong)chunk.Length;

                        progress?.Invoke(totalWritten, (long)size);
                    }
                }

                _log?.Invoke($"[Dump] 完成，保存到: {savePath}");
                return true;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Dump] 转储失败: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 备份 GPT 分区表
        /// </summary>
        public async Task<bool> BackupGptAsync(string savePath, int lun = 0, CancellationToken ct = default)
        {
            try
            {
                _log?.Invoke($"[GPT] 备份 LUN{lun} 分区表...");
                int sectorsToRead = (_sectorSize == 4096) ? 6 : 34;
                
                string xml = FirehosePackets.Read(_sectorSize, sectorsToRead, lun, "0", $"gpt_backup{lun}.bin");
                
                PurgeBuffer();
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                long totalBytes = sectorsToRead * _sectorSize;
                if (!await ReceiveRawDataToFileAsync(savePath, totalBytes, null, ct))
                {
                    _log?.Invoke("[GPT] 备份失败");
                    return false;
                }

                if (!WaitForAck())
                {
                    _log?.Invoke("[GPT] 备份确认失败");
                    return false;
                }

                _log?.Invoke($"[GPT] 备份成功: {savePath}");
                return true;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[GPT] 备份异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 恢复 GPT 分区表
        /// </summary>
        public async Task<bool> RestoreGptAsync(string gptPath, int lun = 0, CancellationToken ct = default)
        {
            try
            {
                if (!File.Exists(gptPath))
                {
                    _log?.Invoke($"[GPT] 文件不存在: {gptPath}");
                    return false;
                }

                _log?.Invoke($"[GPT] 恢复 LUN{lun} 分区表...");
                long fileSize = new FileInfo(gptPath).Length;
                long sectorsToWrite = fileSize / _sectorSize;

                string xml = FirehosePackets.Program(_sectorSize, sectorsToWrite, lun, "0", "gpt_restore.bin");

                PurgeBuffer();
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                if (!WaitForAck())
                {
                    _log?.Invoke("[GPT] 恢复握手失败");
                    return false;
                }

                if (!await SendRawDataAsync(gptPath, sectorsToWrite * _sectorSize, null, ct))
                {
                    _log?.Invoke("[GPT] 恢复数据发送失败");
                    return false;
                }

                if (!WaitForAck())
                {
                    _log?.Invoke("[GPT] 恢复确认失败");
                    return false;
                }

                _log?.Invoke("[GPT] 恢复成功");
                return true;
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[GPT] 恢复异常: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 内存读取 (Peek)
        /// </summary>
        public byte[] PeekMemory(ulong address, int size)
        {
            _log?.Invoke($"[Peek] 读取内存 @ 0x{address:X} ({size} bytes)");
                
            string xml = FirehosePackets.Peek(address, size);
                PurgeBuffer();
            WriteBytes(Encoding.UTF8.GetBytes(xml));

                byte[] result = new byte[size];
                int totalRead = 0;

                    while (totalRead < size)
                    {
                byte[] buffer = ReadBytes(size - totalRead);
                if (buffer.Length == 0) break;
                Array.Copy(buffer, 0, result, totalRead, buffer.Length);
                totalRead += buffer.Length;
                }

                if (totalRead < size)
                {
                _log?.Invoke($"[Peek] 只读取了 {totalRead}/{size} 字节");
                    return null;
                }

            if (!WaitForAck()) return null;
                return result;
        }

        /// <summary>
        /// 内存写入 (Poke)
        /// </summary>
        public bool PokeMemory(ulong address, byte[] data)
        {
            _log?.Invoke($"[Poke] 写入内存 @ 0x{address:X} ({data.Length} bytes)");
            string xml = FirehosePackets.Poke(address, data.Length, BitConverter.ToString(data).Replace("-", ""));
                return SendXmlCommand(xml, true);
        }

        /// <summary>
        /// 获取设备信息
        /// </summary>
        public Dictionary<string, string> GetDeviceInfo()
        {
            var info = new Dictionary<string, string>();

            string xml = FirehosePackets.GetDeviceInfo();
            PurgeBuffer();
            WriteBytes(Encoding.UTF8.GetBytes(xml));

            int maxRetries = 50;
            while (maxRetries-- > 0)
            {
                byte[] buffer = ReadBytes(4096);
                if (buffer.Length == 0) continue;

                string response = Encoding.UTF8.GetString(buffer);

                // 解析 log 中的信息
                var logMatches = System.Text.RegularExpressions.Regex.Matches(response, @"<log value=""([^""]*)""\s*/>");
                foreach (System.Text.RegularExpressions.Match match in logMatches)
                {
                    string log = match.Groups[1].Value;
                    if (log.Contains(":"))
                    {
                        var parts = log.Split(new[] { ':' }, 2);
                        if (parts.Length == 2)
                        {
                            info[parts[0].Trim()] = parts[1].Trim();
                        }
                    }
                }

                if (response.Contains("\"ACK\"") || response.Contains("\"NAK\""))
                    break;
            }

            return info;
        }

        /// <summary>
        /// 重置设备
        /// </summary>
        public bool Reset(string mode = "reset")
        {
            return Power(mode);
        }

        public void SetSectorSize(int size) => _sectorSize = size;

        private async Task<bool> IsSparseImageAsync(string filePath)
        {
            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read))
                {
                    if (fs.Length < 4) return false;
                    byte[] magic = new byte[4];
                    await fs.ReadAsync(magic, 0, 4);
                    return BitConverter.ToUInt32(magic, 0) == SPARSE_HEADER_MAGIC;
                }
            }
            catch { return false; }
        }

        private async Task<long> GetSparseExpandedSizeAsync(string filePath)
        {
            try
            {
                using (var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    if (fs.Length < 28) return 0;
                    fs.Seek(12, SeekOrigin.Begin);
                    uint blockSize = br.ReadUInt32();
                    uint totalBlocks = br.ReadUInt32();
                    return (long)totalBlocks * blockSize;
                }
            }
            catch { return 0; }
        }

        #endregion

        #region VIP 认证

        /// <summary>
        /// VIP 认证流程
        /// </summary>
        public bool PerformVipAuth(string digestPath, string signaturePath, Action<long, long> progress = null)
        {
            if (!File.Exists(digestPath) || !File.Exists(signaturePath))
            {
                _log?.Invoke("[VIP] 错误: 缺少验证文件");
                return false;
            }

            _log?.Invoke("[VIP] 开始安全验证...");
            PurgeBuffer();
            Thread.Sleep(100);
            PurgeBuffer();

            _log?.Invoke("[VIP] 发送 Digest...");
            byte[] digestData = File.ReadAllBytes(digestPath);
            WriteBytes(digestData);
            progress?.Invoke(digestData.Length, digestData.Length);
            Thread.Sleep(200);

            _log?.Invoke("[VIP] 发送 Verify 指令...");
            SendXmlCommand(FirehosePackets.Verify(), true);
            Thread.Sleep(200);

            _log?.Invoke("[VIP] 发送 Signature...");
            byte[] sigData = File.ReadAllBytes(signaturePath);
            WriteBytes(sigData);
            progress?.Invoke(sigData.Length, sigData.Length);
            Thread.Sleep(200);

            _log?.Invoke("[VIP] 发送 SHA256Init...");
            SendXmlCommand(FirehosePackets.Sha256Init(), true);

            PurgeBuffer();
            _log?.Invoke("[VIP] 验证流程结束");
            return true;
        }

        /// <summary>
        /// 签名认证
        /// </summary>
        public bool SendSignature(byte[] signatureData)
        {
            try
            {
                string xml = FirehosePackets.Signature(signatureData.Length);
                if (!SendXmlCommand(xml)) return false;

                WriteBytes(signatureData);
                return WaitForAck();
            }
            catch { return false; }
        }

        #endregion

        #region GPT 操作

        /// <summary>
        /// 读取 GPT 包 (用于分区表读取)
        /// </summary>
        public async Task<byte[]> ReadGptPacketAsync(string lun, long startSector, int numSectors, string label, 
            string filename, CancellationToken ct = default)
        {
            try
            {
                string xml = FirehosePackets.Read(_sectorSize, numSectors, int.Parse(lun), startSector.ToString(), filename);

                PurgeBuffer();
                _log?.Invoke($"[Read] 读取 GPT LUN{lun} ({label})...");
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                byte[] buffer = new byte[numSectors * _sectorSize];
                if (await ReceiveRawDataToMemoryAsync(buffer, ct))
                {
                    if (WaitForAck())
                        return buffer;
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] GPT 读取异常: {ex.Message}");
            }
            return null;
        }

        /// <summary>
        /// 读取 Backup GPT (使用 NUM_DISK_SECTORS 语法)
        /// </summary>
        public async Task<byte[]> ReadBackupGptAsync(string lun = "0", int numSectors = 6, CancellationToken ct = default)
        {
            PurgeBuffer();
            string start = $"NUM_DISK_SECTORS-{numSectors}.";
            string filename = "ssd";
            string label = "ssd";

            string xml = FirehosePackets.Read(_sectorSize, numSectors, int.Parse(lun), start, filename);

            _log?.Invoke($"[Read] 读取 Backup GPT LUN{lun} (Start: {start})...");
            WriteBytes(Encoding.UTF8.GetBytes(xml));

            byte[] buffer = new byte[numSectors * _sectorSize];
            if (!await ReceiveRawDataToMemoryAsync(buffer, ct)) return null;
            if (!WaitForAck()) return null;
            return buffer;
        }

        /// <summary>
        /// 读取 GPT (兼容旧接口)
        /// </summary>
        public async Task<byte[]> ReadGptAsync(string lun, long startSector, int numSectors, string label, string filename, CancellationToken ct = default)
        {
            try
            {
                string xml = FirehosePackets.Read(_sectorSize, numSectors, int.Parse(lun), startSector.ToString(), filename);
                
                PurgeBuffer();
                _log?.Invoke($"[Read] 读取 GPT LUN{lun} ({label})...");
                WriteBytes(Encoding.UTF8.GetBytes(xml));

                byte[] buffer = new byte[numSectors * _sectorSize];
                if (await ReceiveRawDataToMemoryAsync(buffer, ct))
                {
                    if (WaitForAck())
                        return buffer;
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"[Error] GPT 读取异常: {ex.Message}");
            }
            return null;
        }

        private async Task<bool> ReceiveRawDataToMemoryAsync(byte[] buffer, CancellationToken ct)
        {
            return await Task.Run(() =>
            {
                try
                {
                    int totalBytes = buffer.Length;
                    int received = 0;
                    bool headerFound = false;
                    byte[] tempBuf = new byte[65536];

                    while (received < totalBytes)
                    {
                        if (ct.IsCancellationRequested) return false;

                        int requestSize = headerFound ? Math.Min(tempBuf.Length, totalBytes - received) : 4096;
                        byte[] readData = ReadBytes(requestSize);
                        if (readData.Length == 0) throw new TimeoutException("Read Timeout");

                        int dataStartOffset = 0;

                        if (!headerFound)
                        {
                            string content = Encoding.UTF8.GetString(readData);

                            int ackIndex = content.IndexOf("rawmode=\"true\"", StringComparison.OrdinalIgnoreCase);
                            if (ackIndex >= 0)
                            {
                                int xmlEndIndex = content.IndexOf("</data>", ackIndex);
                                if (xmlEndIndex >= 0)
                                {
                                    headerFound = true;
                                    dataStartOffset = xmlEndIndex + 7;
                                    if (dataStartOffset >= readData.Length) continue;
                                }
                            }
                            else if (content.Contains("value=\"NAK\""))
                            {
                                _log?.Invoke("[Error] 读取被拒绝");
                                return false;
                            }
                            else
                            {
                                continue;
                            }
                        }

                        int dataLength = readData.Length - dataStartOffset;
                        if (dataLength > 0)
                        {
                            if (received + dataLength > buffer.Length)
                                dataLength = buffer.Length - received;
                            Array.Copy(readData, dataStartOffset, buffer, received, dataLength);
                            received += dataLength;
                        }
                    }
                    return true;
            }
            catch (Exception ex)
            {
                    _log?.Invoke($"[Error] 数据接收异常: {ex.Message}");
                    return false;
                }
            }, ct);
        }

        #endregion

        public void Dispose()
        {
            _highSpeedWriter?.Dispose();
            if (_serialPort != null && _serialPort.IsOpen)
            {
                _serialPort.Close();
            }
        }
    }
}
