/* tui.c - 终端控制面板（原生 C，直接调用 API，不启动 Python） */
#include "bitweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <conio.h>
#include <shellapi.h>

static void tui_print(const wchar_t *text) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteConsoleW(h, text, (DWORD)wcslen(text), &written, NULL);
}

static void tui_clear(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(h, &csbi)) return;
    DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    DWORD written = 0;
    COORD home = { 0, 0 };
    FillConsoleOutputCharacterW(h, L' ', cells, home, &written);
    FillConsoleOutputAttribute(h, csbi.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(h, home);
}

static void pause_console(const wchar_t *prompt) {
    if (prompt) tui_print(prompt);
    tui_print(L"\n按任意键返回...");
    _getwch();
}

static void launch_sibling(const wchar_t *exe_name, const wchar_t *args) {
    wchar_t dir[BITWEB_MAX_PATH * 2], path[BITWEB_MAX_PATH * 2], cmd[BITWEB_MAX_PATH * 4];
    bw_get_exe_dir(dir, (int)(sizeof(dir) / sizeof(wchar_t)));
    swprintf(path, (int)(sizeof(path) / sizeof(wchar_t)), L"%ls\\%ls", dir, exe_name);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        tui_print(L"找不到同目录的程序：");
        tui_print(path);
        tui_print(L"\n");
        return;
    }
    swprintf(cmd, (int)(sizeof(cmd) / sizeof(wchar_t)), L"\"%ls\" %ls", path, args ? args : L"");
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        tui_print(L"启动失败。\n");
    }
}

static void restart_daemon(void) {
    HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, BITWEB_STOP_EVENT);
    if (stop) {
        SetEvent(stop);
        CloseHandle(stop);
        tui_print(L"已通知守护进程退出，正在重新启动...\n");
        Sleep(1500);
    }
    launch_sibling(L"bitweb.exe", L"daemon");
}

static void open_log(void) {
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        tui_print(L"读取配置失败，无法定位日志。\n");
        return;
    }
    wchar_t path[BITWEB_MAX_PATH * 2];
    bw_resolve_relative(g_bitweb_config_path, cfg.log_file, path,
                        (int)(sizeof(path) / sizeof(wchar_t)));
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        tui_print(L"日志文件还不存在：");
        tui_print(path);
        tui_print(L"\n");
        return;
    }
    ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

int bw_tui_run(void) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(hIn, &mode)) {
        printf("请在交互式控制台中运行 TUI。\n");
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    for (;;) {
        BWConfig cfg;
        wchar_t err[256] = { 0 };
        int ok = bw_config_load(&cfg, g_bitweb_config_path, err, 256);
        tui_clear();
        if (!ok) {
            tui_print(L"读取配置失败：");
            tui_print(err);
            tui_print(L"\n配置文件：");
            tui_print(g_bitweb_config_path);
            tui_print(L"\n");
        } else {
            BWStatus st;
            bw_status_collect(&cfg, &st, 1);
            int cap = 65536;
            wchar_t *buf = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
            if (buf) {
                bw_status_format(&st, buf, cap);
                tui_print(buf);
                free(buf);
            }
        }
        tui_print(L"\n----------------------------------------------------------------------\n");
        tui_print(L" [R]刷新（断网自动重连）  [S]自检  [P]设置密码  [L]打开日志  [G]托盘界面\n");
        tui_print(L" [D]重启守护进程  [O]安装目录  [Q]退出\n");
        tui_print(L" 请按键: ");
        wchar_t key = _getwch();
        if (key == L'q' || key == L'Q' || key == 27) {
            tui_clear();
            return 0;
        }
        if (key == L'r' || key == L'R' || key == L'\r') continue;
        if (key == L's' || key == L'S') {
            tui_clear();
            bw_cli_selftest();
            pause_console(NULL);
            continue;
        }
        if (key == L'p' || key == L'P') {
            tui_clear();
            bw_cli_set_password();
            pause_console(NULL);
            continue;
        }
        if (key == L'l' || key == L'L') { open_log(); continue; }
        if (key == L'g' || key == L'G') { launch_sibling(L"bitweb.exe", L""); continue; }
        if (key == L'd' || key == L'D') {
            tui_clear();
            restart_daemon();
            pause_console(NULL);
            continue;
        }
        if (key == L'o' || key == L'O') {
            wchar_t dir[BITWEB_MAX_PATH * 2];
            bw_get_exe_dir(dir, (int)(sizeof(dir) / sizeof(wchar_t)));
            ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
            continue;
        }
    }
}
