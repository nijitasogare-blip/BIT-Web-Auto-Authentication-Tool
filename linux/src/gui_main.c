/* linux/src/gui_main.c - bitweb-gui 入口 */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

wchar_t g_bitweb_config_path[BITWEB_MAX_PATH * 2];

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    char config_arg[BITWEB_MAX_PATH * 2] = { 0 };
    int silent = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--silent") == 0) silent = 1;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            strncpy(config_arg, argv[++i], sizeof(config_arg) - 1);
        else if (strncmp(argv[i], "--config=", 9) == 0)
            strncpy(config_arg, argv[i] + 9, sizeof(config_arg) - 1);
        else if (strcmp(argv[i], "--version") == 0) { printf("v%ls\n", BITWEB_VERSION); return 0; }
    }
    wchar_t wconfig[BITWEB_MAX_PATH * 2] = { 0 };
    if (config_arg[0]) bw_utf8_to_wide(config_arg, wconfig, sizeof(wconfig) / sizeof(wchar_t));
    bw_resolve_config_path(config_arg[0] ? wconfig : NULL, g_bitweb_config_path,
                           sizeof(g_bitweb_config_path) / sizeof(wchar_t));
    return bw_gui_run(silent);
}