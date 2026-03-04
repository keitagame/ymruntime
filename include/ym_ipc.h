#pragma once
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * YM Runtime IPC プロトコル定義
 * Unix Domain Socket経由でデーモンと通信する
 * ============================================================ */

#define YM_SOCKET_PATH   "/tmp/ymruntime.sock"
#define YM_SOCKET_PATH_ENV "YM_RUNTIME_SOCKET"
#define YM_MAX_DEVICES   8
#define YM_PROTO_MAGIC   0x594D4400u  /* "YMD\0" */
#define YM_PROTO_VERSION 1

/* コマンド種別 */
typedef enum {
    /* デバイス管理 */
    YM_CMD_OPEN_DEVICE   = 0x01,  /* デバイスをオープン */
    YM_CMD_CLOSE_DEVICE  = 0x02,  /* デバイスをクローズ */
    YM_CMD_RESET_DEVICE  = 0x03,  /* チップリセット */
    YM_CMD_LIST_DEVICES  = 0x04,  /* デバイス一覧取得 */

    /* レジスタ操作 */
    YM_CMD_WRITE_REG     = 0x10,  /* レジスタ書き込み */
    YM_CMD_READ_REG      = 0x11,  /* レジスタ読み出し */
    YM_CMD_WRITE_BULK    = 0x12,  /* バルクレジスタ書き込み */

    /* 高レベル操作 */
    YM_CMD_KEY_ON        = 0x20,  /* キーオン */
    YM_CMD_KEY_OFF       = 0x21,  /* キーオフ */
    YM_CMD_SET_FREQ      = 0x22,  /* 周波数設定 */
    YM_CMD_SET_VOLUME    = 0x23,  /* ボリューム設定 */
    YM_CMD_SET_PATCH     = 0x24,  /* パッチ(音色)設定 */

    /* VGM再生 */
    YM_CMD_VGM_LOAD      = 0x30,  /* VGMデータをロード */
    YM_CMD_VGM_PLAY      = 0x31,  /* 再生開始 */
    YM_CMD_VGM_PAUSE     = 0x32,  /* 一時停止 */
    YM_CMD_VGM_STOP      = 0x33,  /* 停止 */
    YM_CMD_VGM_SEEK      = 0x34,  /* シーク */
    YM_CMD_VGM_STATUS    = 0x35,  /* 再生状態取得 */

    /* システム */
    YM_CMD_PING          = 0xF0,  /* 疎通確認 */
    YM_CMD_GET_VERSION   = 0xF1,  /* バージョン取得 */
    YM_CMD_SHUTDOWN      = 0xFF,  /* デーモン終了 */
} ym_command_t;

/* チップ種別 (プロトコル用) */
typedef enum {
    YM_CHIP_NONE  = 0,
    YM_CHIP_OPN   = 1,  /* YM2203 */
    YM_CHIP_OPM   = 2,  /* YM2151 */
    YM_CHIP_OPN2  = 3,  /* YM2612 */
    YM_CHIP_OPL2  = 4,  /* YM3812 */
} ym_chip_id_t;

/* エラーコード */
typedef enum {
    YM_OK              = 0,
    YM_ERR_UNKNOWN     = -1,
    YM_ERR_INVALID_CMD = -2,
    YM_ERR_NO_DEVICE   = -3,
    YM_ERR_MAX_DEVICES = -4,
    YM_ERR_BUSY        = -5,
    YM_ERR_INVALID_ARG = -6,
    YM_ERR_IO          = -7,
    YM_ERR_NO_MEMORY   = -8,
    YM_ERR_VGM_INVALID = -9,
    YM_ERR_VGM_PLAYING = -10,
} ym_error_t;

/* ============================================================
 * パケット構造
 * ============================================================ */

/* 共通ヘッダ (全パケット先頭) */
typedef struct __attribute__((packed)) {
    uint32_t magic;    /* YM_PROTO_MAGIC */
    uint8_t  version;
    uint8_t  command;  /* ym_command_t */
    uint16_t seq;      /* シーケンス番号 */
    uint32_t payload_len;
} ym_pkt_header_t;

/* OPEN_DEVICE ペイロード */
typedef struct __attribute__((packed)) {
    uint8_t  chip_type;   /* ym_chip_id_t */
    uint8_t  chip_index;  /* 同種チップの番号 (0始まり) */
    uint32_t clock_hz;    /* チッククロック (例: 4000000) */
    char     client_name[32];
} ym_pkt_open_t;

/* OPEN_DEVICE レスポンス */
typedef struct __attribute__((packed)) {
    int32_t  result;    /* ym_error_t (0=OK) */
    uint8_t  device_id; /* 割り当てられたデバイスID */
} ym_pkt_open_resp_t;

/* WRITE_REG ペイロード */
typedef struct __attribute__((packed)) {
    uint8_t device_id;
    uint8_t port;   /* ポート番号 (OPNは0/1, OPMは0) */
    uint8_t addr;
    uint8_t data;
} ym_pkt_write_reg_t;

/* WRITE_BULK ペイロード (可変長, headerの後にym_reg_pair_t配列が続く) */
typedef struct __attribute__((packed)) {
    uint8_t  device_id;
    uint8_t  port;
    uint16_t count;   /* ペア数 */
} ym_pkt_write_bulk_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t addr;
    uint8_t data;
    uint16_t delay_samples; /* 書き込み後のディレイ(サンプル数) */
} ym_reg_pair_t;

/* KEY_ON/OFF ペイロード */
typedef struct __attribute__((packed)) {
    uint8_t device_id;
    uint8_t channel;   /* 0始まり */
    uint8_t slot_mask; /* どのOPをONにするか (bit0=OP1..bit3=OP4) */
} ym_pkt_key_t;

/* SET_FREQ ペイロード */
typedef struct __attribute__((packed)) {
    uint8_t  device_id;
    uint8_t  channel;
    uint16_t fnum;    /* F-Number (OPN) or KC<<6|KF (OPM) */
    uint8_t  block;   /* ブロック/オクターブ */
} ym_pkt_freq_t;

/* SET_PATCH ペイロード - 1チャンネル分の音色 */
typedef struct __attribute__((packed)) {
    uint8_t  device_id;
    uint8_t  channel;
    uint8_t  algorithm;
    uint8_t  feedback;
    uint8_t  pan;
    uint8_t  ams;
    uint8_t  pms;
    /* 4オペレータ × 11パラメータ */
    struct {
        uint8_t ar, dr, sr, rr, sl, tl, ks, mul, dt1, dt2, am;
    } op[4];
} ym_pkt_patch_t;

/* VGM_LOAD ペイロード (ヘッダの後にvgmデータが続く) */
typedef struct __attribute__((packed)) {
    uint8_t  device_id;
    uint32_t data_len;
    uint8_t  loop;    /* 1=ループ再生 */
} ym_pkt_vgm_load_t;

/* VGM_STATUS レスポンス */
typedef struct __attribute__((packed)) {
    uint8_t  state;        /* 0=停止, 1=再生中, 2=一時停止 */
    uint32_t position_ms;  /* 現在位置(ms) */
    uint32_t total_ms;     /* 総時間(ms) */
    uint32_t loop_count;   /* ループ回数 */
} ym_pkt_vgm_status_t;

/* LIST_DEVICES レスポンス */
typedef struct __attribute__((packed)) {
    uint8_t count;
    struct {
        uint8_t  device_id;
        uint8_t  chip_type;
        uint8_t  chip_index;
        uint8_t  in_use;
        uint32_t clock_hz;
        char     owner_name[32];
    } devices[YM_MAX_DEVICES];
} ym_pkt_list_resp_t;

/* 汎用レスポンス */
typedef struct __attribute__((packed)) {
    int32_t result;   /* ym_error_t */
    uint8_t data[60]; /* コマンド依存データ */
} ym_pkt_resp_t;

/* ============================================================
 * ヘルパー関数プロトタイプ
 * ============================================================ */

/* パケット送受信 */
int ym_send_packet(int fd, ym_command_t cmd, uint16_t seq,
                   const void *payload, size_t payload_len);
int ym_recv_packet(int fd, ym_pkt_header_t *hdr,
                   void *payload_buf, size_t buf_size);

/* エラー文字列 */
const char* ym_strerror(ym_error_t err);
