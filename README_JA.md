<p align="center">
  <img src="assets/logo.png" alt="SakuraEDL Logo" width="128">
</p>

# SakuraEDL

**オープンソースの Windows デスクトップツール。Qualcomm EDL・MediaTek (MTK)・Fastboot など、複数モードでのフラッシュとデバイス管理をサポート。**

[中文](README.md) | [English](README_EN.md) | [日本語](README_JA.md) | [한국어](README_KO.md) | [Русский](README_RU.md) | [Español](README_ES.md)

[![.NET 8](https://img.shields.io/badge/.NET-8.0-512BD4?logo=dotnet)](https://dotnet.microsoft.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?logo=windows)](https://www.microsoft.com/windows)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## 概要

SakuraEDL は Windows 向けの Android フラッシュ・復旧ツールです。USB で接続し、次のモードを利用できます。

- **Qualcomm EDL (9008)**：Sahara / Firehose プロトコル、パーティション読み書き、GPT バックアップ、ファームウェア復号など
- **MediaTek (MTK)**：BROM / Preloader、XFlash と XML のデュアルプロトコル、DA ロードとエクスプロイト
- **Fastboot**：パーティション読み書き、OEM アンロック、デバイス情報、Huawei/Honor などのベンダー拡張

個人の学習・研究・復旧・フラッシュに利用できます。ファームウェアとドライバは各自で用意してください。

---

## 機能概要

| プラットフォーム | 主な機能 |
|------------------|----------|
| **Qualcomm EDL** | Sahara V2/V3、Firehose フラッシュ、GPT バックアップ/復元、eMMC/UFS 検出、OFP/OZIP/OPS 復号、Diag（IMEI/MEID/QCN）、Loader 機能検出、Motorola ファームウェア |
| **MTK** | BROM/Preloader、XFlash + XML デュアルプロトコル、DA ロード、Carbonara/AllinoneSignature エクスプロイト、CRC32 チェック |
| **Fastboot** | パーティション読み書き、OEM アンロック/リロック、デバイス情報、Huawei/Honor（FRP、Device ID、Bootloader アンロック） |

### Qualcomm EDL 認証方式

一部デバイスは EDL でパーティション読み書きの前にベンダー認証が必要です。本ツールは複数の認証モードに対応しており、接続時に選択できます。

| モード | 説明 |
|--------|------|
| **認証なし (none)** | 標準 Firehose。digest/signature 不要。多くの公開 Loader デバイス向け。 |
| **VIP / OPLUS** | 共通 VIP パーティション認証。先に digest と signature を送信し、通過後に configure と読み書き。OPPO/OnePlus などの一部 Loader で使用。 |
| **OnePlus / Demacia** | OnePlus 専用フロー。Firehose configure の後に認証を実行。 |
| **Xiaomi** | Xiaomi EDL 認証。一部機種は自動検出され、アカウント Token の入力が求められます。 |
| **ベンダー専用（例: Realme）** | ベンダー独自 Loader の認証。**Modern**（新機種、Sahara 後に digest）、**Legacy initdigest**（getstorageinfo + initdigest）、**Legacy 簡易**（nop → configure → getsigndata のみ）などのサブタイプがあり、Loader の banner で自動判定して対応フローを実行します。 |

接続前に「認証モード」でデバイスに合う項目を選択してください。不明な場合はまず「認証なし」を選び、読み書きできない場合にベンダーモードを試してください。

### 共通機能

- 多言語 UI（日本語・英語・中国語・韓国語・ロシア語・スペイン語）
- クラウド Loader マッチング（Qualcomm、チップ ID で Loader 取得）
- Payload.bin 解析、Super パーティション結合、Sparse/Raw 変換、rawprogram 解析

---

## 環境と実行

- **OS**：Windows 10/11（64 ビット）
- **ランタイム**：.NET 8（Windows Forms）
- **ドライバ**：Qualcomm 9008、MTK PreLoader、ADB/Fastboot など用途に応じてインストール

### クイックスタート

1. [Releases](https://github.com/xiriovo/SakuraEDL/releases) から最新版をダウンロードして解凍（パスは英数字推奨）。
2. デバイスに合わせて USB ドライバをインストール。
3. `SakuraEDL.exe` を実行し、ポートとモードを選択して操作。

### ソースからビルド

```bash
# .NET 8 SDK が必要
dotnet restore
dotnet build -c Release
# 出力: bin/Release/net8.0-windows/
```

- **公開/GitHub 用ビルド**: Realme 認証はデフォルトで除外。上記の `dotnet build` でビルド可能。
- **Realme 認証を含むフルビルド**: 対応するソースがある場合のみ `dotnet build -c Release -p:ExcludeRealmeAuth=false` を使用。

詳細は [DEVELOPER.md](DEVELOPER.md)。

---

## プロジェクト構成

```
SakuraEDL/
├── Qualcomm/          # Qualcomm EDL: Sahara, Firehose, Diag, デバイス情報
├── MediaTek/          # MediaTek: BROM, XFlash, XML, DA, エクスプロイト
├── Fastboot/          # Fastboot プロトコルとベンダー拡張（Huawei/Honor 等）
├── Common/            # 共通: 言語、ウォッチドッグ、設定
├── Libs/              # サードパーティ/ローカルライブラリ
├── Properties/        # アセンブリと設定
├── assets/            # アイコン・スクリーンショット
├── SakuraEDL.sln
└── SakuraEDL.csproj
```

---

## よくある質問

- **Qualcomm 9008 で接続できない**：デバイスが EDL モードか確認し、Qualcomm HS-USB ドライバをインストール。USB ポートやケーブルの変更も試してください。
- **MTK が認識されない**：MediaTek PreLoader ドライバをインストールし、電源オフのまま音量キーを押して USB 接続し BROM モードにしてください。
- **切断後にポートが解放されない**：アプリは切断時にポートを解放します。まだ使用中と出る場合はアプリを再起動するか、デバイスを抜き差ししてください。

---

## ライセンス

本プロジェクトは [MIT ライセンス](LICENSE) です。使用・改変・再配布は可能で、著作権とライセンス表示を保持してください。詳しくは [LICENSE](LICENSE)。

---

## 謝辞・参考

- [edl](https://github.com/bkerler/edl) — Qualcomm EDL 参考
- [mtkclient](https://github.com/bkerler/mtkclient) — MTK プロトコル参考

---

## 連絡先

- **QQ**：[SakuraEDL](https://qm.qq.com/q/z3iVnkm22c)
- **Telegram**：[@xiriery](https://t.me/xiriery)
- **GitHub**：[@xiriovo](https://github.com/xiriovo)

---

<p align="center">
  SakuraEDL · Copyright © 2025-2026
</p>
