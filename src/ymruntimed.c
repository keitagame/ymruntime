/*
 * ymruntimed.c - YM Runtime デーモン
 *
 * バックグラウンドで動作するサウンドサーバー
 * - Unix Domain Socketでクライアントからのコマンドを受け付ける
 * - PulseAudio/PipeWireへリアルタイムで音声をストリーミング
 * - 複数チップの同時エミュレーション (最大YM_MAX_DEVICES個)
 * - VGMファイル再生サポート
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <getopt.h>

/* PulseAudio Simple API */
#include <pulse/simple.h>
#include <pulse/error.h>

#include "ym_core.h"
#include "ym_ipc.h"
#include "vgm_player.h"

/* ============================================================
 * 設定
 * ============================================================ */

#define DAEMON_NAME      "ymruntimed"
#define MAX_CLIENTS      32
#define AUDIO_BUF_FRAMES 512
#define MIX_CHANNELS     2
#define MIX_SAMPLE_RATE  44100

/* ============================================================
 * デバイス管理
 * ============================================================ */

typedef struct {
    int          used;
    uint8_t      device_id;
    uint8_t      chip_type;  /* ym_chip_id_t */
    uint8_t      chip_index;
    uint32_t     clock_hz;
    char         owner_name[32];
    int          owner_fd;   /* オーナークライアントのfd */
    ym_chip_t   *chip;
    vgm_player_t vgm;
} ym_device_t;

/* ============================================================
 * グローバル状態
 * ============================================================ */

static volatile int g_running  = 1;
static ym_device_t  g_devices[YM_MAX_DEVICES];
static int          g_num_devices = 0;
static pthread_mutex_t g_device_mutex = PTHREAD_MUTEX_INITIALIZER;

/* オーディオ */
static pa_simple    *g_pa_stream = NULL;
static pthread_t     g_audio_thread;

/* ソケット */
static int g_server_fd = -1;

/* ============================================================
 * シグナルハンドラ
 * ============================================================ */

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================
 * デバイス検索
 * ============================================================ */

static ym_device_t *find_device(uint8_t device_id) {
    for (int i = 0; i < YM_MAX_DEVICES; i++) {
        if (g_devices[i].used && g_devices[i].device_id == device_id)
            return &g_devices[i];
    }
    return NULL;
}

static ym_device_t *alloc_device(void) {
    for (int i = 0; i < YM_MAX_DEVICES; i++) {
        if (!g_devices[i].used) {
            memset(&g_devices[i], 0, sizeof(g_devices[i]));
            g_devices[i].used      = 1;
            g_devices[i].device_id = (uint8_t)(i + 1);
            return &g_devices[i];
        }
    }
    return NULL;
}

static void free_device(ym_device_t *dev) {
    if (!dev) return;
    vgm_player_stop(&dev->vgm);
    vgm_player_destroy(&dev->vgm);
    if (dev->chip) {
        ym_destroy(dev->chip);
        dev->chip = NULL;
    }
    dev->used = 0;
}

/* ============================================================
 * オーディオスレッド
 * PulseAudioへ継続的にオーディオデータを書き出す
 * ============================================================ */

static void *audio_thread(void *arg) {
    (void)arg;

    static int16_t chip_buf[AUDIO_BUF_FRAMES * MIX_CHANNELS];
    static int32_t mix_buf[AUDIO_BUF_FRAMES * MIX_CHANNELS];
    static int16_t out_buf[AUDIO_BUF_FRAMES * MIX_CHANNELS];

    fprintf(stderr, "[audio] thread started\n");

    while (g_running) {
        memset(mix_buf, 0, sizeof(mix_buf));

        pthread_mutex_lock(&g_device_mutex);

        for (int i = 0; i < YM_MAX_DEVICES; i++) {
            ym_device_t *dev = &g_devices[i];
            if (!dev->used || !dev->chip) continue;

            /* VGMプレーヤーのコマンドを処理 */
            vgm_player_step(&dev->vgm, AUDIO_BUF_FRAMES);

            /* チップをレンダリング */
            memset(chip_buf, 0, sizeof(chip_buf));
            ym_render(dev->chip, chip_buf, AUDIO_BUF_FRAMES);

            /* ミックスバッファに加算 */
            for (int s = 0; s < AUDIO_BUF_FRAMES * MIX_CHANNELS; s++)
                mix_buf[s] += chip_buf[s];
        }

        pthread_mutex_unlock(&g_device_mutex);

        /* クリップ & int16_tへ変換 */
        for (int s = 0; s < AUDIO_BUF_FRAMES * MIX_CHANNELS; s++) {
            int32_t v = mix_buf[s];
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            out_buf[s] = (int16_t)v;
        }

        /* PulseAudioへ書き出し */
        if (g_pa_stream) {
            int pa_err;
            if (pa_simple_write(g_pa_stream, out_buf,
                                sizeof(out_buf), &pa_err) < 0) {
                fprintf(stderr, "[audio] pa_simple_write: %s\n",
                        pa_strerror(pa_err));
            }
        } else {
            /* PulseAudio未接続の場合はスリープ */
            struct timespec ts = {
                .tv_sec  = 0,
                .tv_nsec = (long)AUDIO_BUF_FRAMES * 1000000000L / MIX_SAMPLE_RATE
            };
            nanosleep(&ts, NULL);
        }
    }

    fprintf(stderr, "[audio] thread exiting\n");
    return NULL;
}

/* ============================================================
 * コマンド処理
 * ============================================================ */

static void send_response(int fd, uint16_t seq, int32_t result,
                           const void *extra, size_t extra_len) {
    ym_pkt_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.result = result;
    if (extra && extra_len > 0 && extra_len <= sizeof(resp.data))
        memcpy(resp.data, extra, extra_len);
    ym_send_packet(fd, YM_CMD_PING /* 応答は同じコマンドIDを使わないが簡略化 */,
                   seq, &resp, sizeof(resp));
}

static void handle_open_device(int fd, uint16_t seq,
                                const ym_pkt_open_t *req) {
    pthread_mutex_lock(&g_device_mutex);

    ym_device_t *dev = alloc_device();
    if (!dev) {
        pthread_mutex_unlock(&g_device_mutex);
        ym_pkt_open_resp_t resp = { .result = YM_ERR_MAX_DEVICES, .device_id = 0 };
        ym_send_packet(fd, YM_CMD_OPEN_DEVICE, seq, &resp, sizeof(resp));
        return;
    }

    /* チップ種別 → ym_chip_type_t */
    ym_chip_type_t ctype;
    double clock_hz = req->clock_hz ? (double)req->clock_hz : 4000000.0;
    switch (req->chip_type) {
    case YM_CHIP_OPN:  ctype = YM_CHIP_YM2203; break;
    case YM_CHIP_OPM:  ctype = YM_CHIP_YM2151; break;
    default:
        free_device(dev);
        pthread_mutex_unlock(&g_device_mutex);
        ym_pkt_open_resp_t resp = { .result = YM_ERR_INVALID_ARG };
        ym_send_packet(fd, YM_CMD_OPEN_DEVICE, seq, &resp, sizeof(resp));
        return;
    }

    dev->chip = ym_create(ctype, clock_hz, MIX_SAMPLE_RATE);
    if (!dev->chip) {
        free_device(dev);
        pthread_mutex_unlock(&g_device_mutex);
        ym_pkt_open_resp_t resp = { .result = YM_ERR_NO_MEMORY };
        ym_send_packet(fd, YM_CMD_OPEN_DEVICE, seq, &resp, sizeof(resp));
        return;
    }

    dev->chip_type  = req->chip_type;
    dev->chip_index = req->chip_index;
    dev->clock_hz   = req->clock_hz;
    dev->owner_fd   = fd;
    vgm_player_init(&dev->vgm);
    dev->vgm.chip_2203 = (ctype == YM_CHIP_YM2203) ? dev->chip : NULL;
    dev->vgm.chip_2151 = (ctype == YM_CHIP_YM2151) ? dev->chip : NULL;
    strncpy(dev->owner_name, req->client_name, sizeof(dev->owner_name) - 1);

    uint8_t did = dev->device_id;
    g_num_devices++;
    pthread_mutex_unlock(&g_device_mutex);

    fprintf(stderr, "[daemon] opened device %u: chip=%u clock=%uHz owner=%s\n",
            did, req->chip_type, req->clock_hz, req->client_name);

    ym_pkt_open_resp_t resp = { .result = YM_OK, .device_id = did };
    ym_send_packet(fd, YM_CMD_OPEN_DEVICE, seq, &resp, sizeof(resp));
}

static void handle_close_device(int fd, uint16_t seq, const uint8_t *payload) {
    uint8_t device_id = payload[0];
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(device_id);
    int32_t result = YM_ERR_NO_DEVICE;
    if (dev) {
        free_device(dev);
        g_num_devices--;
        result = YM_OK;
    }
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, result, NULL, 0);
}

static void handle_write_reg(int fd, uint16_t seq,
                              const ym_pkt_write_reg_t *req) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(req->device_id);
    if (!dev || !dev->chip) {
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0);
        return;
    }
    /* OPNはポート1(0xA0-0xFF系)は別アドレス空間だが簡略化 */
    ym_write(dev->chip, req->addr, req->data);
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, YM_OK, NULL, 0);
}

static void handle_key_on(int fd, uint16_t seq,
                           const ym_pkt_key_t *req, int on) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(req->device_id);
    if (!dev || !dev->chip) {
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0);
        return;
    }
    if (on) {
        switch (dev->chip->type) {
        case YM_CHIP_YM2203: ym2203_key_on(&dev->chip->opn, req->channel, req->slot_mask); break;
        case YM_CHIP_YM2151: ym2151_key_on(&dev->chip->opm, req->channel, req->slot_mask); break;
        default: break;
        }
    } else {
        switch (dev->chip->type) {
        case YM_CHIP_YM2203: ym2203_key_off(&dev->chip->opn, req->channel, req->slot_mask); break;
        case YM_CHIP_YM2151: ym2151_key_off(&dev->chip->opm, req->channel, req->slot_mask); break;
        default: break;
        }
    }
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, YM_OK, NULL, 0);
}

static void handle_set_freq(int fd, uint16_t seq,
                             const ym_pkt_freq_t *req) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(req->device_id);
    if (!dev || !dev->chip) {
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0);
        return;
    }
    switch (dev->chip->type) {
    case YM_CHIP_YM2203:
        ym2203_set_freq(&dev->chip->opn, req->channel, req->fnum, req->block);
        break;
    case YM_CHIP_YM2151:
        ym2151_set_freq(&dev->chip->opm, req->channel,
                        (uint8_t)(req->fnum >> 6), (uint8_t)(req->fnum & 0x3F));
        break;
    default: break;
    }
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, YM_OK, NULL, 0);
}

static void handle_vgm_load(int fd, uint16_t seq,
                             const ym_pkt_vgm_load_t *req,
                             const uint8_t *vgm_data) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(req->device_id);
    if (!dev) {
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0);
        return;
    }
    vgm_player_stop(&dev->vgm);
    int ret = vgm_player_load(&dev->vgm, vgm_data, req->data_len, req->loop);
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, ret == 0 ? YM_OK : YM_ERR_VGM_INVALID, NULL, 0);
}

static void handle_vgm_play(int fd, uint16_t seq, const uint8_t *payload) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(payload[0]);
    if (!dev) { pthread_mutex_unlock(&g_device_mutex); send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0); return; }
    int ret = vgm_player_play(&dev->vgm);
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, ret == 0 ? YM_OK : YM_ERR_VGM_INVALID, NULL, 0);
}

static void handle_vgm_pause(int fd, uint16_t seq, const uint8_t *payload) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(payload[0]);
    if (dev) vgm_player_pause(&dev->vgm);
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, dev ? YM_OK : YM_ERR_NO_DEVICE, NULL, 0);
}

static void handle_vgm_stop(int fd, uint16_t seq, const uint8_t *payload) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(payload[0]);
    if (dev) vgm_player_stop(&dev->vgm);
    pthread_mutex_unlock(&g_device_mutex);
    send_response(fd, seq, dev ? YM_OK : YM_ERR_NO_DEVICE, NULL, 0);
}

static void handle_vgm_status(int fd, uint16_t seq, const uint8_t *payload) {
    pthread_mutex_lock(&g_device_mutex);
    ym_device_t *dev = find_device(payload[0]);
    if (!dev) {
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, seq, YM_ERR_NO_DEVICE, NULL, 0);
        return;
    }
    ym_pkt_vgm_status_t st = {
        .state       = (uint8_t)dev->vgm.state,
        .position_ms = vgm_player_position_ms(&dev->vgm),
        .total_ms    = vgm_player_total_ms(&dev->vgm),
        .loop_count  = dev->vgm.loop_count,
    };
    pthread_mutex_unlock(&g_device_mutex);
    ym_send_packet(fd, YM_CMD_VGM_STATUS, seq, &st, sizeof(st));
}

static void handle_list_devices(int fd, uint16_t seq) {
    pthread_mutex_lock(&g_device_mutex);
    ym_pkt_list_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    int cnt = 0;
    for (int i = 0; i < YM_MAX_DEVICES && cnt < YM_MAX_DEVICES; i++) {
        if (!g_devices[i].used) continue;
        resp.devices[cnt].device_id  = g_devices[i].device_id;
        resp.devices[cnt].chip_type  = g_devices[i].chip_type;
        resp.devices[cnt].chip_index = g_devices[i].chip_index;
        resp.devices[cnt].in_use     = 1;
        resp.devices[cnt].clock_hz   = g_devices[i].clock_hz;
        strncpy(resp.devices[cnt].owner_name, g_devices[i].owner_name, 31);
        cnt++;
    }
    resp.count = (uint8_t)cnt;
    pthread_mutex_unlock(&g_device_mutex);
    ym_send_packet(fd, YM_CMD_LIST_DEVICES, seq, &resp, sizeof(resp));
}

/* ============================================================
 * クライアント処理
 * ============================================================ */

static void process_client(int fd) {
    ym_pkt_header_t hdr;
    uint8_t payload[1024 * 1024]; /* 最大1MB (VGMロード用) */

    if (ym_recv_packet(fd, &hdr, payload, sizeof(payload)) < 0)
        return;

    ym_command_t cmd = (ym_command_t)hdr.command;

    switch (cmd) {
    case YM_CMD_OPEN_DEVICE:
        if (hdr.payload_len >= sizeof(ym_pkt_open_t))
            handle_open_device(fd, hdr.seq, (ym_pkt_open_t *)payload);
        break;

    case YM_CMD_CLOSE_DEVICE:
        if (hdr.payload_len >= 1)
            handle_close_device(fd, hdr.seq, payload);
        break;

    case YM_CMD_RESET_DEVICE:
        if (hdr.payload_len >= 1) {
            pthread_mutex_lock(&g_device_mutex);
            ym_device_t *dev = find_device(payload[0]);
            if (dev && dev->chip) ym_reset(dev->chip);
            pthread_mutex_unlock(&g_device_mutex);
            send_response(fd, hdr.seq, dev ? YM_OK : YM_ERR_NO_DEVICE, NULL, 0);
        }
        break;

    case YM_CMD_WRITE_REG:
        if (hdr.payload_len >= sizeof(ym_pkt_write_reg_t))
            handle_write_reg(fd, hdr.seq, (ym_pkt_write_reg_t *)payload);
        break;

    case YM_CMD_WRITE_BULK: {
        if (hdr.payload_len < sizeof(ym_pkt_write_bulk_hdr_t)) break;
        ym_pkt_write_bulk_hdr_t *bhdr = (ym_pkt_write_bulk_hdr_t *)payload;
        ym_reg_pair_t *pairs = (ym_reg_pair_t *)(payload + sizeof(*bhdr));
        pthread_mutex_lock(&g_device_mutex);
        ym_device_t *dev = find_device(bhdr->device_id);
        if (dev && dev->chip) {
            for (int p = 0; p < bhdr->count; p++)
                ym_write(dev->chip, pairs[p].addr, pairs[p].data);
        }
        pthread_mutex_unlock(&g_device_mutex);
        send_response(fd, hdr.seq, dev ? YM_OK : YM_ERR_NO_DEVICE, NULL, 0);
        break;
    }

    case YM_CMD_KEY_ON:
        if (hdr.payload_len >= sizeof(ym_pkt_key_t))
            handle_key_on(fd, hdr.seq, (ym_pkt_key_t *)payload, 1);
        break;

    case YM_CMD_KEY_OFF:
        if (hdr.payload_len >= sizeof(ym_pkt_key_t))
            handle_key_on(fd, hdr.seq, (ym_pkt_key_t *)payload, 0);
        break;

    case YM_CMD_SET_FREQ:
        if (hdr.payload_len >= sizeof(ym_pkt_freq_t))
            handle_set_freq(fd, hdr.seq, (ym_pkt_freq_t *)payload);
        break;

    case YM_CMD_SET_PATCH:
        if (hdr.payload_len >= sizeof(ym_pkt_patch_t)) {
            ym_pkt_patch_t *p = (ym_pkt_patch_t *)payload;
            /* パッチをレジスタに展開して書き込む */
            pthread_mutex_lock(&g_device_mutex);
            ym_device_t *dev = find_device(p->device_id);
            if (dev && dev->chip) {
                uint8_t ch = p->channel;
                /* ALG/FB */
                ym_write(dev->chip, 0xB0 + ch,
                         (p->feedback << 3) | p->algorithm);
                /* 4 OPのパラメータ */
                static const int op_off[4] = {0, 8, 4, 12};
                for (int op = 0; op < 4; op++) {
                    int b = 0x30 + op_off[op] + ch;
                    ym_write(dev->chip, b,        (p->op[op].dt1 << 4) | p->op[op].mul);
                    ym_write(dev->chip, b + 0x10, p->op[op].tl);
                    ym_write(dev->chip, b + 0x20, (p->op[op].ks << 6) | p->op[op].ar);
                    ym_write(dev->chip, b + 0x30, (p->op[op].am << 7) | p->op[op].dr);
                    ym_write(dev->chip, b + 0x40, p->op[op].sr);
                    ym_write(dev->chip, b + 0x50, (p->op[op].sl << 4) | p->op[op].rr);
                }
            }
            pthread_mutex_unlock(&g_device_mutex);
            send_response(fd, hdr.seq, dev ? YM_OK : YM_ERR_NO_DEVICE, NULL, 0);
        }
        break;

    case YM_CMD_VGM_LOAD:
        if (hdr.payload_len >= sizeof(ym_pkt_vgm_load_t)) {
            ym_pkt_vgm_load_t *req = (ym_pkt_vgm_load_t *)payload;
            handle_vgm_load(fd, hdr.seq, req,
                            payload + sizeof(*req));
        }
        break;

    case YM_CMD_VGM_PLAY:  handle_vgm_play(fd, hdr.seq, payload);   break;
    case YM_CMD_VGM_PAUSE: handle_vgm_pause(fd, hdr.seq, payload);  break;
    case YM_CMD_VGM_STOP:  handle_vgm_stop(fd, hdr.seq, payload);   break;
    case YM_CMD_VGM_STATUS:handle_vgm_status(fd, hdr.seq, payload); break;

    case YM_CMD_LIST_DEVICES:
        handle_list_devices(fd, hdr.seq);
        break;

    case YM_CMD_PING: {
        uint32_t pong = 0xDEADBEEFu;
        ym_send_packet(fd, YM_CMD_PING, hdr.seq, &pong, sizeof(pong));
        break;
    }

    case YM_CMD_GET_VERSION: {
        uint32_t ver = YM_PROTO_VERSION;
        ym_send_packet(fd, YM_CMD_GET_VERSION, hdr.seq, &ver, sizeof(ver));
        break;
    }

    case YM_CMD_SHUTDOWN:
        fprintf(stderr, "[daemon] shutdown requested by client\n");
        g_running = 0;
        send_response(fd, hdr.seq, YM_OK, NULL, 0);
        break;

    default:
        send_response(fd, hdr.seq, YM_ERR_INVALID_CMD, NULL, 0);
        break;
    }
}

/* ============================================================
 * クライアントが切断したときのクリーンアップ
 * ============================================================ */

static void cleanup_client(int fd) {
    pthread_mutex_lock(&g_device_mutex);
    for (int i = 0; i < YM_MAX_DEVICES; i++) {
        //if (g_devices[i].used && g_devices[i].owner_fd == fd) {
       //     fprintf(stderr, "[daemon] cleaning up device %u for disconnected client\n",
       //             g_devices[i].device_id);
       //     free_device(&g_devices[i]);
       //     g_num_devices--;
      //  }
    }
    pthread_mutex_unlock(&g_device_mutex);
}

/* ============================================================
 * メインループ
 * ============================================================ */

static int setup_server_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path);  /* 既存ソケットファイルを削除 */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    chmod(path, 0666);  /* 全ユーザーがアクセス可能に */
    if (listen(fd, 16) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static pa_simple *setup_pulseaudio(void) {
    static const pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16NE,
        .rate     = MIX_SAMPLE_RATE,
        .channels = MIX_CHANNELS,
    };
    static const pa_buffer_attr ba = {
        .maxlength = (uint32_t)-1,
        .tlength   = AUDIO_BUF_FRAMES * MIX_CHANNELS * 2 * 2,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = (uint32_t)-1,
    };

    int pa_err;
    pa_simple *pa = pa_simple_new(
        NULL,             /* デフォルトサーバー */
        DAEMON_NAME,      /* アプリ名 */
        PA_STREAM_PLAYBACK,
        NULL,             /* デフォルトデバイス */
        "YM FM Synth",    /* ストリーム名 */
        &ss,
        NULL,             /* チャンネルマップ */
        &ba,
        &pa_err
    );
    if (!pa) {
        fprintf(stderr, "[audio] PulseAudio接続失敗: %s\n",
                pa_strerror(pa_err));
        fprintf(stderr, "[audio] サイレントモードで継続します\n");
    }
    return pa;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "使用法: %s [オプション]\n"
        "  -s <path>   ソケットパス (デフォルト: %s)\n"
        "  -f          フォアグラウンドで実行\n"
        "  -h          ヘルプ\n",
        prog, YM_SOCKET_PATH);
}

int main(int argc, char *argv[]) {
    const char *socket_path = YM_SOCKET_PATH;
    int foreground = 1;  /* デフォルト: フォアグラウンド (デバッグ用) */

    int opt;
    while ((opt = getopt(argc, argv, "s:fh")) != -1) {
        switch (opt) {
        case 's': socket_path  = optarg; break;
        case 'f': foreground   = 1;      break;
        case 'h': print_usage(argv[0]);  return 0;
        default:  print_usage(argv[0]);  return 1;
        }
    }

    /* シグナル設定 */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[daemon] %s starting\n", DAEMON_NAME);
    fprintf(stderr, "[daemon] socket: %s\n", socket_path);

    /* PulseAudio接続 */
    g_pa_stream = setup_pulseaudio();

    /* サーバーソケット */
    g_server_fd = setup_server_socket(socket_path);
    if (g_server_fd < 0) return 1;

    /* オーディオスレッド起動 */
    pthread_create(&g_audio_thread, NULL, audio_thread, NULL);

    fprintf(stderr, "[daemon] ready, waiting for clients...\n");

    /* クライアント管理 */
    int client_fds[MAX_CLIENTS];
    memset(client_fds, -1, sizeof(client_fds));

    struct pollfd fds[MAX_CLIENTS + 1];

    while (g_running) {
        int nfds = 0;
        fds[nfds].fd      = g_server_fd;
        fds[nfds].events  = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                fds[nfds].fd      = client_fds[i];
                fds[nfds].events  = POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }

        int ret = poll(fds, (nfds_t)nfds, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* 新規接続 */
        if (fds[0].revents & POLLIN) {
            int cfd = accept(g_server_fd, NULL, NULL);
            if (cfd >= 0) {
                /* 空きスロットを探す */
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] < 0) {
                        client_fds[i] = cfd;
                        placed = 1;
                        fprintf(stderr, "[daemon] client connected (fd=%d slot=%d)\n",
                                cfd, i);
                        break;
                    }
                }
                if (!placed) {
                    fprintf(stderr, "[daemon] max clients reached, rejecting\n");
                    close(cfd);
                }
            }
        }

        /* クライアントデータ */
        for (int pi = 1; pi < nfds; pi++) {
            if (!(fds[pi].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            int cfd = fds[pi].fd;
            if (fds[pi].revents & POLLIN) {
                process_client(cfd);
            }
            if (fds[pi].revents & (POLLHUP | POLLERR)) {
                fprintf(stderr, "[daemon] client disconnected (fd=%d)\n", cfd);
                cleanup_client(cfd);
                close(cfd);
                /* スロットを空ける */
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == cfd) {
                        client_fds[i] = -1;
                        break;
                    }
                }
            }
        }
    }

    fprintf(stderr, "[daemon] shutting down...\n");

    /* クリーンアップ */
    g_running = 0;
    pthread_join(g_audio_thread, NULL);

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (client_fds[i] >= 0) { close(client_fds[i]); client_fds[i] = -1; }

    if (g_server_fd >= 0) { close(g_server_fd); g_server_fd = -1; }
    unlink(socket_path);

    pthread_mutex_lock(&g_device_mutex);
    for (int i = 0; i < YM_MAX_DEVICES; i++)
        if (g_devices[i].used) free_device(&g_devices[i]);
    pthread_mutex_unlock(&g_device_mutex);

    if (g_pa_stream) {
        pa_simple_drain(g_pa_stream, NULL);
        pa_simple_free(g_pa_stream);
    }

    fprintf(stderr, "[daemon] bye\n");
    return 0;
}
