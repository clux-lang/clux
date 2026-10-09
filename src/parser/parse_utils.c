#include "parser/parse_utils.h"
#include "core/panic.h"
#include "parser/ast_error.h"
#include "parser/ast_scope_annot.h"
#include "parser/parse_expr.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 游标操作 ---- */

const token_t *cur_token(const parser_t *p) {
    if (!p || !p->tokens) return NULL;
    if (p->pos >= vec_len(p->tokens)) {
        /* 返回最后一个 token（EOF） */
        return (const token_t *)vec_get(p->tokens, vec_len(p->tokens) - 1);
    }
    return (const token_t *)vec_get(p->tokens, p->pos);
}

const token_t *prev_token(const parser_t *p) {
    if (!p || !p->tokens || p->pos == 0) return NULL;
    return (const token_t *)vec_get(p->tokens, p->pos - 1);
}

const token_t *advance(parser_t *p) {
    if (!p) return NULL;
    const token_t *t = cur_token(p);
    if (p->pos < vec_len(p->tokens)) {
        p->pos++;
    }
    return t;
}

void skip_trivia(parser_t *p) {
    if (!p) return;
    while (p->pos < vec_len(p->tokens)) {
        const token_t *t = (const token_t *)vec_get(p->tokens, p->pos);
        token_kind_t kind = token_get_kind(t);
        if (kind != TOKEN_TYPE_WHITESPACE &&
            kind != TOKEN_TYPE_COMMENT &&
            kind != TOKEN_TYPE_MULTILINE_COMMENT) {
            break;
        }
        p->pos++;
    }
}

const token_t *advance_skip(parser_t *p) {
    const token_t *t = advance(p);
    skip_trivia(p);
    return t;
}

bool at_end(const parser_t *p) {
    if (!p || !p->tokens) return true;
    return token_get_kind(cur_token(p)) == TOKEN_TYPE_EOF;
}

/* ---- 查看（不消费） ---- */

bool check_keyword(const parser_t *p, const char *kw) {
    return token_get_kind(cur_token(p)) == TOKEN_TYPE_KEYWORD &&
           token_is(cur_token(p), kw);
}

bool check_symbol(const parser_t *p, const char *sym) {
    return token_get_kind(cur_token(p)) == TOKEN_TYPE_SYMBOL &&
           token_is(cur_token(p), sym);
}

bool check_kind(const parser_t *p, token_kind_t kind) {
    return token_get_kind(cur_token(p)) == kind;
}

/* ---- 移位运算符合成 ---- */

const token_t *synthesize_shift_token(parser_t *p) {
    if (!p) return NULL;
    const token_t *t0 = cur_token(p);
    if (token_get_kind(t0) != TOKEN_TYPE_SYMBOL) return NULL;
    strslice_t s0 = token_strslice(t0);
    if (s0.len != 1 || (s0.ptr[0] != '<' && s0.ptr[0] != '>')) return NULL;

    /* 下一个有效 token（跳过 trivia）必须同为该尖括号 */
    uint32_t save = p->pos;
    const token_t *t1 = advance(p);
    skip_trivia(p);
    const token_t *t2 = cur_token(p);
    bool match = token_get_kind(t2) == TOKEN_TYPE_SYMBOL &&
                 token_is(t2, s0.ptr[0] == '<' ? "<" : ">");
    (void)t1;
    p->pos = save; /* 不消费任何 token（含 trivia 游标恢复） */

    if (!match) return NULL;

    return arena_token_symbol(p->arena, s0.ptr[0] == '<' ? "<<" : ">>",
                              token_get_location(t0));
}

/* ---- 匹配与消费 ---- */

bool match_keyword(parser_t *p, const char *kw) {
    if (check_keyword(p, kw)) {
        advance(p);
        return true;
    }
    return false;
}

bool match_symbol(parser_t *p, const char *sym) {
    if (check_symbol(p, sym)) {
        advance(p);
        return true;
    }
    return false;
}

/* ---- 期望与报错 ---- */

bool expect_keyword(parser_t *p, const char *kw) {
    if (check_keyword(p, kw)) {
        advance(p);
        return true;
    }
    parse_error(p, "expected keyword '%s'", kw);
    return false;
}

bool expect_symbol(parser_t *p, const char *sym) {
    if (check_symbol(p, sym)) {
        advance(p);
        return true;
    }
    parse_error(p, "expected '%s'", sym);
    return false;
}

/* ---- 诊断 ---- */

void parse_error(parser_t *p, const char *fmt, ...) {
    if (!p) return;

    const token_t *t = cur_token(p);
    const location_t *loc = token_get_location(t);

    fprintf(stderr, "%s:%zu:%zu: error: ",
            loc ? loc->filename : "<unknown>",
            loc ? loc->begin.line : 0,
            loc ? loc->begin.column : 0);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

/* ---- Token → strslice 便捷 ---- */

strslice_t token_strslice(const token_t *t) {
    if (!t) return STRSLICE_EMPTY;
    size_t len = 0;
    const char *ptr = token_get_text(t, &len);
    return strslice_from_bytes(ptr, len);
}

/* ---- 字面量解析工具 ---- */

uint32_t parse_escape_seq(const char *text, size_t len, size_t *consumed) {
    if (len == 0) { *consumed = 0; return 0; }
    char c = text[0];
    *consumed = 1;
    switch (c) {
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '0':  return '\0';
        case 'x': {
            /* \xNN — 2 hex digits */
            if (len < 3) return 0;
            uint32_t v = 0;
            for (size_t i = 1; i <= 2; i++) {
                char h = text[i];
                int d = -1;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                if (d < 0) break;
                v = v * 16 + (uint32_t)d;
            }
            *consumed = 3;
            return v;
        }
        case 'u': {
            /* \uNNNN — 4 hex digits (BMP) */
            if (len < 5) return 0;
            uint32_t v = 0;
            for (size_t i = 1; i <= 4; i++) {
                char h = text[i];
                int d = -1;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                if (d < 0) break;
                v = v * 16 + (uint32_t)d;
            }
            *consumed = 5;
            return v;
        }
        case 'U': {
            /* \UNNNNNNNN — 8 hex digits (full Unicode) */
            if (len < 9) return 0;
            uint32_t v = 0;
            for (size_t i = 1; i <= 8; i++) {
                char h = text[i];
                int d = -1;
                if (h >= '0' && h <= '9') d = h - '0';
                else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                if (d < 0) break;
                v = v * 16 + (uint32_t)d;
            }
            *consumed = 9;
            return v;
        }
        default:
            return (unsigned char)c;  /* 未知转义，原样返回 */
    }
}

size_t utf8_encode_len(uint32_t cp) {
    if (cp < 0x80)    return 1;
    if (cp < 0x800)   return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}


/* ---- 类型标注解析 ---- */

/**
 * 解析作用域标注集合 '<a,b,c>'：'< 已由调用点确认（lexer 合成 "'<" 复合
 * SYMBOL token，lexer.c:580），消费该复合 token 后循环解析标识符（'*'
 * 记 global），直到 '>'。
 * 返回 names 数组（arena 分配）与 count；失败返回 NULL（诊断已报）。
 */
static strslice_t *parse_scope_annot_names(parser_t *p, size_t *out_count) {
    skip_trivia(p);

    if (check_symbol(p, ">")) {
        parse_error(p, "scope annotation cannot be empty");
        return NULL;
    }

    /* 两遍扫描：先数元素，再 arena 分配一次性数组（走 arena 通道，随
     * AST 生命周期释放，无 allocator 追踪负担）。 */
    size_t count = 0;
    uint32_t save = p->pos;
    for (;;) {
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER) &&
            !(check_symbol(p, "*") && count == 0)) {
            /* '*' 仅在首元素合法（'<*>' = global；'<*,a>' 可化简等价 '<a>'） */
            parse_error(p, "expected scope name in annotation");
            return NULL;
        }
        count++;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, ",")) {
            advance(p);
            skip_trivia(p);
            continue;
        }
        if (check_symbol(p, ">")) break;
        parse_error(p, "expected ',' or '>' in scope annotation");
        return NULL;
    }
    p->pos = save;

    strslice_t *names = (strslice_t *)arena_calloc(
        p->arena, count, sizeof(strslice_t), ALIGNOF(max_align_t));
    if (!names) return NULL;

    for (size_t i = 0; i < count; i++) {
        names[i] = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
        if (i + 1 < count) {
            advance(p); /* ',' */
            skip_trivia(p);
        }
    }

    if (!expect_symbol(p, ">")) {
        parse_error(p, "expected '>' after scope annotation");
        return NULL;
    }
    skip_trivia(p);

    *out_count = count;
    return names;
}

ast_node_t *parse_scope_annotated_type(parser_t *p) {
    /* lexer 把前导 `'<` 合成单符号 token（lexer.c:581，'< 紧跟 = 作用域
       标注前缀，非字符字面量）；此处匹配复合 token "'<"，非单 "'"。 */
    if (!check_symbol(p, "'<")) {
        return parse_expr_prec(p, 1);
    }

    uint32_t tb = p->pos;
    advance(p); /* 消费 '<（标注前缀） */
    skip_trivia(p);

    size_t count = 0;
    strslice_t *names = parse_scope_annot_names(p, &count);
    if (!names) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "invalid scope annotation");
    }

    /* 递归：'<a> '<b> i32 —— 标注可嵌套（消费层按序收敛） */
    ast_node_t *sub = parse_scope_annotated_type(p);
    if (!sub || sub->kind == AST_ERROR) {
        if (!sub) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected type after scope annotation");
        }
        return sub;
    }

    ast_node_t *node = ast_scope_annot_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_scope_annot_t *)node)->names = names;
    ((ast_scope_annot_t *)node)->count = count;
    ((ast_scope_annot_t *)node)->sub   = sub;
    return node;
}
