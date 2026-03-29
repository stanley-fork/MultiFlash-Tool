#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
分析 Android 线刷包中的 system.img（或任意分区镜像）内部构造：

1) 原始 bin / 已展开的 ext4：从字节 0 起识别 EXT4 / EROFS 超块位置
2) Android sparse（魔数 0xED26FF3A）：按与 edl/sparse.c 相同规则展开逻辑前缀，再识别 FS

输出与 EDL 中定义一致，用于校验「精准偏移」：
  - EXT4: 主超块在分区/逻辑镜像偏移 1024，s_magic (0xEF53) 在超块内偏移 56 → 文件绝对偏移 1080
  - EROFS: 超块在偏移 1024，魔数在超块首 4 字节 → 与 1024 对齐处读 u32

用法:
  python analyze_system_img.py path/to/system.img [--max-mb 256] [--json]

与 C 代码对应:
  ext4_parser.h: EXT4_SUPERBLOCK_OFF, EXT4_MAGIC_FILE_OFFSET (1080)
  erofs_parser.h: EROFS_SUPERBLOCK_OFF / EROFS_MAGIC_FILE_OFFSET
  sparse.c:      SPARSE_HEADER_MAGIC, chunk 类型

路径说明（避免多一层 EDL）:
  若在仓库根目录 D:\\...\\EDL\\EDL 下执行，请用:
    python scripts\\analyze_system_img.py D:\\\\path\\\\system.img
  不要使用 EDL\\scripts\\...（会解析成 ...\\EDL\\EDL\\EDL\\scripts）。
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import BinaryIO, List, Optional, Tuple

# --- 与 edl/sparse.h 一致 ---
SPARSE_HEADER_MAGIC = 0xED26FF3A
SPARSE_CHUNK_RAW = 0xCAC1
SPARSE_CHUNK_FILL = 0xCAC2
SPARSE_CHUNK_DONT_CARE = 0xCAC3
SPARSE_CHUNK_CRC32 = 0xCAC4

# --- 与 ext4_parser.h / erofs_parser.h 一致 ---
EXT4_SUPERBLOCK_OFF = 1024
EXT4_SUPER_MAGIC = 0xEF53
EXT4_MAGIC_OFF_IN_SB = 56  # ext4_superblock_t.s_magic

EROFS_SUPERBLOCK_OFF = 1024
EROFS_SUPER_MAGIC = 0xE0F5E1E2


def le16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def le32(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)


def read_sparse_header(f: BinaryIO) -> Optional[Tuple[dict, int]]:
    """返回 (字段 dict, 下一读位置 file position after header)"""
    f.seek(0)
    raw = f.read(28)
    if len(raw) < 28:
        return None
    magic = le32(raw, 0)
    if magic != SPARSE_HEADER_MAGIC:
        return None
    major, minor = struct.unpack_from("<HH", raw, 4)
    file_hdr_sz, chunk_hdr_sz = struct.unpack_from("<HH", raw, 8)
    blk_sz, total_blks, total_chunks, _csum = struct.unpack_from("<IIII", raw, 12)
    info = {
        "format": "android_sparse",
        "magic_hex": f"0x{magic:08X}",
        "major": major,
        "minor": minor,
        "file_hdr_sz": file_hdr_sz,
        "chunk_hdr_sz": chunk_hdr_sz,
        "blk_sz": blk_sz,
        "total_blks": total_blks,
        "total_chunks": total_chunks,
        "logical_size_bytes": blk_sz * total_blks,
    }
    pos = file_hdr_sz if file_hdr_sz >= 28 else 28
    return info, pos


def expand_sparse_prefix(f: BinaryIO, max_bytes: int) -> Tuple[bytes, List[dict]]:
    """
    展开 sparse 为逻辑字节流前缀（与 edl_sparse_read_block 逐块语义一致）。
    返回 (buffer, chunk 摘要列表)
    """
    parsed = read_sparse_header(f)
    if not parsed:
        raise ValueError("not sparse")
    info, pos = parsed
    f.seek(pos)
    blk_sz = info["blk_sz"]
    chunk_hdr_sz = info["chunk_hdr_sz"]
    out = bytearray()
    chunk_log: List[dict] = []
    buf_blk = bytearray(blk_sz)

    blocks_left_in_chunk = 0
    chunk_type: Optional[int] = None
    fill_pattern = 0
    current_chunk_idx = 0

    while len(out) < max_bytes:
        if blocks_left_in_chunk == 0:
            if current_chunk_idx >= info["total_chunks"]:
                break
            ch_raw = f.read(12)
            if len(ch_raw) < 12:
                break
            ctype, _res, chunk_sz_blks, total_sz = struct.unpack("<HHI I", ch_raw)
            extra = chunk_hdr_sz - 12
            if extra > 0:
                f.read(extra)

            current_chunk_idx += 1
            chunk_type = ctype
            blocks_left_in_chunk = chunk_sz_blks

            if ctype == SPARSE_CHUNK_CRC32:
                data_bytes = total_sz - chunk_hdr_sz
                if data_bytes > 0:
                    f.seek(data_bytes, 1)
                chunk_log.append(
                    {
                        "index": current_chunk_idx,
                        "type": "CRC32",
                        "skipped_bytes": data_bytes,
                    }
                )
                chunk_type = None
                continue

            if ctype == SPARSE_CHUNK_FILL:
                fv = f.read(4)
                if len(fv) < 4:
                    break
                fill_pattern = le32(fv, 0)

            chunk_log.append(
                {
                    "index": current_chunk_idx,
                    "type_hex": f"0x{ctype:04X}",
                    "blocks": chunk_sz_blks,
                }
            )

        if chunk_type is None or blocks_left_in_chunk <= 0:
            continue

        need = min(blk_sz, max_bytes - len(out))
        if chunk_type == SPARSE_CHUNK_RAW:
            got = f.read(blk_sz)
            if len(got) < blk_sz:
                out.extend(got[:need])
                break
            out.extend(got[:need])
        elif chunk_type == SPARSE_CHUNK_FILL:
            for i in range(0, blk_sz, 4):
                buf_blk[i : i + 4] = struct.pack("<I", fill_pattern)
            out.extend(buf_blk[:need])
        elif chunk_type == SPARSE_CHUNK_DONT_CARE:
            out.extend(b"\x00" * need)
        else:
            chunk_log.append({"error": f"unknown chunk 0x{chunk_type:04X}"})
            break

        blocks_left_in_chunk -= 1

    return bytes(out), chunk_log


def read_raw_prefix(path: Path, max_bytes: int) -> bytes:
    with path.open("rb") as f:
        return f.read(max_bytes)


def sniff_fs(buf: bytes) -> dict:
    """在逻辑偏移 0 处识别 EXT4 / EROFS（与 fs_prop_probe try_one_offset(0) 一致）"""
    n = len(buf)
    out: dict = {"logical_fs_start": 0, "fs": None}

    # EXT4: 超块 1024，魔数 @ 1024+56
    if n >= EXT4_SUPERBLOCK_OFF + 60:
        off_magic = EXT4_SUPERBLOCK_OFF + EXT4_MAGIC_OFF_IN_SB
        m = le16(buf, off_magic)
        if m == EXT4_SUPER_MAGIC:
            lbs = le32(buf, EXT4_SUPERBLOCK_OFF + 24)
            log_bs = lbs if lbs <= 16 else 99
            block_size = 1024 << log_bs if log_bs <= 16 else -1
            out["fs"] = "ext4"
            out["ext4"] = {
                "superblock_off": EXT4_SUPERBLOCK_OFF,
                "magic_u16_off": off_magic,
                "magic_abs_off": off_magic,
                "magic": f"0x{m:04X}",
                "s_log_block_size": lbs,
                "block_size": block_size,
            }

    # EROFS: 超块 1024，魔数在 1024
    if n >= EROFS_SUPERBLOCK_OFF + 4 and out["fs"] is None:
        em = le32(buf, EROFS_SUPERBLOCK_OFF)
        if em == EROFS_SUPER_MAGIC:
            out["fs"] = "erofs"
            out["erofs"] = {
                "superblock_off": EROFS_SUPERBLOCK_OFF,
                "magic_u32_off": EROFS_SUPERBLOCK_OFF,
                "magic": f"0x{em:08X}",
            }

    return out


def analyze(path: Path, max_mb: int) -> dict:
    max_bytes = max_mb * 1024 * 1024
    with path.open("rb") as f:
        head = f.read(4)

    result: dict = {"file": str(path.resolve()), "size_bytes": path.stat().st_size}

    if len(head) >= 4 and le32(head, 0) == SPARSE_HEADER_MAGIC:
        with path.open("rb") as f:
            hdr, _ = read_sparse_header(f)
            if not hdr:
                raise RuntimeError("sparse magic but header parse failed")
            result["container"] = hdr
            with path.open("rb") as f:
                buf, chunks = expand_sparse_prefix(f, max_bytes)
            result["sparse_chunk_summary"] = chunks[:32]  # 避免过长
            result["sparse_chunk_total_logged"] = len(chunks)
            result["expanded_prefix_bytes"] = len(buf)
    else:
        result["container"] = {"format": "raw"}
        buf = read_raw_prefix(path, max_bytes)
        result["expanded_prefix_bytes"] = len(buf)

    result["sniff"] = sniff_fs(buf)
    result["c_constants"] = {
        "EXT4_SUPERBLOCK_OFF": EXT4_SUPERBLOCK_OFF,
        "EXT4_MAGIC_OFF_IN_SUPERBLOCK": EXT4_MAGIC_OFF_IN_SB,
        "EXT4_MAGIC_FILE_OFFSET": EXT4_SUPERBLOCK_OFF + EXT4_MAGIC_OFF_IN_SB,
        "EROFS_SUPERBLOCK_OFF": EROFS_SUPERBLOCK_OFF,
        "SPARSE_HEADER_MAGIC": f"0x{SPARSE_HEADER_MAGIC:08X}",
    }
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="分析 system.img / 分区镜像构造（sparse + EXT4/EROFS 偏移）")
    ap.add_argument("image", type=Path, nargs="?", help="镜像路径，例如 system.img")
    ap.add_argument("--max-mb", type=int, default=256, help="展开/读取的最大前缀 MiB（默认 256）")
    ap.add_argument("--json", action="store_true", help="JSON 输出")
    args = ap.parse_args()

    if not args.image:
        ap.print_help()
        print(
            "\n示例（当前目录为仓库根 EDL\\EDL 时）:\n"
            "  python scripts\\\\analyze_system_img.py D:\\\\RMX1901_...\\\\system.img\n",
            file=sys.stderr,
        )
        return 1

    p = args.image
    if not p.is_file():
        print(f"文件不存在: {p}", file=sys.stderr)
        return 2

    try:
        data = analyze(p, args.max_mb)
    except Exception as e:
        print(f"分析失败: {e}", file=sys.stderr)
        return 3

    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print("===", data["file"], "===")
        print("容器:", data["container"].get("format"), end="")
        if data["container"].get("format") == "android_sparse":
            c = data["container"]
            print(f" | blk={c['blk_sz']} total_logical={c['logical_size_bytes']} bytes")
        else:
            print()
        print("前缀长度:", data["expanded_prefix_bytes"], "bytes")
        sn = data["sniff"]
        print("逻辑 FS 起点: 0")
        if sn.get("fs"):
            print("识别:", sn["fs"], sn.get(sn["fs"], {}))
        else:
            print("识别: 在前缀内未找到 EXT4(1080)/EROFS(1024) — 可能非 system 分区、需更大 --max-mb、或 FS 不在偏移 0")
        print("\n与 C 对齐的常量 (ext4_parser.h / erofs_parser.h):")
        for k, v in data["c_constants"].items():
            print(f"  {k} = {v}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
