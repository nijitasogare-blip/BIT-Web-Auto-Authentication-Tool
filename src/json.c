/* json.c - 轻量 JSON DOM（配置解析 + 门户返回解析，UTF-8） */
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

struct BWJson {
    JType  type;
    int    boolean;
    double num;
    char  *str;
    struct BWJson **items;
    int     count;
    char  **keys;
};

typedef struct {
    const char *p;
    wchar_t    err[256];
    int        errcap;
} JParser;

static void jerr(JParser *ps, const wchar_t *msg) {
    if (ps->err[0] == 0) {
        wcsncpy(ps->err, msg, ps->errcap - 1);
        ps->err[ps->errcap - 1] = 0;
    }
}

static void skip_ws(JParser *ps) {
    for (;;) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ps->p++; continue; }
        break;
    }
}

static BWJson *jnew(JType type) {
    BWJson *j = (BWJson *)calloc(1, sizeof(BWJson));
    if (j) j->type = type;
    return j;
}

void bw_json_free(BWJson *j) {
    if (!j) return;
    if (j->type == J_ARR || j->type == J_OBJ) {
        for (int i = 0; i < j->count; i++) bw_json_free(j->items[i]);
        free(j->items);
        if (j->keys) {
            for (int i = 0; i < j->count; i++) free(j->keys[i]);
            free(j->keys);
        }
    }
    free(j->str);
    free(j);
}

/* 把 (可能含转义的) JSON 字符串写入动态缓冲区 */
static int append_utf8(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 2 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        char *nb = (char *)realloc(*buf, ncap);
        if (!nb) return 0;
        *buf = nb;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = 0;
    return 1;
}

static int append_wchar_utf8(char **buf, size_t *len, size_t *cap, unsigned cp) {
    if (cp < 0x80) {
        return append_utf8(buf, len, cap, (char)cp);
    } else if (cp < 0x800) {
        if (!append_utf8(buf, len, cap, (char)(0xC0 | (cp >> 6)))) return 0;
        return append_utf8(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        if (!append_utf8(buf, len, cap, (char)(0xE0 | (cp >> 12)))) return 0;
        if (!append_utf8(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)))) return 0;
        return append_utf8(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    } else {
        if (!append_utf8(buf, len, cap, (char)(0xF0 | (cp >> 18)))) return 0;
        if (!append_utf8(buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)))) return 0;
        if (!append_utf8(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)))) return 0;
        return append_utf8(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    }
}

static char *parse_string(JParser *ps) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (*ps->p != '"') { jerr(ps, L"字符串缺少引号"); return NULL; }
    ps->p++;
    if (!append_utf8(&buf, &len, &cap, 0)) return NULL;
    buf[0] = 0;
    len = 0;
    while (*ps->p) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') { ps->p++; return buf; }
        if (c < 0x20) { jerr(ps, L"字符串包含非法控制字符"); free(buf); return NULL; }
        if (c == '\\') {
            ps->p++;
            char e = *ps->p;
            switch (e) {
                case '"': append_utf8(&buf, &len, &cap, '"'); ps->p++; break;
                case '\\': append_utf8(&buf, &len, &cap, '\\'); ps->p++; break;
                case '/': append_utf8(&buf, &len, &cap, '/'); ps->p++; break;
                case 'b': append_utf8(&buf, &len, &cap, '\b'); ps->p++; break;
                case 'f': append_utf8(&buf, &len, &cap, '\f'); ps->p++; break;
                case 'n': append_utf8(&buf, &len, &cap, '\n'); ps->p++; break;
                case 'r': append_utf8(&buf, &len, &cap, '\r'); ps->p++; break;
                case 't': append_utf8(&buf, &len, &cap, '\t'); ps->p++; break;
                case 'u': {
                    unsigned cp = 0;
                    if (sscanf(ps->p + 1, "%4x", &cp) != 1) {
                        jerr(ps, L"\\u 转义格式错误"); free(buf); return NULL;
                    }
                    ps->p += 5;
                    /* 代理对 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        unsigned lo = 0;
                        if (sscanf(ps->p + 2, "%4x", &lo) == 1 && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            ps->p += 6;
                        }
                    }
                    append_wchar_utf8(&buf, &len, &cap, cp);
                    break;
                }
                default:
                    jerr(ps, L"未知转义字符"); free(buf); return NULL;
            }
            continue;
        }
        /* 普通 UTF-8 字节原样复制（含多字节序列） */
        append_utf8(&buf, &len, &cap, (char)c);
        ps->p++;
    }
    jerr(ps, L"字符串未闭合");
    free(buf);
    return NULL;
}

static BWJson *parse_value(JParser *ps, int depth);

static BWJson *parse_array(JParser *ps, int depth) {
    BWJson *j = jnew(J_ARR);
    if (!j) return NULL;
    ps->p++; /* [ */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return j; }
    for (;;) {
        BWJson *item = parse_value(ps, depth + 1);
        if (!item) { bw_json_free(j); return NULL; }
        BWJson **ni = (BWJson **)realloc(j->items, sizeof(BWJson *) * (j->count + 1));
        if (!ni) { bw_json_free(item); bw_json_free(j); return NULL; }
        j->items = ni;
        j->items[j->count++] = item;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
        if (*ps->p == ']') { ps->p++; return j; }
        jerr(ps, L"数组格式错误");
        bw_json_free(j);
        return NULL;
    }
}

static BWJson *parse_object(JParser *ps, int depth) {
    BWJson *j = jnew(J_OBJ);
    if (!j) return NULL;
    ps->p++; /* { */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return j; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string(ps);
        if (!key) { bw_json_free(j); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') { jerr(ps, L"对象缺少冒号"); free(key); bw_json_free(j); return NULL; }
        ps->p++;
        BWJson *item = parse_value(ps, depth + 1);
        if (!item) { free(key); bw_json_free(j); return NULL; }
        BWJson **ni = (BWJson **)realloc(j->items, sizeof(BWJson *) * (j->count + 1));
        char **nk = (char **)realloc(j->keys, sizeof(char *) * (j->count + 1));
        if (!ni || !nk) { free(key); bw_json_free(item); bw_json_free(j); return NULL; }
        j->items = ni;
        j->keys = nk;
        j->keys[j->count] = key;
        j->items[j->count] = item;
        j->count++;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return j; }
        jerr(ps, L"对象格式错误");
        bw_json_free(j);
        return NULL;
    }
}

static BWJson *parse_value(JParser *ps, int depth) {
    if (depth > 32) { jerr(ps, L"JSON 嵌套过深"); return NULL; }
    skip_ws(ps);
    char c = *ps->p;
    if (c == '"') {
        BWJson *j = jnew(J_STR);
        if (!j) return NULL;
        j->str = parse_string(ps);
        if (!j->str) { free(j); return NULL; }
        return j;
    }
    if (c == '{') return parse_object(ps, depth);
    if (c == '[') return parse_array(ps, depth);
    if (c == 't' && strncmp(ps->p, "true", 4) == 0) {
        BWJson *j = jnew(J_BOOL); if (j) { j->boolean = 1; } ps->p += 4; return j;
    }
    if (c == 'f' && strncmp(ps->p, "false", 5) == 0) {
        BWJson *j = jnew(J_BOOL); if (j) { j->boolean = 0; } ps->p += 5; return j;
    }
    if (c == 'n' && strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return jnew(J_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        double v = strtod(ps->p, &end);
        if (end == ps->p) { jerr(ps, L"数字格式错误"); return NULL; }
        BWJson *j = jnew(J_NUM);
        if (j) j->num = v;
        ps->p = end;
        return j;
    }
    jerr(ps, L"无法识别 JSON 值");
    return NULL;
}

BWJson *bw_json_parse(const char *text, wchar_t *err, int errcap) {
    JParser ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = text ? text : "";
    ps.err[0] = 0;
    ps.errcap = 256;
    BWJson *j = parse_value(&ps, 0);
    if (j) {
        skip_ws(&ps);
        if (*ps.p != 0) {
            bw_json_free(j);
            j = NULL;
            jerr(&ps, L"JSON 末尾存在多余内容");
        }
    }
    if (!j && err) wcsncpy(err, ps.err[0] ? ps.err : L"JSON 解析失败", errcap - 1);
    if (err) err[errcap - 1] = 0;
    return j;
}

static const BWJson *obj_get(const BWJson *j, const char *key) {
    if (!j || j->type != J_OBJ) return NULL;
    for (int i = 0; i < j->count; i++) {
        if (j->keys[i] && strcmp(j->keys[i], key) == 0) return j->items[i];
    }
    return NULL;
}

const BWJson *bw_json_get(const BWJson *j, const char *path) {
    const BWJson *cur = j;
    if (!path || !*path) return cur;
    const char *p = path;
    while (cur && *p) {
        char part[128];
        int n = 0;
        while (*p && *p != '.' && n < (int)sizeof(part) - 1) part[n++] = *p++;
        part[n] = 0;
        if (*p == '.') p++;
        if (cur->type == J_OBJ) {
            cur = obj_get(cur, part);
        } else if (cur->type == J_ARR) {
            char *end = NULL;
            long idx = strtol(part, &end, 10);
            if (!end || *end || idx < 0 || idx >= cur->count) return NULL;
            cur = cur->items[idx];
        } else {
            return NULL;
        }
    }
    return cur;
}

const char *bw_json_string(const BWJson *j, const char *path) {
    const BWJson *v = bw_json_get(j, path);
    return (v && v->type == J_STR) ? v->str : NULL;
}

int bw_json_int(const BWJson *j, const char *path, int def) {
    const BWJson *v = bw_json_get(j, path);
    if (!v) return def;
    if (v->type == J_NUM) return (int)(v->num + (v->num >= 0 ? 0.5 : -0.5));
    if (v->type == J_BOOL) return v->boolean;
    return def;
}

int bw_json_bool(const BWJson *j, const char *path, int def) {
    const BWJson *v = bw_json_get(j, path);
    if (!v) return def;
    if (v->type == J_BOOL) return v->boolean;
    if (v->type == J_NUM) return v->num != 0;
    return def;
}

const BWJson *bw_json_array_get(const BWJson *j, const char *path, int index) {
    const BWJson *arr = bw_json_get(j, path);
    if (!arr || arr->type != J_ARR || index < 0 || index >= arr->count) return NULL;
    return arr->items[index];
}

int bw_json_array_count(const BWJson *j, const char *path) {
    const BWJson *arr = bw_json_get(j, path);
    return (arr && arr->type == J_ARR) ? arr->count : 0;
}
