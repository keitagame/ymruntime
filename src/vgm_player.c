/*
 * vgm_player.c - VGMファイルプレーヤー実装
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "vgm_player.h"
#include "ym_core.h"

#define VGM_SAMPLE_RATE 44100

int vgm_player_init(vgm_player_t *player) {
    memset(player, 0, sizeof(*player));
    return 0;
}

void vgm_player_destroy(vgm_player_t *player) {
    if (player->data) {
        free(player->data);
        player->data = NULL;
    }
}

int vgm_player_load(vgm_player_t *player, const uint8_t *data,
                     size_t len, int loop) {
    if (!data || len < sizeof(vgm_header_t)) return -1;

    const vgm_header_t *hdr = (const vgm_header_t *)data;
    if (hdr->ident != VGM_IDENT) {
        fprintf(stderr, "vgm_player: invalid ident\n");
        return -1;
    }

    /* データをコピー */
    free(player->data);
    player->data = malloc(len);
    if (!player->data) return -1;
    memcpy(player->data, data, len);
    player->data_len = len;

    /* データ開始オフセット計算 */
    uint32_t version = hdr->version;
    size_t data_offset;
    if (version >= 0x150 && hdr->vgm_data_offset != 0) {
        data_offset = 0x34 + hdr->vgm_data_offset;
    } else {
        data_offset = 0x40;  /* VGM 1.00のデフォルト */
    }
    if (data_offset >= len) data_offset = 0x40;

    player->cmd_start = player->data + data_offset;
    player->cmd_end   = player->data + len;
    player->cmd_ptr   = player->cmd_start;

    /* ループ設定 */
    player->loop = (uint8_t)loop;
    if (hdr->loop_offset && hdr->loop_samples) {
        player->loop_ptr = player->data + 0x1C + hdr->loop_offset;
        if (player->loop_ptr >= player->cmd_end)
            player->loop_ptr = player->cmd_start;
    } else {
        player->loop_ptr = player->cmd_start;
    }

    player->total_samples    = hdr->total_samples;
    player->position_samples = 0;
    player->wait_samples     = 0;
    player->loop_count       = 0;
    player->state            = VGM_STATE_STOPPED;

    fprintf(stderr, "vgm_player: loaded %zu bytes, version %04X, "
                    "%u samples (~%u ms)\n",
            len, version, hdr->total_samples,
            hdr->total_samples * 1000 / VGM_SAMPLE_RATE);
    return 0;
}

int vgm_player_play(vgm_player_t *player) {
    if (!player->data) return -1;
    if (player->state == VGM_STATE_STOPPED) {
        player->cmd_ptr          = player->cmd_start;
        player->position_samples = 0;
        player->wait_samples     = 0;
    }
    player->state = VGM_STATE_PLAYING;
    return 0;
}

void vgm_player_pause(vgm_player_t *player) {
    if (player->state == VGM_STATE_PLAYING)
        player->state = VGM_STATE_PAUSED;
    else if (player->state == VGM_STATE_PAUSED)
        player->state = VGM_STATE_PLAYING;
}

void vgm_player_stop(vgm_player_t *player) {
    player->state = VGM_STATE_STOPPED;
    if (player->chip_2203) ym_reset(player->chip_2203);
    if (player->chip_2151) ym_reset(player->chip_2151);
}

int vgm_player_seek(vgm_player_t *player, uint32_t sample_pos) {
    /* 簡略実装: 先頭から再生して指定サンプルまでスキップ */
    /* 本格実装ではサンプル位置をインデックスすべきだが、
       VGMは逐次解析なので先頭から実行する */
    if (!player->data) return -1;
    player->cmd_ptr          = player->cmd_start;
    player->position_samples = 0;
    player->wait_samples     = 0;
    /* チップをリセット */
    if (player->chip_2203) ym_reset(player->chip_2203);
    if (player->chip_2151) ym_reset(player->chip_2151);
    /* 目標サンプルまで高速実行 (オーディオ出力なし) */
    while (player->position_samples < sample_pos &&
           player->cmd_ptr < player->cmd_end) {
        vgm_player_step(player, 1);
    }
    return 0;
}

/* ============================================================
 * VGMコマンド処理
 * ============================================================ */

static void process_vgm_commands(vgm_player_t *player, uint32_t max_samples) {
    while (player->cmd_ptr < player->cmd_end) {
        if (player->wait_samples >= max_samples) {
            player->wait_samples -= max_samples;
            return;
        }
        max_samples -= player->wait_samples;
        player->wait_samples = 0;

        uint8_t cmd = *player->cmd_ptr++;

        switch (cmd) {
        case VGM_CMD_YM2151:
            if (player->cmd_ptr + 1 < player->cmd_end) {
                uint8_t addr = player->cmd_ptr[0];
                uint8_t data = player->cmd_ptr[1];
                player->cmd_ptr += 2;
                if (player->chip_2151)
                    ym_write(player->chip_2151, addr, data);
            }
            break;

        case VGM_CMD_YM2203:
            if (player->cmd_ptr + 1 < player->cmd_end) {
                uint8_t addr = player->cmd_ptr[0];
                uint8_t data = player->cmd_ptr[1];
                player->cmd_ptr += 2;
                if (player->chip_2203)
                    ym_write(player->chip_2203, addr, data);
            }
            break;

        case VGM_CMD_WAIT_N:
            if (player->cmd_ptr + 1 < player->cmd_end) {
                uint16_t n = (uint16_t)(player->cmd_ptr[0] |
                                        (player->cmd_ptr[1] << 8));
                player->cmd_ptr += 2;
                player->wait_samples += n;
            }
            break;

        case VGM_CMD_WAIT_735:
            player->wait_samples += 735;
            break;

        case VGM_CMD_WAIT_882:
            player->wait_samples += 882;
            break;

        case VGM_CMD_END:
            if (player->loop && player->loop_ptr) {
                player->cmd_ptr = player->loop_ptr;
                player->loop_count++;
                fprintf(stderr, "vgm_player: loop #%u\n", player->loop_count);
            } else {
                player->state = VGM_STATE_STOPPED;
                if (player->on_finished)
                    player->on_finished(player->userdata);
                return;
            }
            break;

        case VGM_CMD_DATA_BLK:
            /* データブロックをスキップ */
            if (player->cmd_ptr + 5 < player->cmd_end) {
                player->cmd_ptr++; /* type */
                uint32_t sz = (uint32_t)(player->cmd_ptr[0] |
                    (player->cmd_ptr[1] << 8) |
                    (player->cmd_ptr[2] << 16) |
                    (player->cmd_ptr[3] << 24));
                player->cmd_ptr += 4 + sz;
            }
            break;

        default:
            /* 1バイトウェイト (0x70-0x7F) */
            if (cmd >= 0x70 && cmd <= 0x7F) {
                player->wait_samples += (cmd & 0x0F) + 1;
            }
            /* 2バイトコマンド (既知のものはスキップ) */
            else if (cmd == 0x30 || cmd == 0x3F ||
                     (cmd >= 0x50 && cmd <= 0x5F)) {
                player->cmd_ptr += 2;
            }
            /* それ以外: 1バイトスキップ */
            break;
        }
    }
    /* データ終端 */
    player->state = VGM_STATE_STOPPED;
    if (player->on_finished)
        player->on_finished(player->userdata);
}

void vgm_player_step(vgm_player_t *player, size_t frames) {
    if (player->state != VGM_STATE_PLAYING) return;
    process_vgm_commands(player, (uint32_t)frames);
    player->position_samples += (uint32_t)frames;
}

uint32_t vgm_player_position_ms(const vgm_player_t *player) {
    return player->position_samples * 1000 / VGM_SAMPLE_RATE;
}

uint32_t vgm_player_total_ms(const vgm_player_t *player) {
    return player->total_samples * 1000 / VGM_SAMPLE_RATE;
}
