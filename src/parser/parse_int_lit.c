#include "parser/ast_int_lit.h"
#include "parser/ast_error.h"
#include "parser/parse_utils.h"

#include <stdint.h>
#include <string.h>

/* ---- 判断数字是否浮点 ---- */

bool numeric_is_float(const char *text, size_t len) {
    if (!text || len == 0) return false;

    /* 0x/0o/0b 前缀的总是整数（浮点不支持进制前缀） */
    if (len >= 2 && text[0] == '0') {
        char c = text[1];
        if (c == 'x' || c == 'X' || c == 'o' || c == 'O' || c == 'b' || c == 'B') {
            return false;
        }
    }

    /* 遇到 . 或 e/E 即为浮点（数字 token 不含后缀，后缀是独立 token） */
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '.') return true;
        if (c == 'e' || c == 'E') return true;
    }
    return false;
}

/* ---- 内部辅助 ---- */

/** 整数类型后缀白名单及其值域 */
typedef struct {
    const char *name;
    uint64_t    max_val;   /* 无符号最大值 */
    bool        is_signed;
} int_type_info_t;

static const int_type_info_t kIntTypes[] = {
    {"i8",  127ULL,   true},
    {"i16", 32767ULL, true},
    {"i32", 2147483647ULL, true},
    {"i64", 9223372036854775807ULL, true},
    {"u8",  255ULL,              false},
    {"u16", 65535ULL,            false},
    {"u32", 4294967295ULL,       false},
    {"u64", UINT64_MAX, false},
};

static const int_type_info_t *find_int_type(strslice_t s) {
    for (size_t i = 0; i < sizeof(kIntTypes) / sizeof(kIntTypes[0]); i++) {
        if (strslice_eq(s, strslice_from_cstr(kIntTypes[i].name)))
            return &kIntTypes[i];
    }
    return NULL;
}

/**
 * 解析整数字面量的值（数字 token 不含后缀）。
 */
static uint64_t parse_int_value(strslice_t text) {
    const char *p = text.ptr;
    size_t len = text.len;

    int base = 10;
    size_t start = 0;
    if (len >= 2 && p[0] == '0') {
        char c = p[1];
        if (c == 'x' || c == 'X') { base = 16; start = 2; }
        else if (c == 'o' || c == 'O') { base = 8; start = 2; }
        else if (c == 'b' || c == 'B') { base = 2; start = 2; }
    }

    uint64_t value = 0;
    for (size_t i = start; i < len; i++) {
        char c = p[i];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;

        if (digit < 0 || digit >= base) break;
        value = value * (unsigned)base + (uint64_t)digit;
    }
    return value;
}

/* ---- parse_int_lit ---- */

ast_node_t *parse_int_lit(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_kind(p, TOKEN_TYPE_NUMERIC)) { p->pos = tb; return NULL; }
    const token_t *t = cur_token(p);
    strslice_t text = token_strslice(t);

    if (numeric_is_float(text.ptr, text.len)) { p->pos = tb; return NULL; }

    advance(p);
    skip_trivia(p);

    uint64_t value = parse_int_value(text);

    /* 检查紧跟的类型后缀（类型名是 keyword token） */
    strslice_t type = STRSLICE_EMPTY;
    if (check_kind(p, TOKEN_TYPE_KEYWORD)) {
        strslice_t next = token_strslice(cur_token(p));
        const int_type_info_t *ti = find_int_type(next);
        if (ti) {
            /* 校验 value 是否在 type 值域内 */
            if (value > ti->max_val) {
                advance(p);
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "integer literal %llu out of range for type %.*s",
                                     (unsigned long long)value, (int)next.len, next.ptr);
            }
            type = next;
            advance(p);
        }
    }

    ast_node_t *node = ast_int_lit_new(p->arena, tb, p->pos);
    ((ast_int_lit_t *)node)->value = value;
    ((ast_int_lit_t *)node)->type  = type;
    return node;
}
