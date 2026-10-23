#include "parser/ast_struct_def.h"
#include "parser/ast_array.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_error.h"
#include "parser/parse_utils.h"

#include <stdint.h>
#include <string.h>

static strslice_t arena_name(parser_t *p, const char *s) {
    size_t n = strlen(s);
    char *buf = (char *)arena_alloc(p->arena, n ? n : 1, 1);
    if (!buf) return STRSLICE_EMPTY;
    if (n) memcpy(buf, s, n);
    return strslice_from_bytes(buf, n);
}

static uint32_t word_bits_of(strslice_t name) {
    if (name.len == 2 && memcmp(name.ptr, "u8", 2) == 0) return 8;
    if (name.len == 3 && memcmp(name.ptr, "u16", 3) == 0) return 16;
    if (name.len == 3 && memcmp(name.ptr, "u32", 3) == 0) return 32;
    if (name.len == 3 && memcmp(name.ptr, "u64", 3) == 0) return 64;
    return 0;
}

/* bitfield Name: u32 { a: 3; b: 5; } */
ast_node_t *parse_bitfield_def(parser_t *p) {
    uint32_t tb = p->pos;
    advance(p); /* bitfield */
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected bitfield name after 'bitfield'");
    }
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "bitfield name");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, ":")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ':' after bitfield name");
    }
    skip_trivia(p);
    token_kind_t uk = token_get_kind(cur_token(p));
    if (uk != TOKEN_TYPE_IDENTIFIER && uk != TOKEN_TYPE_KEYWORD) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected unsigned integer type after ':'");
    }
    strslice_t uname = token_strslice(cur_token(p));
    uint32_t word_bits = word_bits_of(uname);
    if (word_bits == 0) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "bitfield underlying type must be u8/u16/u32/u64");
    }
    ast_node_t *underlying = ast_ident_new(p->arena, p->pos, p->pos + 1);
    if (!underlying) return NULL;
    ((ast_ident_t *)underlying)->name = uname;
    advance(p);
    skip_trivia(p);
    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start bitfield field list");
    }
    skip_trivia(p);

    const size_t max_fields = 128;
    ast_bitfield_field_t *bf = (ast_bitfield_field_t *)arena_calloc(
        p->arena, max_fields, sizeof(*bf), ALIGNOF(max_align_t));
    if (!bf) return NULL;
    size_t nf = 0;
    uint64_t total = 0;

    while (!check_symbol(p, "}")) {
        if (nf >= max_fields) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "too many bitfield fields (max 128)");
        }
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos + 1,
                                 "expected bitfield field name");
        }
        {
            ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "bitfield field");
            if (rerr) return rerr;
        }
        strslice_t fname = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
        if (!expect_symbol(p, ":")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ':' after bitfield field name");
        }
        skip_trivia(p);
        ast_node_t *wnode = parse_int_lit(p);
        if (!wnode || wnode->kind == AST_ERROR || wnode->kind != AST_INT_LIT) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "bitfield field width must be an integer literal");
        }
        uint64_t width = ((ast_int_lit_t *)wnode)->value;
        if (width == 0 || width > word_bits) {
            return ast_error_new(p->diag, p->tokens, p->arena, wnode->tok_begin,
                                 wnode->tok_end,
                                 "bitfield field width must be in 1..%u", word_bits);
        }
        for (size_t i = 0; i < nf; i++) {
            if (bf[i].name.len == fname.len &&
                memcmp(bf[i].name.ptr, fname.ptr, fname.len) == 0) {
                return ast_error_new(p->diag, p->tokens, p->arena, wnode->tok_begin,
                                     wnode->tok_end, "duplicate bitfield field '%.*s'",
                                     (int)fname.len, fname.ptr);
            }
        }
        if (total + width > UINT32_MAX) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "bitfield total size is too large");
        }
        bf[nf].name = fname;
        bf[nf].offset = (uint32_t)total;
        bf[nf].width = (uint32_t)width;
        nf++;
        total += width;
        skip_trivia(p);
        if (check_symbol(p, ";")) { advance(p); skip_trivia(p); }
    }
    if (nf == 0) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "bitfield must declare at least one field");
    }
    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to end bitfield");
    }

    /* 隐藏存储字段：__bits: [word_count]underlying */
    uint64_t wc64 = (total + word_bits - 1) / word_bits;
    if (wc64 == 0 || wc64 > UINT32_MAX) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "bitfield storage size is too large");
    }
    uint32_t wc = (uint32_t)wc64;
    ast_node_t *len = ast_int_lit_new(p->arena, tb, p->pos);
    ast_node_t *arr = ast_array_new(p->arena, tb, p->pos);
    ast_node_t *hfield = ast_struct_field_new(p->arena, tb, p->pos);
    if (!len || !arr || !hfield) return NULL;
    ((ast_int_lit_t *)len)->value = wc;
    ((ast_int_lit_t *)len)->type = STRSLICE_LIT("u64");
    ((ast_array_t *)arr)->base_type = underlying;
    ((ast_array_t *)arr)->length = len;
    ((ast_struct_field_t *)hfield)->name = arena_name(p, "__bits");
    ((ast_struct_field_t *)hfield)->type = arr;

    ast_node_t *node = ast_struct_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ast_struct_def_t *sd = (ast_struct_def_t *)node;
    sd->name = name;
    sd->fields = hfield;
    sd->fields_last = hfield;
    sd->is_bitfield = true;
    sd->bitfield_underlying = underlying;
    sd->bitfield_word_bits = word_bits;
    sd->bitfield_total_bits = (uint32_t)total;
    sd->bitfield_word_count = wc;
    sd->bitfield_field_count = nf;
    sd->bitfield_fields = bf;
    return node;
}
