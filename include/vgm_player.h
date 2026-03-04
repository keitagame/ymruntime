#pragma once
/*
 * vgm_player.h - VGMファイルプレーヤー
 * VGM 1.50+ (YM2203/YM2151対応)
 */

#include <stdint.h>
#include <stddef.h>
#include "ym_core.h"

/* VGMヘッダ構造 */
typedef struct __attribute__((packed)) {
    uint32_t ident;          /* "Vgm " = 0x206D6756 */
    uint32_t eof_offset;
    uint32_t version;
    uint32_t sn76489_clock;
    uint32_t ym2413_clock;
    uint32_t gd3_offset;
    uint32_t total_samples;
    uint32_t loop_offset;
    uint32_t loop_samples;
    uint32_t rate;           /* 再生レート (Hz), 通常60 */
    uint16_t sn76489_feedback;
    uint8_t  sn76489_shift_width;
    uint8_t  sn76489_flags;
    uint32_t ym2612_clock;
    uint32_t ym2151_clock;
    uint32_t vgm_data_offset; /* VGM 1.50+: データ開始オフセット (相対) */
    /* ... (以降は各チップクロック) */
    uint32_t segapcm_clock;
    uint32_t segapcm_interface;
    uint32_t rf5c68_clock;
    uint32_t ym2203_clock;
} vgm_header_t;

#define VGM_IDENT 0x206D6756u  /* "Vgm " */

/* VGMコマンド */
#define VGM_CMD_YM2612_0  0x52  /* YM2612 port 0 write */
#define VGM_CMD_YM2612_1  0x53  /* YM2612 port 1 write */
#define VGM_CMD_YM2151    0x54  /* YM2151 write */
#define VGM_CMD_YM2203    0x55  /* YM2203 write */
#define VGM_CMD_WAIT_N    0x61  /* Wait N samples (uint16_t follows) */
#define VGM_CMD_WAIT_735  0x62  /* Wait 735 samples (1/60 sec) */
#define VGM_CMD_WAIT_882  0x63  /* Wait 882 samples (1/50 sec) */
#define VGM_CMD_END       0x66  /* End of data */
#define VGM_CMD_DATA_BLK  0x67  /* Data block */
#define VGM_CMD_WAIT_1    0x70  /* Wait (n&0xF)+1 samples */

typedef enum {
    VGM_STATE_STOPPED = 0,
    VGM_STATE_PLAYING,
    VGM_STATE_PAUSED,
} vgm_state_t;

typedef struct {
    /* データ */
    uint8_t      *data;
    size_t        data_len;
    const uint8_t *cmd_ptr;   /* 現在のコマンドポインタ */
    const uint8_t *cmd_start; /* データ先頭 */
    const uint8_t *cmd_end;

    /* 状態 */
    vgm_state_t   state;
    uint32_t      wait_samples;  /* 残りウェイトサンプル数 */
    uint32_t      position_samples; /* 再生位置(サンプル数) */
    uint32_t      total_samples;
    uint32_t      loop_count;
    uint8_t       loop;
    const uint8_t *loop_ptr;

    /* ターゲットチップ */
    ym_chip_t    *chip_2203;  /* NULLなら無視 */
    ym_chip_t    *chip_2151;  /* NULLなら無視 */

    /* コールバック (再生終了時) */
    void (*on_finished)(void *userdata);
    void *userdata;
} vgm_player_t;

/* API */
int  vgm_player_init(vgm_player_t *player);
void vgm_player_destroy(vgm_player_t *player);
int  vgm_player_load(vgm_player_t *player, const uint8_t *data, size_t len, int loop);
int  vgm_player_play(vgm_player_t *player);
void vgm_player_pause(vgm_player_t *player);
void vgm_player_stop(vgm_player_t *player);
int  vgm_player_seek(vgm_player_t *player, uint32_t sample_pos);

/* オーディオコールバックから呼ぶ: frameサンプル分進める */
/* 出力はplayer内のチップが直接レンダリングする */
void vgm_player_step(vgm_player_t *player, size_t frames);

uint32_t vgm_player_position_ms(const vgm_player_t *player);
uint32_t vgm_player_total_ms(const vgm_player_t *player);
