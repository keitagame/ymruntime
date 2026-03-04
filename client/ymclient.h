#pragma once
/*
 * ymclient.h - YM Runtime クライアントライブラリ
 *
 * 他のアプリケーションからYMサウンドデーモンを操作するためのAPI
 *
 * 使用例:
 *   ym_client_t *c = ym_client_connect(NULL);
 *   int dev = ym_client_open_device(c, YM_CHIP_OPN, 0, 4000000, "myapp");
 *   ym_client_set_freq(c, dev, 0, 0x269, 4);
 *   ym_client_key_on(c, dev, 0, 0x0F);
 *   // ... 音を鳴らす ...
 *   ym_client_key_off(c, dev, 0, 0x0F);
 *   ym_client_close_device(c, dev);
 *   ym_client_disconnect(c);
 */

#include <stdint.h>
#include <stddef.h>
#include "ym_ipc.h"

typedef struct ym_client ym_client_t;

/* 接続/切断 */
ym_client_t *ym_client_connect(const char *socket_path);
void         ym_client_disconnect(ym_client_t *c);
int          ym_client_is_connected(const ym_client_t *c);
int          ym_client_ping(ym_client_t *c);

/* デバイス管理 */
int  ym_client_open_device(ym_client_t *c, uint8_t chip_type,
                            uint8_t chip_index, uint32_t clock_hz,
                            const char *client_name);
int  ym_client_close_device(ym_client_t *c, int device_id);
int  ym_client_reset_device(ym_client_t *c, int device_id);
int  ym_client_list_devices(ym_client_t *c, ym_pkt_list_resp_t *out);

/* 低レベルレジスタ操作 */
int  ym_client_write_reg(ym_client_t *c, int device_id,
                          uint8_t port, uint8_t addr, uint8_t data);
int  ym_client_write_bulk(ym_client_t *c, int device_id,
                           uint8_t port,
                           const ym_reg_pair_t *pairs, uint16_t count);

/* 高レベル操作 */
int  ym_client_key_on(ym_client_t *c, int device_id,
                       uint8_t channel, uint8_t slot_mask);
int  ym_client_key_off(ym_client_t *c, int device_id,
                        uint8_t channel, uint8_t slot_mask);
int  ym_client_set_freq(ym_client_t *c, int device_id,
                         uint8_t channel, uint16_t fnum, uint8_t block);
int  ym_client_set_patch(ym_client_t *c, int device_id,
                          const ym_pkt_patch_t *patch);

/* VGM再生 */
int  ym_client_vgm_load(ym_client_t *c, int device_id,
                         const uint8_t *data, size_t len, int loop);
int  ym_client_vgm_load_file(ym_client_t *c, int device_id,
                              const char *path, int loop);
int  ym_client_vgm_play(ym_client_t *c, int device_id);
int  ym_client_vgm_pause(ym_client_t *c, int device_id);
int  ym_client_vgm_stop(ym_client_t *c, int device_id);
int  ym_client_vgm_status(ym_client_t *c, int device_id,
                            ym_pkt_vgm_status_t *status);

/* ソケットFD取得 (上級者向け) */
int ym_client_get_fd(const ym_client_t *c);

/* YM2203 FNUMを周波数(Hz)から計算 */
void ym2203_freq_to_fnum(double freq_hz, double clock,
                          uint16_t *fnum, uint8_t *block);

/* YM2151 KCをノート名から取得 (例: "A4" → 0x4A) */
uint8_t ym2151_note_to_kc(const char *note);

/* 簡単なパッチプリセット */
void ym_patch_piano(ym_pkt_patch_t *patch, int device_id, int channel);
void ym_patch_organ(ym_pkt_patch_t *patch, int device_id, int channel);
void ym_patch_brass(ym_pkt_patch_t *patch, int device_id, int channel);
void ym_patch_bell(ym_pkt_patch_t *patch, int device_id, int channel);
