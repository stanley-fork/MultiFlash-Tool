#include "edl/oneplus_auth.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

/* ===== 设备配置数据库 ===== */

typedef struct { const char *proj; int version; const char *cm; int pm; } op_config_t;

static const op_config_t op_configs[] = {
    /* OP5-7T (V1) */
    {"16859",1,NULL,0},{"17801",1,NULL,0},{"17819",1,NULL,0},
    {"18801",1,NULL,0},{"18811",1,NULL,0},{"18857",1,NULL,0},
    {"18821",1,NULL,0},{"18825",1,NULL,0},{"18827",1,NULL,0},
    {"18831",1,NULL,0},{"18865",1,NULL,0},{"19801",1,NULL,0},
    {"19861",1,NULL,0},{"19863",1,NULL,0},
    /* OP8 (V2) */
    {"19821",2,"0cffee8a",0},{"19855",2,"6d9215b4",0},
    {"19867",2,"4107b2d4",0},{"19868",2,"178d8213",0},
    {"19811",2,"40217c07",0},{"19805",2,"1a5ec176",0},
    {"20809",2,"d6bc8c36",0},
    /* Nord (V2) */
    {"20801",2,"eacf50e7",0},{"20813",2,"48ad7b61",0},
    /* OP9 (V2) */
    {"19815",2,"9c151c7f",0},{"20859",2,"9c151c7f",0},
    {"20857",2,"9c151c7f",0},{"19825",2,"0898dcd6",0},
    {"20851",2,"0898dcd6",0},{"20852",2,"0898dcd6",0},
    {"20853",2,"0898dcd6",0},{"20828",2,"f498b60f",0},
    {"20854",2,"16225d4e",0},
    /* Dre (V1) */
    {"20818",1,NULL,0},{"2083C",1,NULL,0},{"2083D",1,NULL,0},
    /* N10/N100 (V3) */
    {"20885",3,"3a403a71",1},{"20886",3,"b8bd9e39",1},
    {"20888",3,"142f1bd7",1},{"20889",3,"f2056ae1",1},
    {"20880",3,"6ccf5913",1},{"20881",3,"fa9ff378",1},
    {"20882",3,"4ca1e84e",1},{"20883",3,"ad9dba4a",1},
};

#define OP_CONFIG_COUNT (sizeof(op_configs)/sizeof(op_configs[0]))

/* ===== AES 密钥材料 ===== */

static const uint8_t AES_KEY_PRE_DEMACIA[] = {0x01,0x63,0xA0,0xD1,0xFD,0xE2,0x67,0x11};
static const uint8_t AES_KEY_SUF_DEMACIA[] = {0x48,0x27,0xC2,0x08,0xFB,0xB0,0xE6,0xF0};
static const uint8_t AES_IV_DEMACIA[]      = {0x96,0xE0,0x79,0x0C,0xAE,0x2B,0xB4,0xAF,
                                               0x68,0x4C,0x36,0xCB,0x0B,0xEC,0x49,0xCE};

static const uint8_t AES_KEY_PRE_V1[]  = {0x10,0x45,0x63,0x87,0xE3,0x7E,0x23,0x71};
static const uint8_t AES_KEY_SUF_V1[]  = {0xA2,0xD4,0xA0,0x74,0x0F,0xD3,0x28,0x96};
static const uint8_t AES_IV_V1[]       = {0x9D,0x61,0x4A,0x1E,0xAC,0x81,0xC9,0xB2,
                                           0xD3,0x76,0xD7,0x49,0x31,0x03,0x63,0x79};

static const uint8_t AES_KEY_PRE_V3[]  = {0x46,0xA5,0x97,0x30,0xBB,0x0D,0x41,0xE8};
static const uint8_t AES_IV_V3[]       = {0xDC,0x91,0x0D,0x88,0xE3,0xC6,0xEE,0x65,
                                           0xF0,0xC7,0x44,0xB4,0x02,0x30,0xCE,0x40};

static const char *PROD_KEY_OLD = "b2fad511325185e5";
static const char *PROD_KEY_NEW = "7016147d58e8c038";
static const char *RANDOM_V1   = "8MwDdWXZO7sj0PF3";
static const char *RANDOM_V3   = "c75oVnz8yUgLZObh";
static const char *VERSION_V1  = "guacamoles_21_O.22_191107";
static const char *VERSION_V3  = "billie8_14_E.01_201028";

/* ===== Windows BCrypt Helpers ===== */

#ifdef _WIN32

static bool compute_sha256(const uint8_t *data, int len, uint8_t *hash)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
            BCryptHashData(hh, (PUCHAR)data, len, 0);
            ok = (BCryptFinishHash(hh, hash, 32, 0) == 0);
            BCryptDestroyHash(hh);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

static bool aes_cbc_encrypt(const uint8_t *key, int key_len,
                             const uint8_t *iv,
                             const uint8_t *plain, int plain_len,
                             uint8_t *cipher)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE kh = NULL;
    bool ok = false;
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) == 0) {
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                          (ULONG)(wcslen(BCRYPT_CHAIN_MODE_CBC)+1)*sizeof(wchar_t), 0);
        if (BCryptGenerateSymmetricKey(alg, &kh, NULL, 0, (PUCHAR)key, key_len, 0) == 0) {
            ULONG out_len = 0;
            ok = (BCryptEncrypt(kh, (PUCHAR)plain, plain_len, NULL, iv_copy, 16,
                                cipher, plain_len, &out_len, 0) == 0);
            BCryptDestroyKey(kh);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

#endif /* _WIN32 */

static void bytes_to_hex_upper(const uint8_t *data, int len, char *out)
{
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02X", data[i]);
}

static void bytes_to_hex_lower(const uint8_t *data, int len, char *out)
{
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", data[i]);
}

/* ===== Public API ===== */

int edl_oneplus_build_getprjversion(char *buf, int buf_size)
{
    return snprintf(buf, buf_size, "<?xml version=\"1.0\" ?><data><getprjversion /></data>");
}

int edl_oneplus_build_setprocstart(char *buf, int buf_size)
{
    return snprintf(buf, buf_size, "<?xml version=\"1.0\" ?><data><setprocstart /></data>");
}

edl_oneplus_config_t edl_oneplus_lookup_config(const char *proj_id)
{
    edl_oneplus_config_t result;
    memset(&result, 0, sizeof(result));
    result.version = 1;
    result.found = false;

    if (!proj_id) return result;
    for (int i = 0; i < (int)OP_CONFIG_COUNT; i++) {
        if (strcmp(op_configs[i].proj, proj_id) == 0) {
            result.version = op_configs[i].version;
            if (op_configs[i].cm)
                snprintf(result.cm, sizeof(result.cm), "%s", op_configs[i].cm);
            result.param_mode = op_configs[i].pm;
            result.found = true;
            return result;
        }
    }
    return result;
}

void edl_oneplus_generate_pk(char *pk, int size)
{
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    srand((unsigned)time(NULL));
    int len = (size - 1 < 16) ? size - 1 : 16;
    for (int i = 0; i < len; i++)
        pk[i] = chars[rand() % (sizeof(chars) - 1)];
    pk[len] = '\0';
}

bool edl_oneplus_generate_demacia(const char *serial, const char *pk,
                                   char *demacia_hex, int hex_size)
{
#ifdef _WIN32
    if (!serial || !pk || !demacia_hex || hex_size < 513) return false;

    /* 补齐序列号到10位 */
    char padded_serial[11];
    int slen = (int)strlen(serial);
    memset(padded_serial, '0', 10);
    if (slen >= 10) memcpy(padded_serial, serial + slen - 10, 10);
    else memcpy(padded_serial + 10 - slen, serial, slen);
    padded_serial[10] = '\0';

    /* SHA256("2e7006834dafe8ad" + serial + "a6674c6b039707ff") */
    char hash_src[128];
    snprintf(hash_src, sizeof(hash_src), "2e7006834dafe8ad%sa6674c6b039707ff", padded_serial);
    uint8_t hash[32];
    if (!compute_sha256((const uint8_t *)hash_src, (int)strlen(hash_src), hash))
        return false;

    /* 构建 256 字节明文 */
    uint8_t plain[256];
    memset(plain, 0, sizeof(plain));
    memcpy(plain, "907heavyworkload", 16);
    memcpy(plain + 16, hash, 32);

    /* AES-256-CBC 加密 */
    uint8_t key[32];
    memcpy(key, AES_KEY_PRE_DEMACIA, 8);
    memcpy(key + 8, pk, 16);
    memcpy(key + 24, AES_KEY_SUF_DEMACIA, 8);

    uint8_t cipher[256];
    if (!aes_cbc_encrypt(key, 32, AES_IV_DEMACIA, plain, 256, cipher))
        return false;

    bytes_to_hex_upper(cipher, 256, demacia_hex);
    return true;
#else
    (void)serial; (void)pk; (void)demacia_hex; (void)hex_size;
    return false;
#endif
}

bool edl_oneplus_generate_setprojmodel(const char *model_id, const char *serial,
                                        const char *pk, const char *proj_id,
                                        char *token_hex, int hex_size)
{
#ifdef _WIN32
    if (!model_id || !serial || !pk || !proj_id || !token_hex || hex_size < 513) return false;

    const char *prod_key = (strcmp(proj_id, "18825") == 0 || strcmp(proj_id, "18801") == 0)
                           ? PROD_KEY_OLD : PROD_KEY_NEW;

    /* modelHash = SHA256(prodKey + modelId + randomV1) */
    char h1[256];
    snprintf(h1, sizeof(h1), "%s%s%s", prod_key, model_id, RANDOM_V1);
    uint8_t hash1[32];
    compute_sha256((const uint8_t *)h1, (int)strlen(h1), hash1);
    char model_hash[65];
    bytes_to_hex_upper(hash1, 32, model_hash);

    /* timestamp */
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));

    /* secret = SHA256(prefix + model + "0" + serial + version + ts + modelHash + suffix) */
    char h2[512];
    snprintf(h2, sizeof(h2), "c4b95538c57df231%s0%s%s%s%s5b0217457e49381b",
             model_id, serial, VERSION_V1, ts, model_hash);
    uint8_t hash2[32];
    compute_sha256((const uint8_t *)h2, (int)strlen(h2), hash2);
    char secret[65];
    bytes_to_hex_upper(hash2, 32, secret);

    /* 构建明文 */
    char data_str[384];
    snprintf(data_str, sizeof(data_str), "%s,%s,%s,%s,0,%s,%s,%s",
             model_id, RANDOM_V1, model_hash, VERSION_V1, serial, ts, secret);

    uint8_t plain[256];
    memset(plain, 0, 256);
    memcpy(plain, data_str, strlen(data_str) > 255 ? 255 : strlen(data_str));

    /* AES-256-CBC */
    uint8_t key[32];
    memcpy(key, AES_KEY_PRE_V1, 8);
    memcpy(key + 8, pk, 16);
    memcpy(key + 24, AES_KEY_SUF_V1, 8);

    uint8_t cipher[256];
    if (!aes_cbc_encrypt(key, 32, AES_IV_V1, plain, 256, cipher))
        return false;

    bytes_to_hex_upper(cipher, 256, token_hex);
    return true;
#else
    (void)model_id; (void)serial; (void)pk; (void)proj_id; (void)token_hex; (void)hex_size;
    return false;
#endif
}

bool edl_oneplus_generate_setswprojmodel(const char *model_id, const char *serial,
                                          const char *pk, const char *proj_id,
                                          const char *device_ts,
                                          char *token_hex, int hex_size)
{
#ifdef _WIN32
    if (!model_id || !serial || !pk || !proj_id || !device_ts || !token_hex || hex_size < 1025)
        return false;

    const char *prod_key = (strcmp(proj_id, "18825") == 0 || strcmp(proj_id, "18801") == 0)
                           ? PROD_KEY_OLD : PROD_KEY_NEW;

    char h1[256];
    snprintf(h1, sizeof(h1), "%s%s%s", prod_key, model_id, RANDOM_V3);
    uint8_t hash1[32];
    compute_sha256((const uint8_t *)h1, (int)strlen(h1), hash1);
    char model_hash[65];
    bytes_to_hex_upper(hash1, 32, model_hash);

    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));

    char h2[512];
    snprintf(h2, sizeof(h2), "%s%s%s%s%s%s8f7359c8a2951e8c",
             prod_key, model_id, serial, VERSION_V3, ts, model_hash);
    uint8_t hash2[32];
    compute_sha256((const uint8_t *)h2, (int)strlen(h2), hash2);
    char secret[65];
    bytes_to_hex_upper(hash2, 32, secret);

    /* device_id = hex → decimal */
    char device_id_str[32];
    long device_id = strtol(model_id, NULL, 16);
    snprintf(device_id_str, sizeof(device_id_str), "%ld", device_id);

    char data_str[512];
    snprintf(data_str, sizeof(data_str), "%s,%s,%s,0,0,%s,%s,%s,%s,%s",
             model_id, RANDOM_V3, model_hash, VERSION_V3, serial, device_id_str, ts, secret);

    uint8_t plain[512];
    memset(plain, 0, 512);
    memcpy(plain, data_str, strlen(data_str) > 511 ? 511 : strlen(data_str));

    /* AES key: prefix(8) + pk(16) + device_ts_le(8) */
    uint8_t key[32];
    memcpy(key, AES_KEY_PRE_V3, 8);
    memcpy(key + 8, pk, 16);
    int64_t ts_val = strtoll(device_ts, NULL, 10);
    memcpy(key + 24, &ts_val, 8);

    uint8_t cipher[512];
    if (!aes_cbc_encrypt(key, 32, AES_IV_V3, plain, 512, cipher))
        return false;

    bytes_to_hex_upper(cipher, 512, token_hex);
    return true;
#else
    (void)model_id; (void)serial; (void)pk; (void)proj_id;
    (void)device_ts; (void)token_hex; (void)hex_size;
    return false;
#endif
}

bool edl_oneplus_extract_projid(const char *response, char *proj_id, int size)
{
    if (!response || !proj_id) return false;

    /* 搜索 prjversion= 或 PrjVersion= 或 projid= */
    const char *patterns[] = {"prjversion=\"","PrjVersion=\"","projid=\"",NULL};
    for (int p = 0; patterns[p]; p++) {
        const char *s = strstr(response, patterns[p]);
        if (!s) continue;
        s += strlen(patterns[p]);
        int i = 0;
        while (*s && *s != '"' && i < size - 1)
            proj_id[i++] = *s++;
        proj_id[i] = '\0';
        if (i >= 5) return true;
    }
    return false;
}

bool edl_oneplus_extract_timestamp(const char *response, char *ts, int size)
{
    if (!response || !ts) return false;
    const char *p = strstr(response, "device_timestamp=\"");
    if (!p) return false;
    p += 18;
    int i = 0;
    while (*p && *p != '"' && i < size - 1)
        ts[i++] = *p++;
    ts[i] = '\0';
    return i > 0;
}

bool edl_oneplus_is_auth_success(const char *response)
{
    if (!response) return false;
    return strstr(response, "model_check=\"0\"") != NULL ||
           strstr(response, "verify_res=\"0\"") != NULL ||
           strstr(response, "value=\"ACK\"") != NULL;
}
