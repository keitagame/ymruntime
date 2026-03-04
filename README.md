# YM Runtime

YM2203 (OPN) / YM2151 (OPM) FM音源エミュレーターランタイム。  
バックグラウンドデーモンとして動作し、複数のアプリケーションから同時に使用できます。

---

## アーキテクチャ

```
┌──────────────────────────────────────────────────────────┐
│                   アプリケーション群                         │
│  myapp1.c     myapp2.c     ymctl (CLI)    VGMプレーヤー   │
│    │               │            │               │         │
│    └───────────────┴────────────┴───────────────┘         │
│                         │ Unix Domain Socket               │
│                         │ /tmp/ymruntime.sock              │
└─────────────────────────┼────────────────────────────────┘
                          ▼
┌──────────────────────────────────────────────────────────┐
│                    ymruntimed (デーモン)                    │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ YM2203#1 │  │ YM2151#1 │  │ YM2203#2 │  ...          │
│  └──────────┘  └──────────┘  └──────────┘               │
│        │            │              │                      │
│        └────────────┴──────────────┘                     │
│                     │ ミックス (44100Hz Stereo)            │
│                     ▼                                     │
│              PulseAudio / PipeWire                        │
└──────────────────────────────────────────────────────────┘
```

## ディレクトリ構成

```
ymruntime/
├── include/
│   ├── ym_core.h      # チップエミュレーション共通定義
│   ├── ym_ipc.h       # IPCプロトコル定義
│   └── vgm_player.h   # VGMプレーヤーヘッダ
├── src/
│   ├── ym2203.c       # YM2203 (OPN) エミュレーター
│   ├── ym2151.c       # YM2151 (OPM) エミュレーター
│   ├── ym_core.c      # 統合ラッパー + IPCヘルパー
│   ├── vgm_player.c   # VGMファイルパーサー/プレーヤー
│   └── ymruntimed.c   # メインデーモン
├── client/
│   ├── ymclient.h     # クライアントライブラリヘッダ
│   └── ymclient.c     # クライアントライブラリ実装
├── tools/
│   └── ymctl.c        # コマンドラインコントロールツール
├── systemd/
│   └── ymruntime.service  # systemdユニットファイル
└── Makefile
```

## ビルド

### 依存関係

```bash
# Ubuntu/Debian
sudo apt install libpulse-dev gcc make

# Arch Linux
sudo pacman -S libpulse gcc make

# Fedora
sudo dnf install pulseaudio-libs-devel gcc make
```

### コンパイル

```bash
make all
```

### インストール

```bash
sudo make install
```

### systemdサービス登録 (ユーザー単位)

```bash
make install-service
systemctl --user enable --now ymruntime
```

---

## 使い方

### 1. デーモン起動

```bash
# フォアグラウンドで起動 (デバッグ用)
ymruntimed -f

# バックグラウンド (systemd使用時は不要)
ymruntimed &
```

### 2. コマンドラインツール (ymctl)

```bash
# 疎通確認
ymctl ping

# デバイス一覧
ymctl list

# YM2203デバイスをオープン (クロック4MHz)
ymctl open opn 4000000 myapp
# → デバイスID: 1

# YM2151デバイスをオープン (クロック3.58MHz)
ymctl open opm 3580000 myapp
# → デバイスID: 2

# デモ演奏 (スケール)
ymctl demo scale opn

# デモ演奏 (コード進行)
ymctl demo chord opm brass

# VGMファイルを再生
ymctl vgm load 1 song.vgm 1      # ループ再生
ymctl vgm play 1
ymctl vgm status 1
ymctl vgm pause 1
ymctl vgm stop 1

# 低レベルレジスタ操作
ymctl reg 1 0xB0 0x32            # ALG=2, FB=6
ymctl reg 1 0x28 0x01            # Key On ch0

# 周波数設定
ymctl freq 1 0 440.0             # A4 (440Hz)

# キーオン/オフ
ymctl keyon  1 0 0x0F
ymctl keyoff 1 0 0x0F

# プリセット音色の適用
ymctl patch 1 0 piano
ymctl patch 1 0 organ
ymctl patch 1 0 brass
ymctl patch 1 0 bell

# デバイスをクローズ
ymctl close 1

# デーモン終了
ymctl shutdown
```

---

## C言語からの利用

```c
#include <ymruntime/ymclient.h>

int main(void) {
    // デーモンに接続
    ym_client_t *c = ym_client_connect(NULL);
    if (!c) return 1;

    // YM2203デバイスをオープン
    int dev = ym_client_open_device(c, YM_CHIP_OPN, 0, 4000000, "myapp");
    if (dev < 0) goto done;

    // プリセットパッチを設定
    ym_pkt_patch_t patch;
    ym_patch_piano(&patch, dev, 0);
    ym_client_set_patch(c, dev, &patch);

    // A4 (440Hz) を鳴らす
    uint16_t fnum; uint8_t block;
    ym2203_freq_to_fnum(440.0, 4000000.0, &fnum, &block);
    ym_client_set_freq(c, dev, 0, fnum, block);
    ym_client_key_on(c, dev, 0, 0x0F);

    sleep(1);

    ym_client_key_off(c, dev, 0, 0x0F);
    ym_client_close_device(c, dev);

done:
    ym_client_disconnect(c);
    return 0;
}
```

コンパイル:
```bash
gcc myapp.c -I/usr/local/include/ymruntime \
    -L/usr/local/lib -lymclient -lm -o myapp
```

---

## VGMファイル再生

VGM (Video Game Music) フォーマット 1.00〜1.70 に対応。  
YM2203/YM2151のVGMファイルは以下のサイトで入手できます:
- https://vgmrips.net/

```bash
# YM2203用VGMをロードして再生
ymctl open opn 4000000 vgmplayer   # → デバイスID: 1
ymctl vgm load 1 game.vgm 0        # ループなし
ymctl vgm play 1
ymctl vgm status 1                  # 再生状態確認
```

---

## IPCプロトコル

すべての通信は `/tmp/ymruntime.sock` (Unix Domain Socket) 経由で行われます。  
独自の言語バインディングを作成する場合は `include/ym_ipc.h` のパケット定義を参照してください。

### パケット構造

```
[Header 10 bytes] [Payload N bytes]

Header:
  magic       : 4 bytes  "YMD\0"
  version     : 1 byte
  command     : 1 byte
  seq         : 2 bytes  (シーケンス番号)
  payload_len : 4 bytes
```

---

## チップ仕様

| チップ  | 別名 | FMチャンネル | SSG | ステレオ | クロック |
|---------|------|-------------|-----|----------|---------|
| YM2203  | OPN  | 3ch (4-OP)  | 3ch | モノ     | 4MHz    |
| YM2151  | OPM  | 8ch (4-OP)  | -   | ステレオ | 3.58MHz |

---

## ライセンス

MIT License
