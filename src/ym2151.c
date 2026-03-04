/*
 * YM2151 (OPM) エミュレーション
 * 8チャンネルFM音源 (4-OP), ステレオパン付き
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ym_core.h"

/* ============================================================
 * OPM固有定数
 * ============================================================ */

#define OPM_PHASE_BITS 20
#define OPM_PHASE_MASK ((1 << OPM_PHASE_BITS) - 1)

/* OPMのノート→F-Number変換テーブル */
/* KC (Key Code) 0x00-0x77 = 音階C0..B7 */
static const uint16_t kc_to_kf[128] = {
    /* 簡略化: 実際のOPMはKC+KF(6bit)で制御 */
    /* ここではKCごとの基本F値 */
    0x6A, 0x6E, 0x73, 0x77, 0x7C, 0x80, 0x85, 0x8A,
    0x8F, 0x94, 0x9A, 0x9F, 0xD4, 0xDC, 0xE5, 0xEE,
    0xF8, 0x00, 0x0A, 0x14, 0x1E, 0x28, 0x34, 0x3F,
    0xA8, 0xB1, 0xBB, 0xC5, 0xCF, 0xD9, 0xE4, 0xEF,
    /* ... (64以上は繰り返し/オクターブシフト) */
};

/* ============================================================
 * 外部テーブル参照 (ym2203.cで定義済み)
 * ============================================================ */
extern int16_t sin_table[2048];
extern int     tables_initialized;

static const int dt1_table[8] = { 0, 1, 2, 3, 0, -1, -2, -3 };
/* DT2テーブル: 0,600,781,950 cent */
static const float dt2_ratio[4] = { 1.0f, 1.0354f, 1.0594f, 1.0817f };

static const float mul_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f
};

/* ============================================================
 * 位相インクリメント (OPM用)
 * OPM Clock: 3.58MHz / Fout = KCのF番号で決まる
 * ============================================================ */

static uint32_t opm_calc_phase_inc(double clock, uint32_t sample_rate,
                                    uint8_t kc, uint8_t kf,
                                    uint8_t mul, uint8_t dt1, uint8_t dt2) {
    /* KC = 0x00..0x77: 上位4bitがオクターブ(0-7), 下位4bitが音名(0-11) */
    int octave = (kc >> 4) & 0x07;
    int note   = kc & 0x0F;
    if (note > 11) note = 11;

    /* 基本周波数 (A4=440Hz, KC=0x4A相当) */
    static const double note_freq[12] = {
        261.626, 277.183, 293.665, 311.127, 329.628, 349.228,
        369.994, 391.995, 415.305, 440.000, 466.164, 493.883
    };
    double base = note_freq[note] * (double)(1 << (octave > 3 ? octave - 3 : 0))
                                  / (double)(1 << (octave < 3 ? 3 - octave : 0));
    /* KF(0-63): 1/64半音単位の微調整 */
    base *= pow(2.0, (double)kf / (64.0 * 12.0));
    /* DT1 */
    int dt1v = dt1_table[dt1 & 7];
    base *= pow(2.0, dt1v / (128.0 * 12.0));
    /* DT2 */
    base *= dt2_ratio[dt2 & 3];
    /* マルチプル */
    base *= mul_table[mul & 0xF];

    double inc = base / (double)sample_rate * (double)(1 << OPM_PHASE_BITS);
    return (uint32_t)inc;
}

/* ============================================================
 * EG処理 (ym2203.cと同じロジック)
 * ============================================================ */

static const uint32_t eg_inc_table[64] = {
    0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 2, 2,
    4, 4, 4, 4, 8, 8, 8, 8,
    16, 16, 16, 16, 32, 32, 32, 32,
    64, 64, 64, 64, 128, 128, 128, 128,
    256, 256, 256, 256, 512, 512, 512, 512,
    1024, 1024, 1024, 1024, 2048, 2048, 2048, 2048,
    4096, 4096, 4096, 4096, 8192, 8192, 8192, 8192,
    16384, 16384, 16384, 16384
};

static uint32_t tl_to_linear(uint8_t tl) {
    return (127 - tl) * 512;
}

static void eg_step(op_state_t *op, const ym_operator_t *p, uint8_t key_on) {
    const uint32_t EG_MAX = 0xFFFFu;
    switch (op->state) {
    case ENV_ATTACK:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)p->ar * 2 + p->ks;
            if (rate >= 64) rate = 63;
            op->volume += eg_inc_table[rate];
            if (op->volume >= tl_to_linear(p->tl)) {
                op->volume = tl_to_linear(p->tl);
                op->state  = ENV_DECAY;
            }
        }
        break;
    case ENV_DECAY:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)p->dr * 2 + p->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate]) op->volume -= eg_inc_table[rate];
            else op->volume = 0;
            if (op->volume <= (uint32_t)(15 - p->sl) * 4096) op->state = ENV_SUSTAIN;
        }
        break;
    case ENV_SUSTAIN:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)p->sr * 2 + p->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate]) op->volume -= eg_inc_table[rate];
            else op->volume = 0;
        }
        break;
    case ENV_RELEASE:
        {
            uint32_t rate = (uint32_t)p->rr * 4 + 2 + p->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate]) op->volume -= eg_inc_table[rate];
            else { op->volume = 0; op->state = ENV_OFF; }
        }
        break;
    case ENV_OFF:
    default:
        if (key_on) { op->state = ENV_ATTACK; op->volume = 0; op->phase = 0; }
        break;
    }
    if (op->volume > EG_MAX) op->volume = EG_MAX;
}

static int32_t op_output(op_state_t *op, int32_t mod) {
    if (op->state == ENV_OFF) return 0;
    op->phase = (op->phase + op->phase_inc) & OPM_PHASE_MASK;
    uint32_t phase = (op->phase + (mod >> 1)) & OPM_PHASE_MASK;
    int idx = (phase >> (OPM_PHASE_BITS - 11)) & 2047;
    int32_t wave = sin_table[idx];
    return (int32_t)((int64_t)wave * (int64_t)op->volume / 65536);
}

static int32_t algo_output(ch_state_t *cs, const ym_channel_t *ch) {
    int32_t m1, m2, c1, c2;
    int32_t fb = cs->op[0].out;
    int32_t fb_mod = (fb * (ch->feedback ? (1 << (ch->feedback - 1)) : 0)) >> 3;

    switch (ch->algorithm & 7) {
    case 0:
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], m1);
        c1 = op_output(&cs->op[2], m2);
        c2 = op_output(&cs->op[3], c1);
        cs->op[0].out = m1; return c2;
    case 1:
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], m1 + m2);
        c2 = op_output(&cs->op[3], c1);
        cs->op[0].out = m1; return c2;
    case 2:
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], m2);
        c2 = op_output(&cs->op[3], m1 + c1);
        cs->op[0].out = m1; return c2;
    case 3:
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], m1);
        c1 = op_output(&cs->op[2], m1);
        c2 = op_output(&cs->op[3], m2 + c1);
        cs->op[0].out = m1; return c2;
    case 4:
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], 0);
        c2 = op_output(&cs->op[3], m2);
        cs->op[0].out = m1; return c1 + c2;
    case 5:
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], m1);
        c2 = op_output(&cs->op[3], m1);
        cs->op[0].out = m1; return c1 + m2 + c2;
    case 6:
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], 0);
        c2 = op_output(&cs->op[3], 0);
        cs->op[0].out = m1; return c1 + m2 + c2;
    case 7:
    default:
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], 0);
        c2 = op_output(&cs->op[3], 0);
        cs->op[0].out = m1; return m1 + m2 + c1 + c2;
    }
}

/* ============================================================
 * レジスタ書き込み (OPM)
 * OPMはポートが1つのみ (アドレス0x00-0xFF)
 * ============================================================ */

static void opm_recalc_op(ym2151_t *opm, int ch, int op) {
    /* OPMのレジスタマップ: アドレス = base + ch + op*8 */
    static const int op_reg_base[4] = { 0, 8, 16, 24 };

    ym_operator_t *param = &opm->ch[ch].op[op];
    op_state_t    *st    = &opm->ch_state[ch].op[op];

    int b = op_reg_base[op] + ch;
    param->dt1 = (opm->regs[0x40 + b] >> 4) & 0x07;
    param->mul = opm->regs[0x40 + b] & 0x0F;
    param->tl  = opm->regs[0x60 + b] & 0x7F;
    param->ks  = (opm->regs[0x80 + b] >> 6) & 0x03;
    param->ar  = opm->regs[0x80 + b] & 0x1F;
    param->am  = (opm->regs[0xA0 + b] >> 7) & 0x01;
    param->dt2 = (opm->regs[0xA0 + b] >> 6) & 0x03;
    param->dr  = opm->regs[0xA0 + b] & 0x1F;
    param->sr  = opm->regs[0xC0 + b] & 0x1F;
    param->sl  = (opm->regs[0xE0 + b] >> 4) & 0x0F;
    param->rr  = opm->regs[0xE0 + b] & 0x0F;

    /* 周波数更新 */
    uint8_t kc = opm->regs[0x28 + ch];
    uint8_t kf = opm->regs[0x30 + ch] >> 2;
    st->phase_inc = opm_calc_phase_inc(opm->clock, opm->sample_rate,
                                       kc, kf, param->mul,
                                       param->dt1, param->dt2);
}

void ym2151_write_reg(ym2151_t *opm, uint8_t addr, uint8_t data) {
    opm->regs[addr] = data;

    if (addr >= 0x20 && addr <= 0x27) {
        /* チャンネルコントロール: pan, FB, CON */
        int ch = addr - 0x20;
        opm->ch[ch].pan       = (data >> 6) & 0x03;
        opm->ch[ch].ams       = (data >> 4) & 0x03;
        opm->ch[ch].pms       = data & 0x07;
    } else if (addr >= 0x28 && addr <= 0x2F) {
        /* Key Code */
        int ch = addr - 0x28;
        for (int op = 0; op < 4; op++)
            opm_recalc_op(opm, ch, op);
    } else if (addr >= 0x38 && addr <= 0x3F) {
        /* Noise + LFO */
        if (addr == 0x38) {
            opm->lfo_freq = data;
        } else if (addr == 0x1B) {
            opm->lfo_waveform = data & 0x03;
        }
    } else if (addr == 0x08) {
        /* Key On/Off */
        int ch        = data & 0x07;
        uint8_t slots = (data >> 3) & 0x0F;
        opm->ch_state[ch].key_on = slots;
        for (int op = 0; op < 4; op++) {
            if (slots & (1 << op)) {
                opm->ch_state[ch].op[op].state  = ENV_ATTACK;
                opm->ch_state[ch].op[op].volume = 0;
                opm->ch_state[ch].op[op].phase  = 0;
            } else if (opm->ch_state[ch].op[op].state != ENV_OFF) {
                opm->ch_state[ch].op[op].state = ENV_RELEASE;
            }
        }
    } else if (addr >= 0x40 && addr <= 0xFF) {
        /* OP パラメータ */
        int op_block = (addr - 0x40) >> 5;
        int ch = (addr - 0x40) & 0x07;
        if (ch < 8 && op_block < 4)
            opm_recalc_op(opm, ch, op_block);
    } else if (addr >= 0x60 && addr <= 0x7F) {
        int op_block = (addr - 0x60) >> 3;
        int ch = (addr - 0x60) & 0x07;
        if (ch < 8 && op_block < 4)
            opm_recalc_op(opm, ch, op_block);
    }
}

void ym2151_key_on(ym2151_t *opm, int ch, uint8_t slot_mask) {
    if (ch < 0 || ch >= 8) return;
    opm->ch_state[ch].key_on = slot_mask;
    for (int op = 0; op < 4; op++) {
        if (slot_mask & (1 << op)) {
            opm->ch_state[ch].op[op].state  = ENV_ATTACK;
            opm->ch_state[ch].op[op].volume = 0;
            opm->ch_state[ch].op[op].phase  = 0;
        }
    }
}

void ym2151_key_off(ym2151_t *opm, int ch, uint8_t slot_mask) {
    if (ch < 0 || ch >= 8) return;
    opm->ch_state[ch].key_on &= ~slot_mask;
    for (int op = 0; op < 4; op++) {
        if ((slot_mask & (1 << op)) &&
            opm->ch_state[ch].op[op].state != ENV_OFF) {
            opm->ch_state[ch].op[op].state = ENV_RELEASE;
        }
    }
}

void ym2151_set_freq(ym2151_t *opm, int ch, uint8_t kc, uint8_t kf) {
    if (ch < 0 || ch >= 8) return;
    opm->regs[0x28 + ch] = kc;
    opm->regs[0x30 + ch] = kf << 2;
    for (int op = 0; op < 4; op++)
        opm_recalc_op(opm, ch, op);
}

/* ============================================================
 * レンダリング
 * ============================================================ */

void ym2151_render(ym2151_t *opm, int16_t *buf, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        int32_t out_l = 0, out_r = 0;

        for (int ch = 0; ch < 8; ch++) {
            ch_state_t *cs = &opm->ch_state[ch];
            for (int op = 0; op < 4; op++)
                eg_step(&cs->op[op], &opm->ch[ch].op[op], cs->key_on & (1 << op));

            int32_t fm = algo_output(cs, &opm->ch[ch]);
            uint8_t pan = opm->ch[ch].pan;
            if (pan & 2) out_l += fm;
            if (pan & 1) out_r += fm;
        }

        out_l >>= 4;
        out_r >>= 4;
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        buf[i * 2 + 0] = (int16_t)out_l;
        buf[i * 2 + 1] = (int16_t)out_r;
    }
}

/* ============================================================
 * 初期化 / リセット
 * ============================================================ */

void ym2151_init(ym2151_t *opm, double clock, uint32_t sample_rate) {
    memset(opm, 0, sizeof(*opm));
    opm->clock       = clock;
    opm->sample_rate = sample_rate;
    /* デフォルトパン: 両チャンネル出力 */
    for (int ch = 0; ch < 8; ch++)
        opm->ch[ch].pan = 3;
}

void ym2151_reset(ym2151_t *opm) {
    double clock = opm->clock;
    uint32_t sr  = opm->sample_rate;
    ym2151_init(opm, clock, sr);
}
