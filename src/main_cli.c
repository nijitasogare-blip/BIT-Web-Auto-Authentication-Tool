/* main_cli.c - bitweb-cli.exe：status / self-test / set-password / tui / crypto-test */
#include "bitweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <conio.h>

static void print_utf8(const wchar_t *text) {
    size_t len = wcslen(text);
    char *buf = (char *)malloc(len * 4 + 8);
    if (!buf) return;
    bw_wide_to_utf8(text, buf, (int)(len * 4 + 7));
    fputs(buf, stdout);
    fflush(stdout);
    free(buf);
}

static void print_status_text(const BWStatus *st) {
    int cap = 65536;
    wchar_t *buf = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
    if (!buf) return;
    bw_status_format(st, buf, cap);
    print_utf8(buf);
    free(buf);
}

static int read_line_w(const wchar_t *prompt, wchar_t *out, int cap, int hidden) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0, read = 0;
    int is_console = GetConsoleMode(hIn, &mode) != 0;
    if (prompt && *prompt) {
        if (is_console) {
            DWORD written = 0;
            WriteConsoleW(hOut, prompt, (DWORD)wcslen(prompt), &written, NULL);
        } else {
            print_utf8(prompt);
        }
    }
    out[0] = 0;
    if (is_console) {
        DWORD old_mode = mode;
        if (hidden) {
            mode &= ~ENABLE_ECHO_INPUT;
            SetConsoleMode(hIn, mode);
        }
        wchar_t buf[1024];
        if (!ReadConsoleW(hIn, buf, 1023, &read, NULL)) {
            SetConsoleMode(hIn, old_mode);
            return 0;
        }
        buf[read] = 0;
        while (read > 0 && (buf[read - 1] == L'\r' || buf[read - 1] == L'\n')) buf[--read] = 0;
        wcsncpy(out, buf, cap - 1);
        out[cap - 1] = 0;
        if (hidden) {
            SetConsoleMode(hIn, old_mode);
            WriteConsoleW(hOut, L"\n", 1, &read, NULL);
        }
        return 1;
    }
    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) return 0;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    bw_utf8_to_wide(line, out, cap);
    return 1;
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
    print_status_text(&st);
    return 0;
}

static void print_check(const wchar_t *name, int ok, const wchar_t *detail) {
    wchar_t line[1024];
    _snwprintf(line, 1023, L"[%ls] %-10ls %ls\n", ok ? L" OK " : L"FAIL", name,
               detail ? detail : L"");
    line[1023] = 0;
    print_utf8(line);
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
int bw_cli_set_password(void) {
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        bw_config_defaults(&cfg);
    }
    print_utf8(L"设置 BIT-Web 自动登录凭据（密码用 Windows DPAPI 加密保存）\n\n");

    if (!cfg.username[0]) {
        wchar_t user[128];
        if (!read_line_w(L"请输入学号/工号：", user, 128, 0) || !user[0]) {
            print_utf8(L"错误：用户名不能为空\n");
            return 2;
        }
        wcsncpy(cfg.username, user, 127);
    } else {
        wchar_t line[256];
        _snwprintf(line, 255, L"用户名：%ls（如需修改请直接编辑 config.json）\n", cfg.username);
        print_utf8(line);
    }

    wchar_t pw[256], pw2[256];
    if (!read_line_w(L"密码（输入不回显）：", pw, 256, 1) || !pw[0]) {
        print_utf8(L"错误：密码不能为空\n");
        return 2;
    }
    if (!read_line_w(L"再输入一次确认：", pw2, 256, 1) || wcscmp(pw, pw2) != 0) {
        print_utf8(L"错误：两次输入不一致\n");
        return 2;
    }
    char pw_utf8[1024];
    bw_wide_to_utf8(pw, pw_utf8, sizeof(pw_utf8));
    if (!bw_ascii_only(pw_utf8)) {
        print_utf8(L"错误：密码包含非 ASCII 字符，当前协议实现不支持\n");
        return 2;
    }
    char b64[4096];
    if (!bw_dpapi_protect((const unsigned char *)pw_utf8, (int)strlen(pw_utf8), b64, sizeof(b64))) {
        print_utf8(L"错误：DPAPI 加密失败\n");
        return 1;
    }
    strncpy(cfg.password_dpapi, b64, sizeof(cfg.password_dpapi) - 1);
    cfg.password_dpapi[sizeof(cfg.password_dpapi) - 1] = 0;
    cfg.password_plain[0] = 0;
    if (!bw_config_save(&cfg, g_bitweb_config_path)) {
        print_utf8(L"错误：保存配置失败\n");
        return 1;
    }
    print_utf8(L"\n已加密保存。守护进程下次登录时会自动读取新密码（无需重启）。\n");
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
        print_utf8(L"读取配置失败\n");
        return 1;
    }
    char password[512];
    if (!bw_config_password(&cfg, password, sizeof(password))) {
        print_utf8(L"未配置密码，请先运行 set-password\n");
        return 1;
    }
    char user_utf8[256];
    bw_wide_to_utf8(cfg.username, user_utf8, sizeof(user_utf8));
    if (!bw_ascii_only(user_utf8) || !bw_ascii_only(password)) {
        print_utf8(L"学号/密码含非 ASCII 字符\n");
        return 1;
    }
    BWPortal portal;
    if (!bw_srun_discover(&cfg, &portal, err, 256)) {
        print_utf8(L"无法访问认证门户：");
        print_utf8(err);
        print_utf8(L"\n");
        return 1;
    }
    BWSrunResult r;
    int ok = bw_srun_login(&cfg, &portal, password, &r);
    wchar_t line[1024];
    if (ok && r.ok) {
        _snwprintf(line, 1023, L"登录请求成功%s（ip=%ls）\n",
                   r.already_online ? L"，门户提示该 IP 已在线" : L"", portal.ip);
        line[1023] = 0;
        print_utf8(line);
        return 0;
    }
    _snwprintf(line, 1023, L"登录失败：%ls（network_error=%d）\n",
               r.message, r.network_error);
    line[1023] = 0;
    print_utf8(line);
    return 1;
}
static int cli_reconnect(void) {
    if (!bw_running_mutex_exists(BITWEB_DAEMON_MUTEX)) {
        wchar_t dir[BITWEB_MAX_PATH * 2], exe[BITWEB_MAX_PATH * 2], cmd[BITWEB_MAX_PATH * 4];
        if (bw_get_exe_dir(dir, (int)(sizeof(dir) / sizeof(wchar_t)))) {
            swprintf(exe, (int)(sizeof(exe) / sizeof(wchar_t)), L"%ls\\bitweb.exe", dir);
            if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
                swprintf(cmd, (int)(sizeof(cmd) / sizeof(wchar_t)), L"\"%ls\" daemon", exe);
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si));
                si.cb = sizeof(si);
                memset(&pi, 0, sizeof(pi));
                if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    bw_sleep_ms(600);
                }
            }
        }
    }
    if (bw_signal_daemon_wake()) {
        print_utf8(L"已通知守护进程立即检查连通性；若断网会自动认证。\n");
        return 0;
    }
    print_utf8(L"守护进程未运行，无法通知（请先运行 install.ps1 或 bitweb.exe daemon）。\n");
    return 1;
}
static void usage(void) {
    wprintf(L"BIT-Web 自动登录（原生 C 版）v%ls\n\n", BITWEB_VERSION);
    wprintf(L"用法：bitweb-cli.exe [--config PATH] <命令>\n\n");
    wprintf(L"命令：\n");
    wprintf(L"  status        显示状态（含外网检测）\n");
    wprintf(L"  tui           终端控制面板\n");
    wprintf(L"  self-test     安装自检\n");
    wprintf(L"  set-password  设置/更新密码\n");
    wprintf(L"  crypto-test   协议签名离线测试\n");
    wprintf(L"  reconnect     通知守护进程立即检查/重连\n");
}

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    wchar_t config[BITWEB_MAX_PATH * 2] = { 0 };
    const wchar_t *command = NULL;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--config") == 0 && i + 1 < argc) {
            wcscpy(config, argv[++i]);
        } else if (wcsncmp(argv[i], L"--config=", 9) == 0) {
            wcscpy(config, argv[i] + 9);
        } else if (!command) {
            command = argv[i];
        }
    }
    bw_resolve_config_path(config[0] ? config : NULL, g_bitweb_config_path,
                           (int)(sizeof(g_bitweb_config_path) / sizeof(wchar_t)));

    if (!command) {
        usage();
        return 0;
    }
    if (wcscmp(command, L"status") == 0) return bw_cli_status(1);
    if (wcscmp(command, L"tui") == 0) return bw_tui_run();
    if (wcscmp(command, L"self-test") == 0) return bw_cli_selftest();
    if (wcscmp(command, L"login-test") == 0) return cli_login_test();
    if (wcscmp(command, L"reconnect") == 0) return cli_reconnect();
    if (wcscmp(command, L"set-password") == 0) return bw_cli_set_password();
    if (wcscmp(command, L"crypto-test") == 0) return bw_cli_crypto_test();
    usage();
    return 2;
}
