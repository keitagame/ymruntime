/*
 * ym_core.c - 統合チップラッパーとIPCプロトコルヘルパー
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include "ym_core.h"
#include "ym_ipc.h"

/* 外部関数プロトタイプ */
void ym2203_init(ym2203_t *opn, double clock, uint32_t sample_rate);
void ym2203_reset(ym2203_t *opn);
void ym2203_write_reg(ym2203_t *opn, uint8_t addr, uint8_t data);
void ym2203_render(ym2203_t *opn, int16_t *buf, size_t frames);

void ym2151_init(ym2151_t *opm, double clock, uint32_t sample_rate);
void ym2151_reset(ym2151_t *opm);
void ym2151_write_reg(ym2151_t *opm, uint8_t addr, uint8_t data);
void ym2151_render(ym2151_t *opm, int16_t *buf, size_t frames);

/* ============================================================
 * チップ統合 API
 * ============================================================ */

ym_chip_t *ym_create(ym_chip_type_t type, double clock, uint32_t sample_rate) {
    ym_chip_t *chip = calloc(1, sizeof(*chip));
    if (!chip) return NULL;
    chip->type = type;
    switch (type) {
    case YM_CHIP_YM2203:
        ym2203_init(&chip->opn, clock, sample_rate);
        break;
    case YM_CHIP_YM2151:
        ym2151_init(&chip->opm, clock, sample_rate);
        break;
    default:
        free(chip);
        return NULL;
    }
    return chip;
}

void ym_destroy(ym_chip_t *chip) {
    if (chip) free(chip);
}

void ym_reset(ym_chip_t *chip) {
    if (!chip) return;
    switch (chip->type) {
    case YM_CHIP_YM2203: ym2203_reset(&chip->opn); break;
    case YM_CHIP_YM2151: ym2151_reset(&chip->opm); break;
    default: break;
    }
}

void ym_write(ym_chip_t *chip, uint8_t addr, uint8_t data) {
    if (!chip) return;
    switch (chip->type) {
    case YM_CHIP_YM2203: ym2203_write_reg(&chip->opn, addr, data); break;
    case YM_CHIP_YM2151: ym2151_write_reg(&chip->opm, addr, data); break;
    default: break;
    }
}

uint8_t ym_read(ym_chip_t *chip, uint8_t addr) {
    if (!chip) return 0;
    switch (chip->type) {
    case YM_CHIP_YM2203: return chip->opn.regs[addr];
    case YM_CHIP_YM2151: return chip->opm.regs[addr];
    default:             return 0;
    }
}

void ym_render(ym_chip_t *chip, int16_t *buf, size_t frames) {
    if (!chip || !buf) return;
    switch (chip->type) {
    case YM_CHIP_YM2203: ym2203_render(&chip->opn, buf, frames); break;
    case YM_CHIP_YM2151: ym2151_render(&chip->opm, buf, frames); break;
    default:             memset(buf, 0, frames * 4);               break;
    }
}

/* ============================================================
 * IPC パケット送受信
 * ============================================================ */

int ym_send_packet(int fd, ym_command_t cmd, uint16_t seq,
                   const void *payload, size_t payload_len) {
    ym_pkt_header_t hdr = {
        .magic       = YM_PROTO_MAGIC,
        .version     = YM_PROTO_VERSION,
        .command     = (uint8_t)cmd,
        .seq         = seq,
        .payload_len = (uint32_t)payload_len,
    };

    /* ヘッダ送信 */
    ssize_t n = write(fd, &hdr, sizeof(hdr));
    if (n != sizeof(hdr)) return -1;

    /* ペイロード送信 */
    if (payload_len > 0 && payload) {
        const uint8_t *p = (const uint8_t *)payload;
        size_t sent = 0;
        while (sent < payload_len) {
            n = write(fd, p + sent, payload_len - sent);
            if (n <= 0) return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

int ym_recv_packet(int fd, ym_pkt_header_t *hdr,
                   void *payload_buf, size_t buf_size) {
    /* ヘッダ受信 */
    ssize_t n = read(fd, hdr, sizeof(*hdr));
    if (n <= 0) return -1;
    if ((size_t)n != sizeof(*hdr)) return -1;

    /* マジック確認 */
    if (hdr->magic != YM_PROTO_MAGIC) return -1;

    /* ペイロード受信 */
    if (hdr->payload_len > 0) {
        if (hdr->payload_len > buf_size) return -1;
        uint8_t *p = (uint8_t *)payload_buf;
        size_t received = 0;
        while (received < hdr->payload_len) {
            n = read(fd, p + received, hdr->payload_len - received);
            if (n <= 0) return -1;
            received += (size_t)n;
        }
    }
    return 0;
}

/* ============================================================
 * エラー文字列
 * ============================================================ */

const char *ym_strerror(ym_error_t err) {
    switch (err) {
    case YM_OK:              return "Success";
    case YM_ERR_UNKNOWN:     return "Unknown error";
    case YM_ERR_INVALID_CMD: return "Invalid command";
    case YM_ERR_NO_DEVICE:   return "Device not found";
    case YM_ERR_MAX_DEVICES: return "Maximum devices reached";
    case YM_ERR_BUSY:        return "Device busy";
    case YM_ERR_INVALID_ARG: return "Invalid argument";
    case YM_ERR_IO:          return "I/O error";
    case YM_ERR_NO_MEMORY:   return "Out of memory";
    case YM_ERR_VGM_INVALID: return "Invalid VGM data";
    case YM_ERR_VGM_PLAYING: return "VGM already playing";
    default:                 return "Unknown";
    }
}
