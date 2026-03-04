/*
 * ymclient.c - YM Runtime クライアントライブラリ実装
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <math.h>
#include "ymclient.h"
#include "ym_ipc.h"

/* ============================================================
 * クライアント構造体
 * ============================================================ */

struct ym_client {
    int      fd;
    uint16_t seq;
    char     socket_path[108];
};

/* ============================================================
 * 接続/切断
 * ============================================================ */

ym_client_t *ym_client_connect(const char *socket_path) {
    if (!socket_path) {
        const char *env = getenv(YM_SOCKET_PATH_ENV);
        socket_path = env ? env : YM_SOCKET_PATH;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return NULL; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ymclient: サーバーに接続できません: %s\n"
                        "  ymruntimedが起動しているか確認してください\n",
                socket_path);
        close(fd);
        return NULL;
    }

    ym_client_t *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    c->fd  = fd;
    c->seq = 0;
    strncpy(c->socket_path, socket_path, sizeof(c->socket_path) - 1);
    return c;
}

void ym_client_disconnect(ym_client_t *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int ym_client_is_connected(const ym_client_t *c) {
    return c && c->fd >= 0;
}

int ym_client_get_fd(const ym_client_t *c) {
    return c ? c->fd : -1;
}

/* ============================================================
 * 低レベル送受信
 * ============================================================ */

static int send_and_recv(ym_client_t *c, ym_command_t cmd,
                          const void *payload, size_t plen,
                          void *resp_buf, size_t resp_size) {
    uint16_t seq = c->seq++;
    if (ym_send_packet(c->fd, cmd, seq, payload, plen) < 0)
        return YM_ERR_IO;

    ym_pkt_header_t hdr;
    uint8_t rbuf[4096];
    if (ym_recv_packet(c->fd, &hdr, rbuf, sizeof(rbuf)) < 0)
        return YM_ERR_IO;

    if (resp_buf && resp_size > 0) {
        size_t copy = hdr.payload_len < resp_size ? hdr.payload_len : resp_size;
        memcpy(resp_buf, rbuf, copy);
    }

    /* 汎用レスポンスのresultを返す */
    if (hdr.payload_len >= sizeof(ym_pkt_resp_t)) {
        return ((ym_pkt_resp_t *)rbuf)->result;
    }
    if (hdr.payload_len >= 4) {
        return (int)(*(int32_t *)rbuf);
    }
    return YM_OK;
}

/* ============================================================
 * Ping
 * ============================================================ */

int ym_client_ping(ym_client_t *c) {
    uint32_t dummy = 0;
    return send_and_recv(c, YM_CMD_PING, &dummy, sizeof(dummy), NULL, 0);
}

/* ============================================================
 * デバイス管理
 * ============================================================ */

int ym_client_open_device(ym_client_t *c, uint8_t chip_type,
                           uint8_t chip_index, uint32_t clock_hz,
                           const char *client_name) {
    ym_pkt_open_t req;
    memset(&req, 0, sizeof(req));
    req.chip_type  = chip_type;
    req.chip_index = chip_index;
    req.clock_hz   = clock_hz;
    if (client_name)
        strncpy(req.client_name, client_name, sizeof(req.client_name) - 1);

    ym_pkt_open_resp_t resp;
    memset(&resp, 0, sizeof(resp));

    uint16_t seq = c->seq++;
    if (ym_send_packet(c->fd, YM_CMD_OPEN_DEVICE, seq, &req, sizeof(req)) < 0)
        return -1;

    ym_pkt_header_t hdr;
    uint8_t rbuf[256];
    if (ym_recv_packet(c->fd, &hdr, rbuf, sizeof(rbuf)) < 0)
        return -1;

    memcpy(&resp, rbuf, sizeof(resp));
    if (resp.result != YM_OK) {
        fprintf(stderr, "ymclient: open_device failed: %s\n",
                ym_strerror((ym_error_t)resp.result));
        return -1;
    }
    return resp.device_id;
}

int ym_client_close_device(ym_client_t *c, int device_id) {
    uint8_t id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_CLOSE_DEVICE, &id, 1, NULL, 0);
}

int ym_client_reset_device(ym_client_t *c, int device_id) {
    uint8_t id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_RESET_DEVICE, &id, 1, NULL, 0);
}

int ym_client_list_devices(ym_client_t *c, ym_pkt_list_resp_t *out) {
    ym_pkt_header_t hdr;
    uint8_t rbuf[sizeof(ym_pkt_list_resp_t) + 64];

    uint16_t seq = c->seq++;
    if (ym_send_packet(c->fd, YM_CMD_LIST_DEVICES, seq, NULL, 0) < 0)
        return YM_ERR_IO;
    if (ym_recv_packet(c->fd, &hdr, rbuf, sizeof(rbuf)) < 0)
        return YM_ERR_IO;
    if (out) memcpy(out, rbuf, sizeof(*out));
    return YM_OK;
}

/* ============================================================
 * レジスタ操作
 * ============================================================ */

int ym_client_write_reg(ym_client_t *c, int device_id,
                         uint8_t port, uint8_t addr, uint8_t data) {
    ym_pkt_write_reg_t req = {
        .device_id = (uint8_t)device_id,
        .port      = port,
        .addr      = addr,
        .data      = data,
    };
    return send_and_recv(c, YM_CMD_WRITE_REG, &req, sizeof(req), NULL, 0);
}

int ym_client_write_bulk(ym_client_t *c, int device_id,
                          uint8_t port,
                          const ym_reg_pair_t *pairs, uint16_t count) {
    size_t total = sizeof(ym_pkt_write_bulk_hdr_t) +
                   sizeof(ym_reg_pair_t) * count;
    uint8_t *buf = malloc(total);
    if (!buf) return YM_ERR_NO_MEMORY;

    ym_pkt_write_bulk_hdr_t *hdr = (ym_pkt_write_bulk_hdr_t *)buf;
    hdr->device_id = (uint8_t)device_id;
    hdr->port      = port;
    hdr->count     = count;
    memcpy(buf + sizeof(*hdr), pairs, sizeof(ym_reg_pair_t) * count);

    int ret = send_and_recv(c, YM_CMD_WRITE_BULK, buf, total, NULL, 0);
    free(buf);
    return ret;
}

/* ============================================================
 * 高レベル操作
 * ============================================================ */

int ym_client_key_on(ym_client_t *c, int device_id,
                      uint8_t channel, uint8_t slot_mask) {
    ym_pkt_key_t req = {
        .device_id = (uint8_t)device_id,
        .channel   = channel,
        .slot_mask = slot_mask,
    };
    return send_and_recv(c, YM_CMD_KEY_ON, &req, sizeof(req), NULL, 0);
}

int ym_client_key_off(ym_client_t *c, int device_id,
                       uint8_t channel, uint8_t slot_mask) {
    ym_pkt_key_t req = {
        .device_id = (uint8_t)device_id,
        .channel   = channel,
        .slot_mask = slot_mask,
    };
    return send_and_recv(c, YM_CMD_KEY_OFF, &req, sizeof(req), NULL, 0);
}

int ym_client_set_freq(ym_client_t *c, int device_id,
                        uint8_t channel, uint16_t fnum, uint8_t block) {
    ym_pkt_freq_t req = {
        .device_id = (uint8_t)device_id,
        .channel   = channel,
        .fnum      = fnum,
        .block     = block,
    };
    return send_and_recv(c, YM_CMD_SET_FREQ, &req, sizeof(req), NULL, 0);
}

int ym_client_set_patch(ym_client_t *c, int device_id,
                         const ym_pkt_patch_t *patch) {
    ym_pkt_patch_t p = *patch;
    p.device_id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_SET_PATCH, &p, sizeof(p), NULL, 0);
}

/* ============================================================
 * VGM操作
 * ============================================================ */

int ym_client_vgm_load(ym_client_t *c, int device_id,
                        const uint8_t *data, size_t len, int loop) {
    size_t total = sizeof(ym_pkt_vgm_load_t) + len;
    uint8_t *buf = malloc(total);
    if (!buf) return YM_ERR_NO_MEMORY;

    ym_pkt_vgm_load_t *req = (ym_pkt_vgm_load_t *)buf;
    req->device_id = (uint8_t)device_id;
    req->data_len  = (uint32_t)len;
    req->loop      = (uint8_t)loop;
    memcpy(buf + sizeof(*req), data, len);

    int ret = send_and_recv(c, YM_CMD_VGM_LOAD, buf, total, NULL, 0);
    free(buf);
    return ret;
}

int ym_client_vgm_load_file(ym_client_t *c, int device_id,
                              const char *path, int loop) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return YM_ERR_IO; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return YM_ERR_VGM_INVALID; }

    uint8_t *data = malloc((size_t)sz);
    if (!data) { fclose(f); return YM_ERR_NO_MEMORY; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    int ret = ym_client_vgm_load(c, device_id, data, (size_t)sz, loop);
    free(data);
    return ret;
}

int ym_client_vgm_play(ym_client_t *c, int device_id) {
    uint8_t id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_VGM_PLAY, &id, 1, NULL, 0);
}

int ym_client_vgm_pause(ym_client_t *c, int device_id) {
    uint8_t id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_VGM_PAUSE, &id, 1, NULL, 0);
}

int ym_client_vgm_stop(ym_client_t *c, int device_id) {
    uint8_t id = (uint8_t)device_id;
    return send_and_recv(c, YM_CMD_VGM_STOP, &id, 1, NULL, 0);
}

int ym_client_vgm_status(ym_client_t *c, int device_id,
                           ym_pkt_vgm_status_t *status) {
    uint8_t id = (uint8_t)device_id;
    ym_pkt_header_t hdr;
    uint8_t rbuf[256];

    uint16_t seq = c->seq++;
    if (ym_send_packet(c->fd, YM_CMD_VGM_STATUS, seq, &id, 1) < 0)
        return YM_ERR_IO;
    if (ym_recv_packet(c->fd, &hdr, rbuf, sizeof(rbuf)) < 0)
        return YM_ERR_IO;
    if (status)
        memcpy(status, rbuf, sizeof(*status));
    return YM_OK;
}

/* ============================================================
 * ユーティリティ
 * ============================================================ */

void ym2203_freq_to_fnum(double freq_hz, double clock,
                          uint16_t *fnum, uint8_t *block) {
    /* OPN: FNUM = freq * 2^(20-block) / (clock/144) */
    /* blockを0-7で最適なものを選択 */
    double base = clock / 144.0;
    *block = 0;
    double f = freq_hz / base * (double)(1 << 20);
    while (f >= 2048.0 && *block < 7) {
        f /= 2.0;
        (*block)++;
    }
    if (f < 0) f = 0;
    if (f > 2047) f = 2047;
    *fnum = (uint16_t)f;
}

uint8_t ym2151_note_to_kc(const char *note) {
    /* "C4", "D#3", "Bb5" などをKCコードに変換 */
    static const char notes[] = "C D EF G A B";
    if (!note || !note[0]) return 0;

    int n = -1;
    for (int i = 0; i < 12; i++) {
        if (notes[i] == note[0]) { n = i; break; }
    }
    if (n < 0) return 0;

    int octave = 4;
    int pos = 1;
    if (note[pos] == '#') { n++; pos++; }
    else if (note[pos] == 'b') { n--; pos++; }
    if (n < 0)  n += 12;
    if (n > 11) n -= 12;

    if (note[pos] >= '0' && note[pos] <= '9')
        octave = note[pos] - '0';

    return (uint8_t)((octave << 4) | (n & 0xF));
}

/* ============================================================
 * プリセットパッチ
 * ============================================================ */

static void set_op(ym_pkt_patch_t *p, int op,
                   uint8_t ar, uint8_t dr, uint8_t sr, uint8_t rr,
                   uint8_t sl, uint8_t tl, uint8_t ks,
                   uint8_t mul, uint8_t dt1, uint8_t dt2, uint8_t am) {
    p->op[op].ar  = ar;  p->op[op].dr  = dr;  p->op[op].sr  = sr;
    p->op[op].rr  = rr;  p->op[op].sl  = sl;  p->op[op].tl  = tl;
    p->op[op].ks  = ks;  p->op[op].mul = mul; p->op[op].dt1 = dt1;
    p->op[op].dt2 = dt2; p->op[op].am  = am;
}

void ym_patch_piano(ym_pkt_patch_t *p, int device_id, int channel) {
    memset(p, 0, sizeof(*p));
    p->device_id = (uint8_t)device_id;
    p->channel   = (uint8_t)channel;
    p->algorithm = 1;
    p->feedback  = 2;
    p->pan       = 3;
    /*          ar  dr  sr  rr  sl  tl  ks  mul dt1 dt2 am */
    set_op(p, 0, 31, 10,  0, 12,  8,  20,  2,  1,  0,  0,  0);
    set_op(p, 1, 20,  8,  0, 10, 10,  32,  1,  2,  3,  0,  0);
    set_op(p, 2, 31, 12,  0, 14,  6,  16,  2,  1,  0,  0,  0);
    set_op(p, 3, 25,  6,  0,  8, 12,  0,   1,  1,  0,  0,  0);
}

void ym_patch_organ(ym_pkt_patch_t *p, int device_id, int channel) {
    memset(p, 0, sizeof(*p));
    p->device_id = (uint8_t)device_id;
    p->channel   = (uint8_t)channel;
    p->algorithm = 7;  /* 全並列 */
    p->feedback  = 0;
    p->pan       = 3;
    set_op(p, 0, 31,  0, 24,  8,  0,  0,  0,  1,  0,  0,  0);
    set_op(p, 1, 31,  0, 24,  8,  0, 24,  0,  2,  0,  0,  0);
    set_op(p, 2, 31,  0, 24,  8,  0, 36,  0,  3,  0,  0,  0);
    set_op(p, 3, 31,  0, 24,  8,  0, 48,  0,  4,  0,  0,  0);
}

void ym_patch_brass(ym_pkt_patch_t *p, int device_id, int channel) {
    memset(p, 0, sizeof(*p));
    p->device_id = (uint8_t)device_id;
    p->channel   = (uint8_t)channel;
    p->algorithm = 2;
    p->feedback  = 5;
    p->pan       = 3;
    set_op(p, 0, 31, 15,  6, 10,  6,  32,  1,  1,  3,  0,  0);
    set_op(p, 1, 20, 18,  8, 10,  8,  24,  1,  1,  0,  0,  0);
    set_op(p, 2, 28, 12,  4,  8,  4,  12,  2,  1,  1,  0,  0);
    set_op(p, 3, 31,  8,  2,  6,  2,   0,  1,  1,  0,  0,  0);
}

void ym_patch_bell(ym_pkt_patch_t *p, int device_id, int channel) {
    memset(p, 0, sizeof(*p));
    p->device_id = (uint8_t)device_id;
    p->channel   = (uint8_t)channel;
    p->algorithm = 5;
    p->feedback  = 0;
    p->pan       = 3;
    set_op(p, 0, 31, 20,  0, 14, 12,  16,  0,  1,  0,  0,  0);
    set_op(p, 1, 31, 18,  0, 12, 10,  24,  0, 13,  0,  0,  0);
    set_op(p, 2, 31, 22,  0, 16, 14,  28,  0,  3,  0,  0,  0);
    set_op(p, 3, 31, 16,  0, 10,  8,   0,  0,  7,  0,  0,  0);
}
