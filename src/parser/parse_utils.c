#include "parser/parse_utils.h"
#include "core/panic.h"
#include "parser/ast_error.h"
#include "parser/ast_ptr.h"
#include "parser/ast_option.h"
#include "parser/ast_slice_type.h"
#include "parser/parse_expr.h"

#include <stdint.h>
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
    /* 不在这里打印：所有调用点都会用 ast_error_new 报出更具体的消息，
       并进入共享诊断缓冲区由 driver 统一打印。直接打印同一处错误会在
       输出里出现两条诊断（一条 fallback、一条 AST 错误）。 */
    return false;
}

bool expect_symbol(parser_t *p, const char *sym) {
    if (check_symbol(p, sym)) {
        advance(p);
        return true;
    }
    /* 同 expect_keyword：由调用点负责报错，避免 direct print + AST_ERROR
       双份诊断。 */
    return false;
}

ast_node_t *parse_reserved_ident_error(parser_t *p, const token_t *tok,
                                       const char *what) {
    if (!p || !tok) return NULL;
    strslice_t s = token_strslice(tok);
    const char *w = what ? what : "identifier";
    if (s.len >= 2 && s.ptr && s.ptr[0] == '_' && s.ptr[1] == '_') {
        p->has_error = true;
        return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos + 1,
                             "invalid %s '%.*s': identifiers starting with '__' "
                             "are reserved for compiler-generated names (and "
                             "future magic methods)",
                             w, (int)s.len, s.ptr);
    }
    if (s.len == 1 && s.ptr && s.ptr[0] == '_') {
        p->has_error = true;
        return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos + 1,
                             "invalid %s '_': '_' is the discard marker and "
                             "cannot be used as an identifier",
                             w);
    }
    return NULL;
}

/* ---- 诊断 ---- */

/* 解析器不再直接往 stderr 打印：所有语法错误都由 ast_error_new 记入共享
   诊断缓冲区，由 driver 在流水线出口统一打印一次（带源码片段）。 */

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

ast_node_t *parse_type_prefix(parser_t *p) {
    /* 裸 `*T` / `[]T` / `?T` 前缀（编译期所有权修饰符已移除，只保留裸指针）。 */
    if (check_symbol(p, "*")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        ast_node_t *base = parse_type_prefix(p);
        if (!base || base->kind == AST_ERROR) {
            return base ? base
                        : ast_error_new(p->diag, p->tokens, p->arena, tb,
                                        p->pos,
                                        "expected type after '*'");
        }
        ast_node_t *node = ast_ptr_new(p->arena, tb, p->pos);
        if (!node) return NULL;
        ((ast_ptr_t *)node)->base_type = base;
        return node;
    }
    if (check_symbol(p, "[")) {
        uint32_t save = p->pos;
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "]")) {
            advance(p);
            skip_trivia(p);
            ast_node_t *elem = parse_type_prefix(p);
            if (!elem || elem->kind == AST_ERROR) {
                return elem ? elem
                            : ast_error_new(p->diag, p->tokens, p->arena, tb,
                                            p->pos,
                                            "expected type after '[]'");
            }
            ast_node_t *node = ast_slice_type_new(p->arena, tb, p->pos);
            if (!node) return NULL;
            ((ast_slice_type_t *)node)->elem_type = elem;
            return node;
        }
        p->pos = save;
    }

    if (check_symbol(p, "?")) {
        uint32_t save = p->pos;
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "*") || check_symbol(p, "[")) {
            ast_node_t *sub = parse_type_prefix(p);
            if (!sub || sub->kind == AST_ERROR) return sub;
            ast_node_t *node = ast_option_new(p->arena, tb, p->pos);
            if (!node) return NULL;
            ((ast_option_t *)node)->sub = sub;
            return node;
        }
        p->pos = save;
    }

    return parse_expr_prec(p, 1);
}
ast_node_t *parse_raw_type_atom(parser_t *p) {
    if (!p) return NULL;


    if (check_symbol(p, "?")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        ast_node_t *sub = parse_raw_type_atom(p);
        if (!sub || sub->kind == AST_ERROR) return sub;
        ast_node_t *node = ast_option_new(p->arena, tb, p->pos);
        if (!node) return NULL;
        ((ast_option_t *)node)->sub = sub;
        return node;
    }
    if (check_symbol(p, "*")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        ast_node_t *base = parse_raw_type_atom(p);
        if (!base || base->kind == AST_ERROR) {
            return base ? base
                        : ast_error_new(p->diag, p->tokens, p->arena, tb,
                                        p->pos, "expected type after '*'");
        }
        ast_node_t *node = ast_ptr_new(p->arena, tb, p->pos);
        if (!node) return NULL;
        ((ast_ptr_t *)node)->base_type = base;
        return node;
    }
    if (check_symbol(p, "[")) {
        uint32_t save = p->pos;
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "]")) {
            advance(p);
            skip_trivia(p);
            ast_node_t *elem = parse_raw_type_atom(p);
            if (!elem || elem->kind == AST_ERROR) {
                return elem ? elem
                            : ast_error_new(p->diag, p->tokens, p->arena, tb,
                                            p->pos, "expected type after '[]'");
            }
            ast_node_t *node = ast_slice_type_new(p->arena, tb, p->pos);
            if (!node) return NULL;
            ((ast_slice_type_t *)node)->elem_type = elem;
            return node;
        }
        p->pos = save;
    }
    return parse_expr_prec(p, 22);
}
