# ============================================================
# YM Runtime - Makefile
# ============================================================

CC      ?= gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic \
           -I./include -I./client
LDFLAGS :=

# PulseAudio
PA_CFLAGS  := $(shell pkg-config --cflags libpulse-simple 2>/dev/null || echo "")
PA_LIBS    := $(shell pkg-config --libs   libpulse-simple 2>/dev/null || echo "-lpulse-simple -lpulse")

# 数学ライブラリ
MATH_LIBS := -lm

# スレッドライブラリ
THREAD_LIBS := -lpthread

# ============================================================
# ソースファイル
# ============================================================

CORE_SRCS := \
    src/ym_core.c   \
    src/ym2203.c    \
    src/ym2151.c    \
    src/vgm_player.c

DAEMON_SRCS := \
    $(CORE_SRCS)    \
    src/ymruntimed.c

CLIENT_SRCS := \
    $(CORE_SRCS)    \
    client/ymclient.c

CTL_SRCS := \
    $(CLIENT_SRCS)  \
    tools/ymctl.c

# ============================================================
# ターゲット
# ============================================================

all: ymruntimed ymctl libymclient.a

# デーモン
ymruntimed: $(DAEMON_SRCS)
	$(CC) $(CFLAGS) $(PA_CFLAGS) -o $@ $^ \
	    $(PA_LIBS) $(MATH_LIBS) $(THREAD_LIBS)
	@echo "✓ ymruntimed ビルド完了"

# コントロールツール
ymctl: $(CTL_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ \
	    $(MATH_LIBS)
	@echo "✓ ymctl ビルド完了"

# スタティックライブラリ
CORE_OBJS := $(CORE_SRCS:.c=.o) client/ymclient.o

libymclient.a: $(CORE_OBJS)
	$(AR) rcs $@ $^
	@echo "✓ libymclient.a ビルド完了"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ============================================================
# インストール
# ============================================================

PREFIX ?= /usr/local

install: ymruntimed ymctl libymclient.a
	install -Dm755 ymruntimed     $(DESTDIR)$(PREFIX)/bin/ymruntimed
	install -Dm755 ymctl          $(DESTDIR)$(PREFIX)/bin/ymctl
	install -Dm644 libymclient.a  $(DESTDIR)$(PREFIX)/lib/libymclient.a
	install -Dm644 include/ym_core.h  $(DESTDIR)$(PREFIX)/include/ymruntime/ym_core.h
	install -Dm644 include/ym_ipc.h   $(DESTDIR)$(PREFIX)/include/ymruntime/ym_ipc.h
	install -Dm644 include/vgm_player.h $(DESTDIR)$(PREFIX)/include/ymruntime/vgm_player.h
	install -Dm644 client/ymclient.h  $(DESTDIR)$(PREFIX)/include/ymruntime/ymclient.h
	@echo "✓ インストール完了: $(DESTDIR)$(PREFIX)"

install-service: install
	@if command -v systemctl >/dev/null 2>&1; then \
	    install -Dm644 systemd/ymruntime.service \
	        $(HOME)/.config/systemd/user/ymruntime.service; \
	    systemctl --user daemon-reload; \
	    echo "✓ systemdサービスをインストールしました"; \
	    echo "  有効化: systemctl --user enable --now ymruntime"; \
	else \
	    echo "systemdが見つかりません。手動起動してください"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ymruntimed
	rm -f $(DESTDIR)$(PREFIX)/bin/ymctl
	rm -f $(DESTDIR)$(PREFIX)/lib/libymclient.a
	rm -rf $(DESTDIR)$(PREFIX)/include/ymruntime

# ============================================================
# テスト/ユーティリティ
# ============================================================

clean:
	rm -f ymruntimed ymctl libymclient.a $(CORE_OBJS) client/ymclient.o

# ビルドチェック (PulseAudioなしでコンパイル確認)
check:
	$(CC) $(CFLAGS) -c src/ym2203.c    -o /dev/null
	$(CC) $(CFLAGS) -c src/ym2151.c    -o /dev/null
	$(CC) $(CFLAGS) -c src/ym_core.c   -o /dev/null
	$(CC) $(CFLAGS) -c src/vgm_player.c -o /dev/null
	$(CC) $(CFLAGS) -c client/ymclient.c -o /dev/null
	@echo "✓ すべてのソースがコンパイル可能"

# デーモン起動ショートカット
run: ymruntimed
	./ymruntimed -f

.PHONY: all clean install install-service uninstall check run
