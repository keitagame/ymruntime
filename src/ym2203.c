/*
 * YM2203 (OPN) エミュレーション
 * 3チャンネルFM音源 + 3チャンネルSSG(AY-3-8910互換)
 */

#define _GNU_SOURCE
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ym_core.h"

/* ============================================================
 * 定数・テーブル
 * ============================================================ */

#define OPN_EG_BITS    23
#define OPN_EG_SHIFT   (OPN_EG_BITS - 10)
#define OPN_PHASE_BITS 20
#define OPN_PHASE_MASK ((1 << OPN_PHASE_BITS) - 1)
#define OPN_OUTPUT_BITS 14

/* EG攻撃/減衰レート → サンプル数テーブル (44100Hzベース) */
static const uint32_t eg_inc_table[64] = {
    /* 0-3: 変化なし */
    0, 0, 0, 0,
    /* 4-11: 非常に遅い */
    1, 1, 1, 1, 2, 2, 2, 2,
    /* 12-19: 遅い */
    4, 4, 4, 4, 8, 8, 8, 8,
    /* 20-27: 中 */
    16, 16, 16, 16, 32, 32, 32, 32,
    /* 28-35: 速い */
    64, 64, 64, 64, 128, 128, 128, 128,
    /* 36-43: より速い */
    256, 256, 256, 256, 512, 512, 512, 512,
    /* 44-51: 速い */
    1024, 1024, 1024, 1024, 2048, 2048, 2048, 2048,
    /* 52-59 */
    4096, 4096, 4096, 4096, 8192, 8192, 8192, 8192,
    /* 60-63: 最速 */
    16384, 16384, 16384, 16384
};

/* サイン波テーブル (2048エントリ, 固定小数点) - ym2151.cからも参照される */
int16_t sin_table[2048];
static int      tables_initialized = 0;

/* デチューンテーブル */
static const int dt_table[8] = { 0, 1, 2, 3, 0, -1, -2, -3 };

/* マルチプルテーブル */
static const float mul_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f
};

/* ============================================================
 * テーブル初期化
 * ============================================================ */

static void init_tables(void) {
    if (tables_initialized) return;
    for (int i = 0; i < 2048; i++) {
        double angle = (double)i / 2048.0 * 2.0 * M_PI;
        sin_table[i] = (int16_t)(sin(angle) * 32767.0);
    }
    tables_initialized = 1;
}

/* ============================================================
 * 位相インクリメント計算
 * ============================================================ */

static uint32_t calc_phase_inc(double clock, uint32_t sample_rate,
                                uint16_t fnum, uint8_t block,
                                uint8_t mul, int8_t dt) {
    /* OPN: F-Number = Fout * 2^(20-block) / (clock/144) */
    double base_freq = (double)fnum * (clock / 144.0)
                       / (double)(1u << (20 - block));
    /* デチューン適用 (簡略化) */
    base_freq += dt * (base_freq * 0.001);
    /* マルチプル適用 */
    base_freq *= mul_table[mul & 0xF];
    /* 位相インクリメント */
    double inc = base_freq / (double)sample_rate * (double)(1 << OPN_PHASE_BITS);
    return (uint32_t)inc;
}

/* ============================================================
 * エンベロープ処理
 * ============================================================ */

static uint32_t tl_to_linear(uint8_t tl) {
    /* TL 0-127 → 線形ボリューム (固定小数点) */
    /* 0が最大音量, 127が最小 */
    return (127 - tl) * 512;  /* 0..65024 */
}

static void eg_step(op_state_t *op, const ym_operator_t *param,
                    uint8_t key_on) {
    const uint32_t EG_MAX = 0xFFFFu;

    switch (op->state) {
    case ENV_ATTACK:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)param->ar * 2 + param->ks;
            if (rate >= 64) rate = 63;
            op->volume += eg_inc_table[rate];
            if (op->volume >= tl_to_linear(param->tl)) {
                op->volume = tl_to_linear(param->tl);
                op->state  = ENV_DECAY;
            }
        }
        break;

    case ENV_DECAY:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)param->dr * 2 + param->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate])
                op->volume -= eg_inc_table[rate];
            else
                op->volume = 0;
            uint32_t sl_vol = (uint32_t)(15 - param->sl) * 4096;
            if (op->volume <= sl_vol)
                op->state = ENV_SUSTAIN;
        }
        break;

    case ENV_SUSTAIN:
        if (!key_on) { op->state = ENV_RELEASE; break; }
        {
            uint32_t rate = (uint32_t)param->sr * 2 + param->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate])
                op->volume -= eg_inc_table[rate];
            else
                op->volume = 0;
        }
        break;

    case ENV_RELEASE:
        {
            uint32_t rate = (uint32_t)param->rr * 4 + 2 + param->ks;
            if (rate >= 64) rate = 63;
            if (op->volume > eg_inc_table[rate])
                op->volume -= eg_inc_table[rate];
            else {
                op->volume = 0;
                op->state  = ENV_OFF;
            }
        }
        break;

    case ENV_OFF:
    default:
        if (key_on) {
            op->state  = ENV_ATTACK;
            op->volume = 0;
        }
        break;
    }
    if (op->volume > EG_MAX) op->volume = EG_MAX;
}

/* ============================================================
 * OPシンセシス (1サンプル)
 * ============================================================ */

static int32_t op_output(op_state_t *op, int32_t mod_input) {
    if (op->state == ENV_OFF) return 0;
    /* 位相を進める */
    op->phase = (op->phase + op->phase_inc) & OPN_PHASE_MASK;
    /* モジュレーション適用 */
    uint32_t phase = (op->phase + (mod_input >> 1)) & OPN_PHASE_MASK;
    /* サイン波テーブル参照 */
    int idx = (phase >> (OPN_PHASE_BITS - 11)) & 2047;
    int32_t wave = sin_table[idx];
    /* エンベロープ適用 */
    return (int32_t)((int64_t)wave * (int64_t)op->volume / 65536);
}

/* ============================================================
 * アルゴリズム別出力計算 (4-OPアルゴリズム0-7)
 * ============================================================ */

static int32_t algo_output(ch_state_t *cs, const ym_channel_t *ch) {
    int32_t m1, m2, c1, c2;
    int32_t fb = cs->op[0].out;  /* フィードバック */
    int32_t fb_mod = (fb * (ch->feedback ? (1 << (ch->feedback - 1)) : 0)) >> 3;

    switch (ch->algorithm & 7) {
    case 0: /* M1→M2→C1→C2 直列 */
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], m1);
        c1 = op_output(&cs->op[2], m2);
        c2 = op_output(&cs->op[3], c1);
        cs->op[0].out = m1;
        return c2;

    case 1: /* (M1+M2)→C1→C2 */
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], m1 + m2);
        c2 = op_output(&cs->op[3], c1);
        cs->op[0].out = m1;
        return c2;

    case 2: /* M1→(M2→C1)+M1→C2 */
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], m2);
        c2 = op_output(&cs->op[3], m1 + c1);
        cs->op[0].out = m1;
        return c2;

    case 3: /* M1→M2→(C1+M1)→C2 */
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], m1);
        c1 = op_output(&cs->op[2], m1);
        c2 = op_output(&cs->op[3], m2 + c1);
        cs->op[0].out = m1;
        return c2;

    case 4: /* M1→C1 + M2→C2 2並列 */
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], 0);
        c2 = op_output(&cs->op[3], m2);
        cs->op[0].out = m1;
        return c1 + c2;

    case 5: /* M1→(C1+M2+C2) */
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], m1);
        c2 = op_output(&cs->op[3], m1);
        cs->op[0].out = m1;
        return c1 + m2 + c2;

    case 6: /* M1→C1 + M2 + C2 */
        m1 = op_output(&cs->op[0], fb_mod);
        c1 = op_output(&cs->op[2], m1);
        m2 = op_output(&cs->op[1], 0);
        c2 = op_output(&cs->op[3], 0);
        cs->op[0].out = m1;
        return c1 + m2 + c2;

    case 7: /* M1+M2+C1+C2 全並列 */
        m1 = op_output(&cs->op[0], fb_mod);
        m2 = op_output(&cs->op[1], 0);
        c1 = op_output(&cs->op[2], 0);
        c2 = op_output(&cs->op[3], 0);
        cs->op[0].out = m1;
        return m1 + m2 + c1 + c2;
    }
    return 0;
}

/* ============================================================
 * SSGシンセシス
 * ============================================================ */

static int32_t ssg_output(ym2203_t *opn, int ch) {
    ym_ssg_channel_t *param = &opn->ssg[ch];
    ssg_state_t      *st    = &opn->ssg_state[ch];

    if (param->period == 0) return 0;

    /* ノイズ生成 (17-bit LFSR) */
    if (st->noise_reg == 0) st->noise_reg = 1;
    /* 音程発振 */
    uint32_t inc = (uint32_t)((double)opn->clock / (double)opn->sample_rate
                               / 16.0 / (double)(param->period ? param->period : 1)
                               * (double)(1u << 16));
    st->phase += inc;
    int tone = (st->phase >> 16) & 1;

    /* ノイズ発振 */
    uint32_t noise_inc = (uint32_t)((double)opn->clock / (double)opn->sample_rate
                                     / 16.0 / (double)(opn->regs[0x06] ? opn->regs[0x06] : 1)
                                     * (double)(1u << 16));
    st->noise_phase += noise_inc;
    if (st->noise_phase >> 16) {
        st->noise_phase &= 0xFFFF;
        /* Galois LFSR: x^17 + x^14 + 1 */
        int bit = (st->noise_reg ^ (st->noise_reg >> 3)) & 1;
        st->noise_reg = (st->noise_reg >> 1) | (bit << 16);
    }
    int noise = st->noise_reg & 1;

    /* ミックス (レジスタ0x07のトーン/ノイズ有効フラグ) */
    uint8_t mixer = opn->regs[0x07];
    int tone_en  = !((mixer >> ch) & 1);
    int noise_en = !((mixer >> (ch + 3)) & 1);
    int out = (tone_en ? tone : 1) & (noise_en ? noise : 1);

    /* ボリューム */
    int32_t vol = param->volume & 0x0F;
    static const int16_t vol_table[16] = {
        0, 256, 362, 512, 724, 1024, 1448, 2048,
        2896, 4096, 5792, 8192, 11585, 16384, 23170, 32767
    };
    return out ? vol_table[vol] : 0;
}

/* ============================================================
 * レジスタ書き込み処理
 * ============================================================ */

static void opn_update_op_params(ym2203_t *opn, int ch, int op_idx) {
    /* OPNのレジスタアドレス計算 */
    /* ch=0,1,2 op=0(M1),1(M2),2(C1),3(C2) */
    static const int op_offset[4] = { 0, 8, 4, 12 };
    int base = 0x30 + op_offset[op_idx] + ch;

    ym_operator_t *op = &opn->ch[ch].op[op_idx];
    op_state_t    *st = &opn->ch_state[ch].op[op_idx];

    op->mul = opn->regs[base] & 0x0F;
    op->dt1 = (opn->regs[base] >> 4) & 0x07;
    op->tl  = opn->regs[base + 0x10] & 0x7F;
    op->ks  = (opn->regs[base + 0x20] >> 6) & 0x03;
    op->ar  = opn->regs[base + 0x20] & 0x1F;
    op->am  = (opn->regs[base + 0x30] >> 7) & 0x01;
    op->dr  = opn->regs[base + 0x30] & 0x1F;
    op->sr  = opn->regs[base + 0x40] & 0x1F;
    op->sl  = (opn->regs[base + 0x50] >> 4) & 0x0F;
    op->rr  = opn->regs[base + 0x50] & 0x0F;

    /* 位相インクリメント再計算 */
    ch_state_t *cs = &opn->ch_state[ch];
    int8_t dt = dt_table[op->dt1 & 7];
    st->phase_inc = calc_phase_inc(opn->clock, opn->sample_rate,
                                   (uint16_t)(cs->freq), cs->block,
                                   op->mul, dt);
}

void ym2203_write_reg(ym2203_t *opn, uint8_t addr, uint8_t data) {
    opn->regs[addr] = data;

    if (addr >= 0x30 && addr <= 0x9F) {
        /* OPパラメータ */
        int ch     = (addr & 0x03);
        if (ch >= 3) return;
        /* どのOPかはアドレス帯で判断 */
        for (int op = 0; op < 4; op++)
            opn_update_op_params(opn, ch, op);
    } else if (addr >= 0xA0 && addr <= 0xA2) {
        /* F-Number LSB */
        int ch = addr - 0xA0;
        opn->ch_state[ch].freq = data |
            ((opn->regs[0xA4 + ch] & 0x07) << 8);
        opn->ch_state[ch].block = (opn->regs[0xA4 + ch] >> 3) & 0x07;
        for (int op = 0; op < 4; op++)
            opn_update_op_params(opn, ch, op);
    } else if (addr >= 0xA4 && addr <= 0xA6) {
        /* F-Number MSB + Block */
        /* 実際のラッチはA0-A2書き込み時 */
    } else if (addr == 0x28) {
        /* Key On/Off */
        int ch        = data & 0x03;
        uint8_t slots = (data >> 4) & 0x0F;
        if (ch < 3) {
            opn->ch_state[ch].key_on = slots;
            /* Key onの場合はエンベロープをリセット */
            for (int op = 0; op < 4; op++) {
                if (slots & (1 << op)) {
                    opn->ch_state[ch].op[op].state  = ENV_ATTACK;
                    opn->ch_state[ch].op[op].volume = 0;
                    opn->ch_state[ch].op[op].phase  = 0;
                } else if (opn->ch_state[ch].op[op].state != ENV_OFF) {
                    opn->ch_state[ch].op[op].state = ENV_RELEASE;
                }
            }
        }
    } else if (addr == 0xB0 || addr == 0xB1 || addr == 0xB2) {
        /* Algorithm + Feedback */
        int ch = addr - 0xB0;
        opn->ch[ch].algorithm = data & 0x07;
        opn->ch[ch].feedback  = (data >> 3) & 0x07;
    }
    /* SSGレジスタ */
    else if (addr <= 0x0E) {
        int ssg_ch = addr / 2;
        if (ssg_ch >= 3) return;
        if (addr == 0x00 || addr == 0x02 || addr == 0x04) {
            opn->ssg[ssg_ch].period = (opn->ssg[ssg_ch].period & 0xF00) | data;
        } else if (addr == 0x01 || addr == 0x03 || addr == 0x05) {
            opn->ssg[ssg_ch].period = (opn->ssg[ssg_ch].period & 0x0FF) | ((data & 0x0F) << 8);
        } else if (addr >= 0x08 && addr <= 0x0A) {
            opn->ssg[addr - 0x08].volume = data & 0x1F;
        }
    }
}

void ym2203_key_on(ym2203_t *opn, int ch, uint8_t slot_mask) {
    if (ch < 0 || ch >= 3) return;
    opn->ch_state[ch].key_on = slot_mask;
    for (int op = 0; op < 4; op++) {
        if (slot_mask & (1 << op)) {
            opn->ch_state[ch].op[op].state  = ENV_ATTACK;
            opn->ch_state[ch].op[op].volume = 0;
            opn->ch_state[ch].op[op].phase  = 0;
        }
    }
}

void ym2203_key_off(ym2203_t *opn, int ch, uint8_t slot_mask) {
    if (ch < 0 || ch >= 3) return;
    opn->ch_state[ch].key_on &= ~slot_mask;
    for (int op = 0; op < 4; op++) {
        if ((slot_mask & (1 << op)) &&
            opn->ch_state[ch].op[op].state != ENV_OFF) {
            opn->ch_state[ch].op[op].state = ENV_RELEASE;
        }
    }
}

void ym2203_set_freq(ym2203_t *opn, int ch, uint16_t fnum, uint8_t block) {
    if (ch < 0 || ch >= 3) return;
    opn->ch_state[ch].freq  = fnum;
    opn->ch_state[ch].block = block;
    for (int op = 0; op < 4; op++)
        opn_update_op_params(opn, ch, op);
}

/* ============================================================
 * レンダリング (1フレーム分)
 * ============================================================ */

void ym2203_render(ym2203_t *opn, int16_t *buf, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        int32_t out_l = 0, out_r = 0;

        /* FM 3チャンネル */
        for (int ch = 0; ch < 3; ch++) {
            ch_state_t *cs = &opn->ch_state[ch];
            /* EGステップ */
            for (int op = 0; op < 4; op++)
                eg_step(&cs->op[op], &opn->ch[ch].op[op], cs->key_on & (1 << op));
            int32_t fm = algo_output(cs, &opn->ch[ch]);
            out_l += fm;
            out_r += fm;
        }

        /* SSG 3チャンネル */
        for (int ch = 0; ch < 3; ch++) {
            int32_t ssg = ssg_output(opn, ch);
            out_l += ssg;
            out_r += ssg;
        }

        /* クリップ */
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

void ym2203_init(ym2203_t *opn, double clock, uint32_t sample_rate) {
    init_tables();
    memset(opn, 0, sizeof(*opn));
    opn->clock       = clock;
    opn->sample_rate = sample_rate;
    /* SSGノイズLFSRを1で初期化 */
    for (int i = 0; i < 3; i++)
        opn->ssg_state[i].noise_reg = 1;
}

void ym2203_reset(ym2203_t *opn) {
    double clock = opn->clock;
    uint32_t sr  = opn->sample_rate;
    ym2203_init(opn, clock, sr);
}
