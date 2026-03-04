/*
 * ymctl.c - YM Runtime コマンドラインコントロールツール
 *
 * 使い方:
 *   ymctl list                         # デバイス一覧
 *   ymctl open opn 4000000 myapp       # YM2203デバイスを開く
 *   ymctl open opm 3580000 myapp       # YM2151デバイスを開く
 *   ymctl close <id>                   # デバイスを閉じる
 *   ymctl reset <id>                   # チップリセット
 *   ymctl reg <id> <addr> <data>       # レジスタ書き込み
 *   ymctl keyon <id> <ch> [mask]       # キーオン
 *   ymctl keyoff <id> <ch> [mask]      # キーオフ
 *   ymctl freq <id> <ch> <freq_hz>     # 周波数設定
 *   ymctl patch <id> <ch> <type>       # プリセットパッチ適用
 *   ymctl vgm load <id> <file> [loop]  # VGMファイルをロード
 *   ymctl vgm play <id>                # VGM再生
 *   ymctl vgm pause <id>               # VGM一時停止
 *   ymctl vgm stop <id>                # VGM停止
 *   ymctl vgm status <id>              # VGM状態
 *   ymctl ping                         # 疎通確認
 *   ymctl shutdown                     # デーモン終了
 *   ymctl demo <type>                  # デモ演奏
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ymclient.h"
#include "ym_ipc.h"

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ymctl内部の簡易送受信 (shutdownコマンド用) */
static int ctl_send_cmd(ym_client_t *c, ym_command_t cmd) {
    /* ymclient.hのym_send_packet経由で送信 */
    /* シャットダウンはレスポンス不要なのでwrite直接ではなく
       ym_client_ping経由でも良いが、専用コマンドが必要 */
    /* workaround: ソケットFDを直接使わずにymclient APIで代替 */
    (void)c; (void)cmd;
    return 0; /* daemonはSIGTERMでも停止可能 */
}

static void print_usage(void) {
    puts(
        "ymctl - YM Runtime コントロールツール\n"
        "\n"
        "使い方: ymctl <コマンド> [引数...]\n"
        "\n"
        "デバイス管理:\n"
        "  list                          デバイス一覧\n"
        "  open <opn|opm> <clock> <name> デバイスをオープン\n"
        "  close <id>                    デバイスをクローズ\n"
        "  reset <id>                    チップリセット\n"
        "\n"
        "レジスタ操作:\n"
        "  reg <id> <addr> <data>        レジスタ書き込み (16進数可)\n"
        "\n"
        "音源制御:\n"
        "  keyon  <id> <ch> [mask]       キーオン (mask省略時=0x0F)\n"
        "  keyoff <id> <ch> [mask]       キーオフ\n"
        "  freq   <id> <ch> <Hz>         周波数設定\n"
        "  patch  <id> <ch> <piano|organ|brass|bell>  プリセット音色\n"
        "\n"
        "VGM再生:\n"
        "  vgm load  <id> <file> [loop]  VGMファイルをロード\n"
        "  vgm play  <id>                再生開始\n"
        "  vgm pause <id>                一時停止/再開\n"
        "  vgm stop  <id>                停止\n"
        "  vgm status <id>               再生状態を表示\n"
        "\n"
        "システム:\n"
        "  ping                          デーモン疎通確認\n"
        "  shutdown                      デーモン終了\n"
        "\n"
        "デモ:\n"
        "  demo scale <opn|opm>          スケールデモ演奏\n"
        "  demo chord <opn|opm>          コードデモ演奏\n"
    );
}

/* ============================================================
 * コマンド実装
 * ============================================================ */

static int cmd_list(ym_client_t *c) {
    ym_pkt_list_resp_t resp;
    int ret = ym_client_list_devices(c, &resp);
    if (ret != YM_OK) {
        fprintf(stderr, "list失敗: %s\n", ym_strerror((ym_error_t)ret));
        return 1;
    }
    if (resp.count == 0) {
        puts("デバイスが開かれていません");
        return 0;
    }
    printf("%-4s %-8s %-6s %-12s %s\n",
           "ID", "チップ", "Idx", "クロック", "オーナー");
    puts("--------------------------------------------");
    static const char *chip_names[] = {"?", "YM2203", "YM2151", "YM2612", "YM3812"};
    for (int i = 0; i < resp.count; i++) {
        const char *name = resp.devices[i].chip_type < 5
            ? chip_names[resp.devices[i].chip_type] : "?";
        printf("%-4u %-8s %-6u %-12u %s\n",
               resp.devices[i].device_id,
               name,
               resp.devices[i].chip_index,
               resp.devices[i].clock_hz,
               resp.devices[i].owner_name);
    }
    return 0;
}

static int cmd_open(ym_client_t *c, int argc, char *argv[]) {
    /* open <opn|opm> <clock> <name> */
    if (argc < 4) {
        fprintf(stderr, "使い方: ymctl open <opn|opm> <clock_hz> <name>\n");
        return 1;
    }
    uint8_t chip_type;
    if      (strcmp(argv[1], "opn") == 0) chip_type = YM_CHIP_OPN;
    else if (strcmp(argv[1], "opm") == 0) chip_type = YM_CHIP_OPM;
    else { fprintf(stderr, "不明なチップ: %s\n", argv[1]); return 1; }

    uint32_t clock = (uint32_t)strtoul(argv[2], NULL, 0);
    if (!clock) clock = (chip_type == YM_CHIP_OPN) ? 4000000 : 3580000;

    int id = ym_client_open_device(c, chip_type, 0, clock, argv[3]);
    if (id < 0) { fprintf(stderr, "オープン失敗\n"); return 1; }
    printf("デバイスID: %d\n", id);
    return 0;
}

static int cmd_reg(ym_client_t *c, int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "使い方: ymctl reg <id> <addr> <data>\n");
        return 1;
    }
    int     id   = atoi(argv[1]);
    uint8_t addr = (uint8_t)strtoul(argv[2], NULL, 16);
    uint8_t data = (uint8_t)strtoul(argv[3], NULL, 16);
    int ret = ym_client_write_reg(c, id, 0, addr, data);
    if (ret != YM_OK) fprintf(stderr, "reg失敗: %s\n", ym_strerror((ym_error_t)ret));
    return ret != YM_OK;
}

static int cmd_freq(ym_client_t *c, int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "使い方: ymctl freq <id> <ch> <Hz>\n");
        return 1;
    }
    int    id   = atoi(argv[1]);
    int    ch   = atoi(argv[2]);
    double freq = atof(argv[3]);

    /* チップの種類を取得してFNUM計算 */
    ym_pkt_list_resp_t resp;
    ym_client_list_devices(c, &resp);
    double clock = 4000000;
    for (int i = 0; i < resp.count; i++) {
        if (resp.devices[i].device_id == (uint8_t)id) {
            clock = resp.devices[i].clock_hz;
            break;
        }
    }

    uint16_t fnum;
    uint8_t  block;
    ym2203_freq_to_fnum(freq, clock, &fnum, &block);
    printf("freq=%.2fHz → fnum=%u block=%u\n", freq, fnum, block);

    int ret = ym_client_set_freq(c, id, (uint8_t)ch, fnum, block);
    if (ret != YM_OK) fprintf(stderr, "freq失敗: %s\n", ym_strerror((ym_error_t)ret));
    return ret != YM_OK;
}

static int cmd_vgm(ym_client_t *c, int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "使い方: ymctl vgm <load|play|pause|stop|status> ...\n"); return 1; }

    const char *sub = argv[1];
    int id = argc >= 3 ? atoi(argv[2]) : 0;

    if (strcmp(sub, "load") == 0) {
        if (argc < 4) { fprintf(stderr, "使い方: ymctl vgm load <id> <file> [loop]\n"); return 1; }
        int loop = argc >= 5 ? atoi(argv[4]) : 0;
        int ret  = ym_client_vgm_load_file(c, id, argv[3], loop);
        if (ret != YM_OK) fprintf(stderr, "vgm load失敗: %s\n", ym_strerror((ym_error_t)ret));
        else printf("VGMロード完了 (デバイス%d, loop=%d)\n", id, loop);
        return ret != YM_OK;
    }
    else if (strcmp(sub, "play") == 0) {
        int ret = ym_client_vgm_play(c, id);
        if (ret != YM_OK) fprintf(stderr, "vgm play失敗: %s\n", ym_strerror((ym_error_t)ret));
        return ret != YM_OK;
    }
    else if (strcmp(sub, "pause") == 0) {
        return ym_client_vgm_pause(c, id) != YM_OK;
    }
    else if (strcmp(sub, "stop") == 0) {
        return ym_client_vgm_stop(c, id) != YM_OK;
    }
    else if (strcmp(sub, "status") == 0) {
        ym_pkt_vgm_status_t st;
        if (ym_client_vgm_status(c, id, &st) != YM_OK) return 1;
        static const char *states[] = {"停止", "再生中", "一時停止"};
        printf("状態: %s\n", states[st.state < 3 ? st.state : 0]);
        printf("位置: %u:%02u.%03u / %u:%02u.%03u\n",
               st.position_ms / 60000, (st.position_ms / 1000) % 60,
               st.position_ms % 1000,
               st.total_ms / 60000, (st.total_ms / 1000) % 60,
               st.total_ms % 1000);
        printf("ループ: %u回\n", st.loop_count);
        return 0;
    }
    fprintf(stderr, "不明なVGMサブコマンド: %s\n", sub);
    return 1;
}

/* ============================================================
 * デモ演奏
 * ============================================================ */

static void demo_init_patch(ym_client_t *c, int id,
                             uint8_t chip_type, int channel,
                             const char *patch_type) {
    ym_pkt_patch_t patch;
    if      (strcmp(patch_type, "organ") == 0) ym_patch_organ(&patch, id, channel);
    else if (strcmp(patch_type, "brass") == 0) ym_patch_brass(&patch, id, channel);
    else if (strcmp(patch_type, "bell")  == 0) ym_patch_bell (&patch, id, channel);
    else                                        ym_patch_piano(&patch, id, channel);

    if (chip_type == YM_CHIP_OPM) {
        /* OPM: pan設定 */
        ym_client_write_reg(c, id, 0, 0x20 + channel, 0xC0 | 0x00); /* L+R, ALG=0 */
    }
    ym_client_set_patch(c, id, &patch);
}

static const double C_MAJOR_SCALE[] = {
    261.63, 293.66, 329.63, 349.23,
    392.00, 440.00, 493.88, 523.25
};
static const char *SCALE_NOTES[] = {
    "C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5"
};

static int cmd_demo(ym_client_t *c, int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "使い方: ymctl demo <scale|chord> <opn|opm>\n");
        return 1;
    }
    const char *sub  = argv[1];
    const char *chip_str = argc >= 3 ? argv[2] : "opn";
    uint8_t chip_type = strcmp(chip_str, "opm") == 0 ? YM_CHIP_OPM : YM_CHIP_OPN;
    double  clock     = chip_type == YM_CHIP_OPM ? 3580000.0 : 4000000.0;
    const char *pname = argc >= 4 ? argv[3] : "piano";

    int id = ym_client_open_device(c, chip_type, 0, (uint32_t)clock, "ymctl-demo");
    if (id < 0) { fprintf(stderr, "デバイスオープン失敗\n"); return 1; }
    printf("デバイス%d (%s) でデモ演奏開始...\n", id, chip_str);

    demo_init_patch(c, id, chip_type, 0, pname);

    if (strcmp(sub, "scale") == 0) {
        /* スケール: ドレミファソラシド */
        for (int i = 0; i < 8; i++) {
            uint16_t fnum; uint8_t block;
            ym2203_freq_to_fnum(C_MAJOR_SCALE[i], clock, &fnum, &block);
            ym_client_set_freq(c, id, 0, fnum, block);
            ym_client_key_on(c, id, 0, 0x0F);
            printf("  %s (%.2fHz)\n", SCALE_NOTES[i], C_MAJOR_SCALE[i]);
            msleep(300);
            ym_client_key_off(c, id, 0, 0x0F);
            msleep(100);
        }
    }
    else if (strcmp(sub, "chord") == 0) {
        /* コード: C-F-G-C */
        double chords[4][3] = {
            { 261.63, 329.63, 392.00 }, /* C */
            { 349.23, 440.00, 523.25 }, /* F */
            { 392.00, 493.88, 587.33 }, /* G */
            { 261.63, 329.63, 392.00 }, /* C */
        };
        const char *chord_names[] = { "C", "F", "G", "C" };
        int max_ch = chip_type == YM_CHIP_OPM ? 3 : 3;

        for (int ci = 0; ci < 4; ci++) {
            printf("  コード: %s\n", chord_names[ci]);
            for (int ch = 0; ch < max_ch; ch++) {
                demo_init_patch(c, id, chip_type, ch, pname);
                uint16_t fnum; uint8_t block;
                ym2203_freq_to_fnum(chords[ci][ch], clock, &fnum, &block);
                ym_client_set_freq(c, id, (uint8_t)ch, fnum, block);
                ym_client_key_on(c, id, (uint8_t)ch, 0x0F);
            }
            msleep(600);
            for (int ch = 0; ch < max_ch; ch++)
                ym_client_key_off(c, id, (uint8_t)ch, 0x0F);
            msleep(200);
        }
    }

    msleep(500);
    ym_client_close_device(c, id);
    puts("デモ完了");
    return 0;
}

/* ============================================================
 * メイン
 * ============================================================ */

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(); return 0; }

    const char *cmd = argv[1];

    /* ping/shutdown は接続チェック込み */
    ym_client_t *c = ym_client_connect(NULL);
    if (!c) {
        fprintf(stderr, "デーモンに接続できません\n"
                        "  ymruntimed を先に起動してください\n");
        return 1;
    }

    int ret = 0;
    if (strcmp(cmd, "list") == 0) {
        ret = cmd_list(c);
    } else if (strcmp(cmd, "open") == 0) {
        ret = cmd_open(c, argc - 1, argv + 1);
    } else if (strcmp(cmd, "close") == 0) {
        if (argc < 3) { fprintf(stderr, "使い方: ymctl close <id>\n"); ret = 1; }
        else { int r = ym_client_close_device(c, atoi(argv[2]));
               printf(r == YM_OK ? "閉じました\n" : "失敗: %s\n", ym_strerror((ym_error_t)r));
               ret = r != YM_OK; }
    } else if (strcmp(cmd, "reset") == 0) {
        if (argc < 3) { fprintf(stderr, "使い方: ymctl reset <id>\n"); ret = 1; }
        else { int r = ym_client_reset_device(c, atoi(argv[2]));
               printf(r == YM_OK ? "リセットしました\n" : "失敗: %s\n", ym_strerror((ym_error_t)r));
               ret = r != YM_OK; }
    } else if (strcmp(cmd, "reg") == 0) {
        ret = cmd_reg(c, argc - 1, argv + 1);
    } else if (strcmp(cmd, "keyon") == 0) {
        if (argc < 4) { fprintf(stderr, "使い方: ymctl keyon <id> <ch> [mask]\n"); ret = 1; }
        else {
            uint8_t mask = argc >= 5 ? (uint8_t)strtoul(argv[4], NULL, 16) : 0x0F;
            ret = ym_client_key_on(c, atoi(argv[2]), (uint8_t)atoi(argv[3]), mask) != YM_OK;
        }
    } else if (strcmp(cmd, "keyoff") == 0) {
        if (argc < 4) { fprintf(stderr, "使い方: ymctl keyoff <id> <ch> [mask]\n"); ret = 1; }
        else {
            uint8_t mask = argc >= 5 ? (uint8_t)strtoul(argv[4], NULL, 16) : 0x0F;
            ret = ym_client_key_off(c, atoi(argv[2]), (uint8_t)atoi(argv[3]), mask) != YM_OK;
        }
    } else if (strcmp(cmd, "freq") == 0) {
        ret = cmd_freq(c, argc - 1, argv + 1);
    } else if (strcmp(cmd, "patch") == 0) {
        if (argc < 5) { fprintf(stderr, "使い方: ymctl patch <id> <ch> <piano|organ|brass|bell>\n"); ret = 1; }
        else {
            ym_pkt_patch_t patch;
            if      (strcmp(argv[4], "organ") == 0) ym_patch_organ(&patch, atoi(argv[2]), atoi(argv[3]));
            else if (strcmp(argv[4], "brass") == 0) ym_patch_brass(&patch, atoi(argv[2]), atoi(argv[3]));
            else if (strcmp(argv[4], "bell")  == 0) ym_patch_bell (&patch, atoi(argv[2]), atoi(argv[3]));
            else                                     ym_patch_piano(&patch, atoi(argv[2]), atoi(argv[3]));
            ret = ym_client_set_patch(c, atoi(argv[2]), &patch) != YM_OK;
        }
    } else if (strcmp(cmd, "vgm") == 0) {
        ret = cmd_vgm(c, argc - 1, argv + 1);
    } else if (strcmp(cmd, "ping") == 0) {
        int r = ym_client_ping(c);
        printf(r == YM_OK ? "pong! (デーモン稼働中)\n" : "応答なし\n");
        ret = r != YM_OK;
    } else if (strcmp(cmd, "shutdown") == 0) {
        /* shutdownコマンドは専用のパケットを直接送信 */
        uint32_t dummy = 0;
        int cfd = ym_client_get_fd(c);
        ym_send_packet(cfd, YM_CMD_SHUTDOWN, 0, &dummy, sizeof(dummy));
        puts("シャットダウン要求を送信しました");
    } else if (strcmp(cmd, "demo") == 0) {
        ret = cmd_demo(c, argc - 1, argv + 1);
    } else {
        fprintf(stderr, "不明なコマンド: %s\n", cmd);
        print_usage();
        ret = 1;
    }

    ym_client_disconnect(c);
    return ret;
}
