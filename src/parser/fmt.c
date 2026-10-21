#include "parser/fmt.h"
#include "parser/parser.h"
#include "parser/lexer.h"
#include "parser/parse_utils.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_import.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_reexport.h"
#include "parser/ast_block.h"
#include "parser/ast_defer.h"
#include "parser/ast_if.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_switch.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_for.h"
#include "parser/ast_foreach.h"
#include "parser/ast_return.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_unary.h"
#include "parser/ast_call.h"
#include "parser/ast_member.h"
#include "parser/ast_index.h"
#include "parser/ast_spread.h"
#include "parser/ast_array.h"
#include "parser/ast_tuple.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_fill.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_ident.h"
#include "parser/ast_const.h"
#include "parser/ast_volatile.h"
#include "parser/ast_option.h"
#include "parser/ast_func_type.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_ternary.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_alignof.h"
#include "parser/ast_typeof.h"
#include "parser/ast_undef.h"
#include "parser/ast_nil.h"
#include "parser/ast_error.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_slice.h"
#include "parser/ast_make.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_new.h"
#include "parser/ast_addr.h"
#include "parser/ast_deref.h"
#include "parser/ast_move.h"
#include "parser/ast_scope_annot.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "core/panic.h"

#include <string.h>

/* ================================================================ */
/* clux 源码格式化（AST-based）：按语法树递归渲染                     */
/* ================================================================ */
/*
 * 策略：内部跑 lexer → parser（recover_partial）→ AST，按节点种类递归
 * 渲染。输出只由语法决定，与源码书写风格无关（固定风格，无配置项）。
 *
 * 注释挂载：每个节点渲染前/后，把 token 池中落在 [tok_begin, tok_end) 内、
 * 尚未消费的 COMMENT / MULTILINE_COMMENT 按源码位置输出：
 *   - 与前一 token 同行的注释 → 行内（前一 token 后一个空格 + 注释文本）
 *   - 其余注释 → 独立行（按当前缩进输出，保留源码空行分组）
 *
 * 表达式重新加括号：AST 丢失源码括号（分组直接返回内层节点），渲染时按
 * 运算符优先级表决定是否加括号（AST_BINARY / AST_UNARY / AST_TERNARY /
 * AST_ASSIGN）。括号是幂等的：加括号后再解析仍是同一棵树，再渲染不变。
 *
 * 幂等性由"AST 结构不变 → 输出不变"保证。
 */


#include "fmt_internal.h"

static void sb_reserve(sb_t *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap : 256;
    while (nc < sb->len + extra + 1) nc *= 2;
    char *nb = (char *)allocator_new_ex(
        sb->alloc, "clux.parser.fmt.buf", nc, NULL, NULL, NULL, 1);
    if (sb->buf) {
        memcpy(nb, sb->buf, sb->len);
        allocator_free(sb->alloc, (void **)&sb->buf);
    }
    sb->buf = nb;
    sb->cap = nc;
}

void sb_put(sb_t *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
}

void sb_str(sb_t *sb, const char *s) { sb_put(sb, s, strlen(s)); }

void sb_ch(sb_t *sb, char c)         { sb_put(sb, &c, 1); }

void sb_indent(sb_t *sb, int level) {
    for (int i = 0; i < level; i++) sb_str(sb, CLUX_INDENT);
}

/* ---- 注释挂载 ---- */

static bool tok_is_comment(const token_t *t) {
    if (!t) return false;
    token_kind_t k = token_get_kind(t);
    return k == TOKEN_TYPE_COMMENT || k == TOKEN_TYPE_MULTILINE_COMMENT;
}

/* i 下标处注释是否已消费 */
static bool comment_done(const fmt_t *f, size_t i) {
    if (i >= f->comment_bytes * 8) return true;
    return (f->comment_seen[i / 8] >> (i % 8)) & 1u;
}

static void comment_mark(fmt_t *f, size_t i) {
    if (i >= f->comment_bytes * 8) return;
    f->comment_seen[i / 8] |= (uint8_t)(1u << (i % 8));
}

/* token 在源码中的起始行号（无 location 时返回 0） */
size_t tok_line(const token_t *t) {
    const location_t *loc = token_get_location(t);
    return loc ? loc->begin.line : 0;
}

/* 输出注释 item：行内（inline=true，前面补一个空格）或独立行。
 * 行注释（//）后强制换行（到行尾结束）；块注释后由调用方 gap 收尾决定。 */
static void emit_comment(fmt_t *f, size_t i, const token_t *t, bool inline_,
                         int indent) {
    size_t tlen = 0;
    const char *text = token_get_text(t, &tlen);
    if (inline_) {
        if (f->at_line_start) {
            sb_indent(&f->sb, indent);
        } else {
            sb_ch(&f->sb, ' ');
        }
    } else {
        if (!f->at_line_start) sb_ch(&f->sb, '\n');
        sb_indent(&f->sb, indent);
    }
    sb_put(&f->sb, text, tlen);
    comment_mark(f, i);
    f->at_line_start = false;
    if (token_get_kind(t) == TOKEN_TYPE_COMMENT) {
        sb_ch(&f->sb, '\n');
        f->at_line_start = true;
    }
}

/* 挂载 [begin, end) 内的注释。
 *   - 行内注释（源码行号 <= last_line：与上一输出同行）→ 前置空格追加
 *   - 独立行注释（行号 > last_line）→ 换行 + 当前缩进输出
 * inline_only=true 时只挂行内注释，独立行注释跳过（留给后续挂载）。
 * 返回最后挂载注释的源码行号（无注释返回传入的 last_line）。
 * 全量扫描 + 位图防重（comment_seen），无游标推进需求。 */
size_t mount_comments(fmt_t *f, uint32_t begin, uint32_t end,
                             size_t last_line, bool inline_only) {
    size_t n = vec_len(f->tokens);
    for (size_t i = begin; i < end && i < n; i++) {
        const token_t *t = (const token_t *)vec_get(f->tokens, i);
        if (!tok_is_comment(t) || comment_done(f, i)) continue;
        size_t line = tok_line(t);
        if (line > last_line) {
            if (inline_only) continue;
            /* 空行保留：注释与上次输出间隔 ≥2 行 → 先补空行 */
            if (line >= last_line + 2) {
                if (!f->at_line_start) sb_ch(&f->sb, '\n');
                sb_ch(&f->sb, '\n');
                f->at_line_start = true;
            }
            emit_comment(f, i, t, /*inline_=*/false, f->indent);
        } else {
            emit_comment(f, i, t, /*inline_=*/true, f->indent);
        }
        last_line = line;
    }
    return last_line;
}

/* ---- 兄弟链渲染 ---- */

/* 以 ", " 分隔渲染兄弟链（用于参数/实参/元素/字段/索引列表）。 */
void render_comma_list(fmt_t *f, ast_node_t *head) {
    bool first = true;
    for (ast_node_t *n = head; n; n = n->next) {
        if (!first) sb_str(&f->sb, ", ");
        render_expr(f, n);
        first = false;
    }
}

/* 以 "; " 分隔渲染兄弟链（struct 字段列表，保持单行紧凑）。 */
void render_semi_list(fmt_t *f, ast_node_t *head) {
    bool first = true;
    for (ast_node_t *n = head; n; n = n->next) {
        if (!first) sb_str(&f->sb, "; ");
        render_node(f, n);
        first = false;
    }
}

/* ---- 构造表达式字段渲染 ---- */

/* 在 token 池 [begin, end) 范围内查找指定单字符标点符号的下标。
 * 用于定位构造表达式的 '{' / '}'（节点 tok_begin/tok_end 覆盖整个表达式，
 * 但字段渲染需要花括号位置做注释挂载边界）。未找到返回 end。 */
uint32_t find_punct_token(fmt_t *f, uint32_t begin, uint32_t end,
                                 const char *sym) {
    size_t n = vec_len(f->tokens);
    for (uint32_t i = begin; i < end && i < n; i++) {
        const token_t *t = (const token_t *)vec_get(f->tokens, i);
        if (token_is(t, sym)) return i;
    }
    return end;
}

/* 统计兄弟链节点数。 */
size_t sibling_count(ast_node_t *head) {
    size_t n = 0;
    for (ast_node_t *p = head; p; p = p->next) n++;
    return n;
}

/* ---- 公共入口 ---- */

/** 语法错误时的安全回退：返回原始源码的副本（不输出可能丢失数据的部分 AST）。 */
static char *fmt_return_orig(allocator_t *alloc, const char *src, size_t len,
                              size_t *out_len) {
    char *out = (char *)allocator_new_ex(
        alloc, "clux.parser.fmt.out", len + 1, NULL, NULL, NULL, 1);
    if (out) {
        memcpy(out, src, len);
        out[len] = '\0';
        if (out_len) *out_len = len;
    }
    return out;
}

char *fmt_format_source(allocator_t *alloc, const char *src, size_t len,
                        size_t *out_len) {
    if (!alloc || !src) return NULL;
    if (out_len) *out_len = 0;

    /* ① lexer：内存源 → token 池（含 trivia，注释挂载用）。
       lexer 持有 stream，lexer_close 的 dispose 一并关闭（勿重复关闭）。 */
    stream_source_t source = stream_source_mem(alloc, src, len, /*owns_data=*/false);
    istream_t *stream = istream_open(alloc, source);
    if (!stream) return NULL;

    lexer_t *lexer = lexer_create(alloc, stream, "<format>");
    if (!lexer) { istream_close(&stream); return NULL; }

    vec_t *pool = vec_new(alloc, /*owns_element=*/true);
    if (!pool) { lexer_close(&lexer); istream_close(&stream); return NULL; }

    bool lex_error = false;
    for (;;) {
        token_t *t = lexer_next(lexer);
        if (!t) break;
        vec_push(pool, alloc, t);
        token_kind_t k = token_get_kind(t);
        if (k == TOKEN_TYPE_ERROR) {
            lex_error = true;
            break;
        }
        if (k == TOKEN_TYPE_EOF) break;
    }
    /* lexer_close 的 dispose 会关闭 stream（含 source ctx），勿重复 istream_close */
    lexer_close(&lexer);
    if (lex_error) {
        vec_free(alloc, &pool);
        return NULL;
    }

    /* ② parser：recover_partial——语法错误时保留错误前的 AST */
    arena_t *arena = arena_new_default(alloc);
    if (!arena) { vec_free(alloc, &pool); return NULL; }

    parser_t *parser = parser_create(alloc, arena, pool);
    if (!parser) { arena_destroy(alloc, &arena); vec_free(alloc, &pool); return NULL; }
    parser->recover_partial = true;
    parser->diag = NULL;   /* 不打印诊断（仅格式化，错误由调用方感知） */

    ast_node_t *root = parser_parse(parser);
    if (!root) {
        /* 词法错误（parser_parse 内部已打印） */
        parser_destroy(&parser);
        arena_destroy(alloc, &arena);
        vec_free(alloc, &pool);
        return NULL;
    }
    /* 语法错误时，不输出部分 AST（会静默丢弃错误后的代码段），
       改为返回原始源码不变。错误已由 parse_error 打印到 stderr。 */
    if (parser->has_error) {
        parser_destroy(&parser);
        arena_destroy(alloc, &arena);
        vec_free(alloc, &pool);
        return fmt_return_orig(alloc, src, len, out_len);
    }
    /* ③ 渲染 */
    fmt_t f = { 0 };
    f.alloc = alloc;
    f.sb.alloc = alloc;   /* sb 增长缓冲独立持有 alloc */
    f.tokens = pool;
    f.indent = 0;
    f.at_line_start = true;
    size_t ntok = vec_len(pool);
    f.comment_bytes = (ntok + 7) / 8;
    f.comment_seen = (uint8_t *)allocator_new_ex(
        alloc, "clux.parser.fmt.comment", f.comment_bytes ? f.comment_bytes : 1,
        NULL, NULL, NULL, 1);
    if (!f.comment_seen) {
        parser_destroy(&parser);
        arena_destroy(alloc, &arena);
        vec_free(alloc, &pool);
        return NULL;
    }
    memset(f.comment_seen, 0, f.comment_bytes);

    render_program(&f, root);

    /* ④ 收尾：NUL 结尾输出缓冲 */
    char *out = NULL;
    if (f.sb.buf) {
        out = (char *)allocator_new_ex(alloc, "clux.parser.fmt.out",
                                       f.sb.len + 1, NULL, NULL, NULL, 1);
        if (out) {
            memcpy(out, f.sb.buf, f.sb.len);
            out[f.sb.len] = '\0';
            if (out_len) *out_len = f.sb.len;
        }
    } else {
        /* 空输入：输出空串 */
        out = (char *)allocator_new_ex(alloc, "clux.parser.fmt.out", 1,
                                       NULL, NULL, NULL, 1);
        if (out) out[0] = '\0';
        if (out_len) *out_len = 0;
    }
    if (f.sb.buf) allocator_free(alloc, (void **)&f.sb.buf);
    allocator_free(alloc, (void **)&f.comment_seen);

    parser_destroy(&parser);
    arena_destroy(alloc, &arena);
    vec_free(alloc, &pool);
    return out;
}
