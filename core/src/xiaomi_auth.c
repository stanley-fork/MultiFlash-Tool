#include "edl/xiaomi_auth.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===== 内置签名 (Base64 → 二进制) ===== */

/* 最小 Base64 解码器 */
static const uint8_t b64_table[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255,
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255
};

static int b64_decode(const char *src, uint8_t *dst, int max_dst)
{
    int len = (int)strlen(src);
    int out = 0;
    uint32_t buf = 0;
    int bits = 0;

    for (int i = 0; i < len && out < max_dst; i++) {
        uint8_t v = b64_table[(unsigned char)src[i]];
        if (v == 255) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

/* 预置签名 Base64 (来自 edlclient 签名库) */
static const char *builtin_sigs[] = {
    "k246jlc8rQfBZ2RLYSF4Ndha1P3bfYQKK3IlQy/NoTp8GSz6l57RZRfmlwsbB99sUW/sgfaWj89/"
    "/dvDl6Fiwso+XXYSSqF2nxshZLObdpMLTMZ1GffzOYd2d/ToryWChoK8v05ZOlfn4wUyaZJT4LHMXZ0N"
    "VUryvUbVbxjW5SkLpKDKwkMfnxnEwaOddmT/q0ip4RpVk4aBmDW4TfVnXnDSX9tRI+ewQP4hEI8K5tf"
    "Z0mfyycYa0FTGhJPcTTP3TQzy1Krc1DAVLbZ8IqGBrW13YWN/cMvaiEzcETNyA4N3kOaEXKWodnkwucJ"
    "v2nEnJWTKNHY9NS9f5Cq3OPs4pQ==",

    "vzXWATo51hZr4Dh+a5sA/Q4JYoP4Ee3oFZSGbPZ2tBsaMupn+6tPbZDkXJRLUzAqHaMtlPMKaOHrEWZy"
    "sCkgCJqpOPkUZNaSbEKpPQ6uiOVJpJwA/PmxuJ72inzSPevriMAdhQrNUqgyu4ATTEsOKnoUIuJTDBmzC"
    "euh/34SOjTdO4Pc+s3ORfMD0TX+WImeUx4c9xVdSL/xirPl/BouhfuwFd4qPPyO5RqkU/fevEoJWGHaF"
    "jfI302c9k7EpfRUhq1z+wNpZblOHuj0B3/7VOkK8KtSvwLkmVF/t9ECiry6G5iVGEOyqMlktNlIAbr2M"
    "MYXn6b4Y3GDCkhPJ5LUkQ=="
};

#define BUILTIN_SIG_COUNT 2

/* ===== Public API ===== */

int edl_xiaomi_build_sig_request(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><sig TargetName=\"sig\" "
        "size_in_bytes=\"256\" verbose=\"1\"/></data>");
}

int edl_xiaomi_build_token_request(char *buf, int buf_size)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" ?><data><sig TargetName=\"req\" /></data>");
}

int edl_xiaomi_builtin_sign_count(void)
{
    return BUILTIN_SIG_COUNT;
}

int edl_xiaomi_builtin_sign(int index, uint8_t *data, int max_len)
{
    if (index < 0 || index >= BUILTIN_SIG_COUNT || !data) return 0;
    return b64_decode(builtin_sigs[index], data, max_len);
}

bool edl_xiaomi_is_auth_success(const char *response)
{
    if (!response) return false;
    /* 小米认证成功标志 */
    if (strstr(response, "authenticated")) return true;
    if (strstr(response, "Authenticated")) return true;
    if (strstr(response, "value=\"ACK\"")) return true;
    return false;
}

bool edl_xiaomi_extract_token(const char *response, char *out, int out_size)
{
    if (!response || !out || out_size <= 0) return false;

    /* 提取 value 属性 */
    const char *p = strstr(response, "value=\"");
    if (!p) return false;
    p += 7;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}
