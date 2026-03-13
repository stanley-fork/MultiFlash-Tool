#if EXCLUDE_REALME_AUTH
// 占位类型：提交 GitHub 时排除 Realme 认证实现，仅保留类型签名以便编译通过。
using System;

namespace SakuraEDL.Qualcomm.Services
{
    public sealed class QualcommRealmeAuthOptions
    {
        public string ProjectNumber { get; set; }
        public string FirmwarePath { get; set; }
        public string NvPlatform { get; set; }
        public string NvCode { get; set; }
        public string ApiUrl { get; set; }
        public string Token { get; set; }
        public string ApiKey { get; set; }
        public string Account { get; set; }
        public bool UseModernProtocol { get; set; }
        public string CloudLoaderFile { get; set; }
        public string CloudDigestFile { get; set; }
        public string CloudDeviceDisplayName { get; set; }
        public string RcsmAuthAccount { get; set; }
        public string RcsmAuthKey { get; set; }

        public QualcommRealmeAuthOptions Clone()
        {
            return new QualcommRealmeAuthOptions
            {
                ProjectNumber = ProjectNumber,
                FirmwarePath = FirmwarePath,
                NvPlatform = NvPlatform,
                NvCode = NvCode,
                ApiUrl = ApiUrl,
                Token = Token,
                ApiKey = ApiKey,
                Account = Account,
                UseModernProtocol = UseModernProtocol,
                CloudLoaderFile = CloudLoaderFile,
                CloudDigestFile = CloudDigestFile,
                CloudDeviceDisplayName = CloudDeviceDisplayName,
                RcsmAuthAccount = RcsmAuthAccount,
                RcsmAuthKey = RcsmAuthKey,
            };
        }
    }

    public class RealmeCloudLoaderInfo
    {
        public int Id { get; set; }
        public string DisplayName { get; set; }
        public string Chip { get; set; }
        public string StorageType { get; set; }
        public string ProjectId { get; set; }
        public bool HasDigest { get; set; }
    }

    public class RealmeCloudLoaderService
    {
        private static readonly RealmeCloudLoaderService _instance = new RealmeCloudLoaderService();
        public static RealmeCloudLoaderService Instance => _instance;
        public System.Threading.Tasks.Task<System.Collections.Generic.List<RealmeCloudLoaderInfo>> GetRealmeLoadersAsync()
            => System.Threading.Tasks.Task.FromResult(new System.Collections.Generic.List<RealmeCloudLoaderInfo>());
        public System.Threading.Tasks.Task<string> DownloadLoaderToTempAsync(int id, System.Action<long, long> progress = null)
            => System.Threading.Tasks.Task.FromResult<string>(null);
        public System.Threading.Tasks.Task<string> DownloadDigestToTempAsync(int id, System.Action<long, long> progress = null)
            => System.Threading.Tasks.Task.FromResult<string>(null);
    }
}
#endif
