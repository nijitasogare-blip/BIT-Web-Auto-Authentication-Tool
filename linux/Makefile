# Linux 版 BIT-Web 自动登录（纯 C；CLI/TUI 无外部依赖，GUI 需要 GTK3）
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lpthread

CORE_SRC = json crypto util config http wlan net srun status
CLI_SRC  = daemon tui main
GUI_SRC  = gui_main gui

CORE_OBJ = $(addprefix src/,$(addsuffix .o,$(CORE_SRC)))
CLI_OBJ  = $(addprefix src/,$(addsuffix .o,$(CLI_SRC)))
GUI_OBJ  = $(addprefix src/,$(addsuffix .o,$(GUI_SRC)))

GUI_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 ayatana-appindicator3-0.1 2>/dev/null)
GUI_LIBS   := $(shell pkg-config --libs gtk+-3.0 ayatana-appindicator3-0.1 2>/dev/null)

all: bitweb

bitweb: $(CORE_OBJ) $(CLI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# 可选：GTK3 桌面控制面板 + AppIndicator 托盘
gui: bitweb-gui
bitweb-gui: $(CORE_OBJ) $(GUI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(GUI_LIBS)

src/gui.o src/gui_main.o: CFLAGS += $(GUI_CFLAGS)

%.o: %.c include/bitweb.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: bitweb
	./bitweb crypto-test

clean:
	rm -f $(CORE_OBJ) $(CLI_OBJ) $(GUI_OBJ) bitweb bitweb-gui

.PHONY: all gui test clean
