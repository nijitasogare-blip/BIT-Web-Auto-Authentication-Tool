/* linux/src/main.c - Linux 入口：status / tui / self-test / set-password / daemon / ... */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include <termios.h>
#include <locale.h>

wchar_t g_bitweb_config_path[BITWEB_MAX_PATH * 2];

static void print_utf8(const wchar_t *text) {
    size_t len = wcslen(text);
    char *buf = (char *)malloc(len * 4 + 8);
    if (!buf) return;
    bw_wide_to_utf8(text, buf, (int)(len * 4 + 7));
    fputs(buf, stdout);
    fflush(stdout);
    free(buf);
}

static int prompt_line(const char *prompt, char *out, int cap, int hidden) {
    printf("%s", prompt);
    fflush(stdout);
    int is_tty = isatty(STDIN_FILENO);
    struct termios old, t;
    if (hidden && is_tty) {
        tcgetattr(STDIN_FILENO, &old);
        t = old;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    int ok = fgets(out, cap, stdin) != NULL;
    if (hidden && is_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        printf("\n");
    }
    if (!ok) return 0;
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return 1;
}

int bw_set_password(void) {
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        bw_config_defaults(&cfg);
    }
    printf("设置 BIT-Web 自动登录凭据（Linux 版：密码保存在 0600 权限的配置文件中）\n\n");
    if (!cfg.username[0]) {
        char user[256];
        if (!prompt_line("请输入学号/工号：", user, sizeof(user), 0) || !user[0]) {
            printf("错误：用户名不能为空\n");
            return 2;
        }
        bw_utf8_to_wide(user, cfg.username, 128);
    } else {
        char u[256];
        bw_wide_to_utf8(cfg.username, u, sizeof(u));
        printf("用户名：%s（如需修改请加 --username 参数或编辑 config.json）\n", u);
    }
    char p1[512], p2[512];
    if (!prompt_line("密码（输入不回显）：", p1, sizeof(p1), 1) || !p1[0]) {
        printf("错误：密码不能为空\n");
        return 2;
    }
    if (!prompt_line("再输入一次确认：", p2, sizeof(p2), 1) || strcmp(p1, p2) != 0) {
        printf("错误：两次输入不一致\n");
        return 2;
    }
    if (!bw_ascii_only(p1)) {
        printf("错误：密码包含非 ASCII 字符，当前协议实现不支持\n");
        return 2;
    }
    strncpy(cfg.password_plain, p1, sizeof(cfg.password_plain) - 1);
    cfg.password_plain[sizeof(cfg.password_plain) - 1] = 0;
    cfg.password_dpapi[0] = 0;
    if (!bw_config_save(&cfg, g_bitweb_config_path)) {
        printf("错误：保存配置失败\n");
        return 1;
    }
    printf("\n已保存（配置文件权限 0600）。守护进程会热加载，无需重启。\n");
    return 0;
}

int bw_cli_status(int with_network) {
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        wchar_t msg[2048];
        _snwprintf(msg, 2047, L"读取配置失败：%ls\n配置文件：%ls\n", err, g_bitweb_config_path);
        msg[2047] = 0;
        print_utf8(msg);
        return 1;
    }
    BWStatus st;
    bw_status_collect(&cfg, &st, with_network);
    int cap = 65536;
    wchar_t *buf = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
    if (!buf) return 1;
    bw_status_format(&st, buf, cap);
    print_utf8(buf);
    free(buf);
    return 0;
}

int bw_cli_selftest(void) {
    int cap = 65536;
    wchar_t *buf = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
    if (!buf) return 1;
    bw_selftest_build(buf, cap);
    print_utf8(buf);
    free(buf);
    return 0;
}

int bw_cli_crypto_test(void) {
    const char *token = "2494f3d9bdb1af7dfea63939126a202694282f6ebbea0c8cf8a7ab2cd7ff8fca";
    const char *user = "2024000001";
    const char *pass = "TestP@ss!234";
    const char *ac = "43";
    const char *ip = "10.0.0.2";
    const char *expect_hmd5 = "1243b7497e0b17501d1c0cdb6593a161";
    const char *expect_chksum = "beec28b6e734647a4e0233999b3501d527bf0d7e";
    const char *expect_info_prefix = "{SRBX1}VdcDUPC05+Qb9RFk9zZoF7boGLwmdUg9CnsMhjj0S+";
    int fails = 0;
    char hmd5[64], info[8192], chksum[64];
    if (!bw_srun_build_login(token, user, pass, ac, ip, hmd5, info, sizeof(info), chksum)) {
        printf("FAIL build_login\n");
        return 1;
    }
    char hmac_hex[33];
    bw_hmac_md5_hex("token", "password", hmac_hex);
    printf("%s hmac-md5(token,password) = %s\n",
           strcmp(hmac_hex, "fd07c716fa2b6a86413916082c8bfdc3") == 0 ? "OK  " : "FAIL", hmac_hex);
    if (strcmp(hmac_hex, "fd07c716fa2b6a86413916082c8bfdc3") != 0) fails++;
    printf("%s hmac-md5(login) = %s\n", strcmp(hmd5, expect_hmd5) == 0 ? "OK  " : "FAIL", hmd5);
    if (strcmp(hmd5, expect_hmd5) != 0) fails++;
    printf("%s chksum = %s\n", strcmp(chksum, expect_chksum) == 0 ? "OK  " : "FAIL", chksum);
    if (strcmp(chksum, expect_chksum) != 0) fails++;
    int info_ok = strncmp(info, expect_info_prefix, strlen(expect_info_prefix)) == 0;
    printf("%s info prefix = %.48s...\n", info_ok ? "OK  " : "FAIL", info);
    if (!info_ok) fails++;
    printf(fails ? "\n%d 项失败\n" : "\n全部通过\n", fails);
    return fails ? 1 : 0;
}

static int cli_login_test(void) {
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        printf("读取配置失败\n");
        return 1;
    }
    char password[512];
    if (!bw_config_password(&cfg, password, sizeof(password))) {
        printf("未配置密码，请先运行 set-password\n");
        return 1;
    }
    char user_utf8[256];
    bw_wide_to_utf8(cfg.username, user_utf8, sizeof(user_utf8));
    if (!bw_ascii_only(user_utf8) || !bw_ascii_only(password)) {
        printf("学号/密码含非 ASCII 字符\n");
        return 1;
    }
    BWPortal portal;
    if (!bw_srun_discover(&cfg, &portal, err, 256)) {
        printf("无法访问认证门户\n");
        return 1;
    }
    BWSrunResult r;
    int ok = bw_srun_login(&cfg, &portal, password, &r);
    if (ok && r.ok) {
        printf("登录请求成功%s（ip=%ls）\n",
               r.already_online ? "，门户提示该 IP 已在线" : "", portal.ip);
        return 0;
    }
    char msg[1024];
    bw_wide_to_utf8(r.message, msg, sizeof(msg));
    printf("登录失败：%s（network_error=%d）\n", msg, r.network_error);
    return 1;
}

static int cli_reconnect(void) {
    if (!bw_running_mutex_exists(BITWEB_DAEMON_MUTEX)) {
        printf("守护进程未运行（请先 systemctl --user start bitweb 或运行 install.sh）。\n");
        return 1;
    }
    if (bw_signal_daemon_wake()) {
        printf("已通知守护进程立即检查连通性；若断网会自动认证。\n");
        return 0;
    }
    printf("通知守护进程失败。\n");
    return 1;
}

static void usage(void) {
    printf("BIT-Web 自动登录（Linux 版）v%ls\n\n", BITWEB_VERSION);
    printf("用法：bitweb [--config PATH] [--simulate-ssid SSID] <命令>\n\n");
    printf("命令：\n");
    printf("  status        显示状态（含外网检测）\n");
    printf("  tui           终端控制面板（默认）\n");
    printf("  self-test     安装自检\n");
    printf("  set-password  设置/更新密码\n");
    printf("  crypto-test   协议签名离线测试\n");
    printf("  login-test    登录请求测试\n");
    printf("  reconnect     通知守护进程立即检查/重连\n");
    printf("  daemon        前台运行守护进程（systemd 使用）\n");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    char config_arg[BITWEB_MAX_PATH * 2] = { 0 };
    const char *command = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            strncpy(config_arg, argv[++i], sizeof(config_arg) - 1);
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            strncpy(config_arg, argv[i] + 9, sizeof(config_arg) - 1);
        } else if (strcmp(argv[i], "--simulate-ssid") == 0 && i + 1 < argc) {
            setenv("BITWEB_SIMULATE_SSID", argv[++i], 1);
        } else if (strncmp(argv[i], "--simulate-ssid=", 16) == 0) {
            setenv("BITWEB_SIMULATE_SSID", argv[i] + 16, 1);
        } else if (!command) {
            command = argv[i];
        }
    }
    wchar_t wconfig[BITWEB_MAX_PATH * 2] = { 0 };
    if (config_arg[0]) bw_utf8_to_wide(config_arg, wconfig, sizeof(wconfig) / sizeof(wchar_t));
    bw_resolve_config_path(config_arg[0] ? wconfig : NULL, g_bitweb_config_path,
                           sizeof(g_bitweb_config_path) / sizeof(wchar_t));

    if (!command) {
        if (isatty(STDIN_FILENO)) return bw_tui_run();
        return bw_cli_status(1);
    }
    if (strcmp(command, "status") == 0) return bw_cli_status(1);
    if (strcmp(command, "tui") == 0) return bw_tui_run();
    if (strcmp(command, "self-test") == 0) return bw_cli_selftest();
    if (strcmp(command, "set-password") == 0) return bw_set_password();
    if (strcmp(command, "crypto-test") == 0) return bw_cli_crypto_test();
    if (strcmp(command, "login-test") == 0) return cli_login_test();
    if (strcmp(command, "reconnect") == 0) return cli_reconnect();
    if (strcmp(command, "daemon") == 0) return bw_daemon_run(g_bitweb_config_path);
    if (strcmp(command, "--version") == 0) {
        printf("v%ls\n", BITWEB_VERSION);
        return 0;
    }
    usage();
    return 2;
}
