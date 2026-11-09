/* main_gui.c - bitweb.exe 入口：默认 GUI/托盘；参数 daemon 时运行无窗口守护进程 */
#include "bitweb.h"
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)hinst;
    (void)prev;
    (void)show;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *mode = L"gui";
    const wchar_t *config = NULL;
    int silent = 0;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"daemon") == 0) mode = L"daemon";
        else if (wcscmp(argv[i], L"gui") == 0) mode = L"gui";
        else if (wcscmp(argv[i], L"--silent") == 0) silent = 1;
        else if (wcscmp(argv[i], L"--config") == 0 && i + 1 < argc) config = argv[++i];
        else if (wcsncmp(argv[i], L"--config=", 9) == 0) config = argv[i] + 9;
    }
    bw_resolve_config_path(config, g_bitweb_config_path,
                           (int)(sizeof(g_bitweb_config_path) / sizeof(wchar_t)));

    int rc;
    if (wcscmp(mode, L"daemon") == 0) {
        rc = bw_daemon_run(g_bitweb_config_path);
    } else {
        rc = bw_gui_run(silent);
    }
    if (argv) LocalFree(argv);
    return rc;
}
