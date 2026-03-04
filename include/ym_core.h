#pragma once
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * YM Core - FM音源エミュレーション共通定義
 * YM2203 (OPN) / YM2151 (OPM)
 * ============================================================ */

#define YM_SAMPLE_RATE    44100
#define YM_BUFFER_FRAMES  512
#define YM_CHANNELS       2

/* チップ種別 */
typedef enum {
    YM_CHIP_YM2203 = 0x2203,  /* OPN: 3ch FM + 3ch SSG */
    YM_CHIP_YM2151 = 0x2151,  /* OPM: 8ch FM */
} ym_chip_type_t;

/* OPエンベロープパラメータ */
typedef struct {
    uint8_t ar;   /* Attack Rate  0-31 */
    uint8_t dr;   /* Decay Rate   0-31 */
    uint8_t sr;   /* Sustain Rate 0-31 */
    uint8_t rr;   /* Release Rate 0-15 */
    uint8_t sl;   /* Sustain Level 0-15 */
    uint8_t tl;   /* Total Level  0-127 */
    uint8_t ks;   /* Key Scale    0-3 */
    uint8_t mul;  /* Multiple     0-15 */
    uint8_t dt1;  /* Detune1      0-7 (OPM) / Detune 0-7 (OPN) */
    uint8_t dt2;  /* Detune2      0-3 (OPM only) */
    uint8_t am;   /* AM enable    0-1 */
} ym_operator_t;

/* FMチャンネルパラメータ */
typedef struct {
    uint8_t      algorithm; /* 0-7 */
    uint8_t      feedback;  /* 0-7 */
    uint8_t      pan;       /* 0=off,1=R,2=L,3=LR (OPM) */
    uint8_t      ams;       /* AM sensitivity (OPM) */
    uint8_t      pms;       /* PM sensitivity (OPM) */
    ym_operator_t op[4];    /* 4 operators: M1,M2,C1,C2 */
} ym_channel_t;

/* SSGチャンネル (YM2203用) */
typedef struct {
    uint16_t period;   /* 音程周期 */
    uint8_t  volume;   /* 0-15 */
    uint8_t  mode;     /* tone/noise/envelope flags */
    uint8_t  env_shape;
    uint16_t env_period;
} ym_ssg_channel_t;

/* 内部エンベロープステート */
typedef enum {
    ENV_ATTACK = 0,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE,
    ENV_OFF
} env_state_t;

typedef struct {
    env_state_t state;
    uint32_t    volume;     /* 固定小数点 */
    uint32_t    phase;      /* オシレータ位相 */
    uint32_t    phase_inc;
    int32_t     out;        /* フィードバック用前回出力 */
} op_state_t;

/* チャンネルランタイム状態 */
typedef struct {
    op_state_t  op[4];
    uint8_t     key_on;
    uint32_t    freq;   /* F-Number */
    uint8_t     block;  /* オクターブ */
    int32_t     out;    /* 最終出力 */
} ch_state_t;

/* SSGランタイム状態 */
typedef struct {
    uint32_t phase;
    uint32_t phase_inc;
    uint32_t noise_reg;
    uint32_t noise_phase;
    uint32_t env_phase;
    int32_t  out;
} ssg_state_t;

/* YM2203チップインスタンス */
typedef struct {
    ym_channel_t    ch[3];
    ym_ssg_channel_t ssg[3];
    ch_state_t      ch_state[3];
    ssg_state_t     ssg_state[3];
    uint8_t         lfo_enable;
    uint8_t         lfo_freq;
    double          clock;      /* クロック周波数 (Hz) */
    uint32_t        sample_rate;
    /* レジスタキャッシュ */
    uint8_t         regs[0x100];
} ym2203_t;

/* YM2151チップインスタンス */
typedef struct {
    ym_channel_t    ch[8];
    ch_state_t      ch_state[8];
    uint8_t         lfo_enable;
    uint8_t         lfo_freq;
    uint8_t         lfo_waveform; /* 0=sawtooth,1=square,2=triangle,3=noise */
    uint8_t         noise_enable;
    uint8_t         noise_freq;
    double          clock;
    uint32_t        sample_rate;
    uint8_t         regs[0x100];
} ym2151_t;

/* 統合チップハンドル */
typedef struct {
    ym_chip_type_t type;
    union {
        ym2203_t opn;
        ym2151_t opm;
    };
} ym_chip_t;

/* API */
ym_chip_t* ym_create(ym_chip_type_t type, double clock, uint32_t sample_rate);
void       ym_destroy(ym_chip_t *chip);
void       ym_reset(ym_chip_t *chip);
void       ym_write(ym_chip_t *chip, uint8_t addr, uint8_t data);
uint8_t    ym_read(ym_chip_t *chip, uint8_t addr);
void       ym_render(ym_chip_t *chip, int16_t *buf, size_t frames);

/* YM2203専用 */
void ym2203_key_on(ym2203_t *opn, int ch, uint8_t slot_mask);
void ym2203_key_off(ym2203_t *opn, int ch, uint8_t slot_mask);
void ym2203_set_freq(ym2203_t *opn, int ch, uint16_t fnum, uint8_t block);

/* YM2151専用 */
void ym2151_key_on(ym2151_t *opm, int ch, uint8_t slot_mask);
void ym2151_key_off(ym2151_t *opm, int ch, uint8_t slot_mask);
void ym2151_set_freq(ym2151_t *opm, int ch, uint8_t note, uint8_t kc_frac);
