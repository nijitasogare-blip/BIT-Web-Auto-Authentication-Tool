/* linux/src/tui.c - 终端控制面板（termios + ANSI，无外部依赖） */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>
#include <ctype.h>

static void print_utf8(const wchar_t *text) {
    size_t len = wcslen(text);
    char *buf = (char *)malloc(len * 4 + 8);
    if (!buf) return;
    bw_wide_to_utf8(text, buf, (int)(len * 4 + 7));
    fputs(buf, stdout);
    free(buf);
}

static void tui_clear(void) {
    fputs("\x1b[2J\x1b[H", stdout);
    fflush(stdout);
}

static struct termios g_old_termios;
static int g_raw = 0;

static void term_raw(void) {
    if (g_raw) return;
    tcgetattr(STDIN_FILENO, &g_old_termios);
    struct termios t = g_old_termios;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = 1;
    fputs("\x1b[?25l", stdout);
}

static void term_restore(void) {
    if (!g_raw) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_old_termios);
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
    g_raw = 0;
}

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

static void pause_enter(void) {
    term_restore();
    printf("\n按回车返回...");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
    term_raw();
}

static void show_selftest(void) {
    wchar_t *buf = (wchar_t *)calloc(65536, sizeof(wchar_t));
    if (!buf) return;
    bw_selftest_build(buf, 65536);
    tui_clear();
    print_utf8(buf);
    free(buf);
}

static void show_log(void) {
    term_restore();
    BWConfig cfg;
    wchar_t err[256];
    if (bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        wchar_t path[BITWEB_MAX_PATH * 2];
        bw_resolve_relative(g_bitweb_config_path, cfg.log_file, path,
                            sizeof(path) / sizeof(wchar_t));
        char utf8[BITWEB_MAX_PATH * 4];
        bw_wide_to_utf8(path, utf8, sizeof(utf8));
        char cmd[BITWEB_MAX_PATH * 5];
        snprintf(cmd, sizeof(cmd), "less -R '%s' 2>/dev/null || cat '%s'", utf8, utf8);
        int rc = system(cmd);
        (void)rc;
    }
    term_raw();
}

static void restart_service(void) {
    term_restore();
    printf("\n正在重启 bitweb 服务...\n");
    int rc = system("systemctl --user restart bitweb 2>/dev/null");
    if (rc != 0) {
        wchar_t pidfile[BITWEB_MAX_PATH * 2];
        bw_runtime_file(L"daemon.pid", pidfile, sizeof(pidfile) / sizeof(wchar_t));
        (void)pidfile;
        printf("systemd 用户服务不可用，请先运行 linux/install.sh 安装。\n");
    }
    fflush(stdout);
    sleep(1);
    term_raw();
}

int bw_tui_run(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        printf("请在交互式终端中运行 TUI。\n");
        return 1;
    }
    int once = getenv("BITWEB_TUI_ONCE") != NULL;
    term_raw();
    for (;;) {
        BWConfig cfg;
        wchar_t err[256] = { 0 };
        tui_clear();
        if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
            wchar_t line[1024];
            _snwprintf(line, 1023, L"读取配置失败：%ls\n配置文件：%ls\n", err, g_bitweb_config_path);
            line[1023] = 0;
            print_utf8(line);
        } else {
            BWStatus st;
            bw_status_collect(&cfg, &st, 1);
            if (st.connectivity_checked && !st.online && st.in_whitelist) {
                bw_signal_daemon_wake();
            }
            int cap = 65536;
            wchar_t *buf = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
            if (buf) {
                bw_status_format(&st, buf, cap);
                print_utf8(buf);
                free(buf);
            }
        }
        print_utf8(L"\n----------------------------------------------------------------------\n");
        print_utf8(L" [R]刷新（断网自动重连）  [S]自检  [P]设置密码  [L]打开日志\n");
        print_utf8(L" [D]重启守护进程  [G]打开配置目录  [Q]退出\n");
        print_utf8(L" 请按键: ");
        fflush(stdout);
        if (once) {
            printf("\n");
            term_restore();
            return 0;
        }
        int key = read_key();
        if (key < 0) break;
        if (key == 'q' || key == 'Q' || key == 27) break;
        if (key == 'r' || key == 'R' || key == '\r' || key == '\n') continue;
        if (key == 's' || key == 'S') {
            show_selftest();
            pause_enter();
            continue;
        }
        if (key == 'p' || key == 'P') {
            term_restore();
            tui_clear();
            bw_set_password();
            pause_enter();
            term_raw();
            continue;
        }
        if (key == 'l' || key == 'L') { show_log(); continue; }
        if (key == 'd' || key == 'D') { restart_service(); pause_enter(); continue; }
        if (key == 'g' || key == 'G') {
            term_restore();
            BWConfig cfg;
            wchar_t err[256];
            if (bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
                wchar_t dir[BITWEB_MAX_PATH * 2];
                wcsncpy(dir, g_bitweb_config_path, sizeof(dir) / sizeof(wchar_t) - 1);
                wchar_t *slash = wcsrchr(dir, L'/');
                if (slash) *slash = 0;
                char utf8[BITWEB_MAX_PATH * 4];
                bw_wide_to_utf8(dir, utf8, sizeof(utf8));
                char cmd[BITWEB_MAX_PATH * 5];
                snprintf(cmd, sizeof(cmd), "xdg-open '%s' 2>/dev/null || ls -la '%s'", utf8, utf8);
                int rc = system(cmd);
                (void)rc;
            }
            term_raw();
            continue;
        }
    }
    tui_clear();
    term_restore();
    return 0;
}
