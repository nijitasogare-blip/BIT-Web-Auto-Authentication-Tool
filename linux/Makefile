# Linux 版 BIT-Web 自动登录（纯 C，无外部依赖）
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lpthread

SRC = src/json.c src/crypto.c src/util.c src/config.c src/http.c src/wlan.c \
      src/net.c src/srun.c src/status.c src/daemon.c src/tui.c src/main.c
OBJ = $(SRC:.c=.o)

all: bitweb

bitweb: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c include/bitweb.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: bitweb
	./bitweb crypto-test

clean:
	rm -f $(OBJ) bitweb

.PHONY: all test clean