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

#define CLUX_INDENT "    "

/* ---- 增长式输出缓冲 ---- */

typedef struct {
    allocator_t *alloc;
    char        *buf;
    size_t       len;
    size_t       cap;
} sb_t;

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

static void sb_put(sb_t *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
}

static void sb_str(sb_t *sb, const char *s) { sb_put(sb, s, strlen(s)); }
static void sb_ch(sb_t *sb, char c)         { sb_put(sb, &c, 1); }

static void sb_indent(sb_t *sb, int level) {
    for (int i = 0; i < level; i++) sb_str(sb, CLUX_INDENT);
}

/* ---- formatter 上下文 ---- */

typedef struct {
    allocator_t *alloc;
    vec_t       *tokens;   /* token 池（含 trivia；注释挂载用） */
    sb_t         sb;
    int          indent;   /* 当前缩进层 */
    bool         at_line_start;
    /* 注释消费位图（bit per token 下标，1 = 已输出） */
    uint8_t     *comment_seen;
    size_t       comment_bytes;
    size_t       src_line;     /* 当前输出位置对应的源码行号（行内/独立行判定） */
} fmt_t;
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
static size_t tok_line(const token_t *t) {
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
static size_t mount_comments(fmt_t *f, uint32_t begin, uint32_t end,
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

/* ---- 渲染器前向声明 ---- */

static void render_node(fmt_t *f, ast_node_t *node);
static void render_expr(fmt_t *f, ast_node_t *node);
static void render_stmt(fmt_t *f, ast_node_t *node);
static void render_block(fmt_t *f, ast_node_t *node);
static void render_func_def(fmt_t *f, ast_node_t *node);
static void render_func_literal(fmt_t *f, ast_node_t *node);
static void render_captures(fmt_t *f, ast_node_t *captures);
static void render_scope_annot_names(fmt_t *f, const strslice_t *names,
                                     size_t count);

/* ---- 兄弟链渲染 ---- */

/* 以 ", " 分隔渲染兄弟链（用于参数/实参/元素/字段/索引列表）。 */
static void render_comma_list(fmt_t *f, ast_node_t *head) {
    bool first = true;
    for (ast_node_t *n = head; n; n = n->next) {
        if (!first) sb_str(&f->sb, ", ");
        render_expr(f, n);
        first = false;
    }
}

/* 以 "; " 分隔渲染兄弟链（struct 字段列表，保持单行紧凑）。 */
static void render_semi_list(fmt_t *f, ast_node_t *head) {
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
static uint32_t find_punct_token(fmt_t *f, uint32_t begin, uint32_t end,
                                 const char *sym) {
    size_t n = vec_len(f->tokens);
    for (uint32_t i = begin; i < end && i < n; i++) {
        const token_t *t = (const token_t *)vec_get(f->tokens, i);
        if (token_is(t, sym)) return i;
    }
    return end;
}

/* 统计兄弟链节点数。 */
static size_t sibling_count(ast_node_t *head) {
    size_t n = 0;
    for (ast_node_t *p = head; p; p = p->next) n++;
    return n;
}

/* 构造表达式字段渲染：struct 具名字段构造（AST_CONSTRUCT_FIELD）≥2 字段时
 * 多行展开（每字段独占一行，4 空格缩进，注释按源码位置挂载）；单字段或
 * 数组/元组值构造（普通值表达式）保持单行紧凑 `{ v1, v2 }`。
 *
 * 区分依据：struct 构造字段是 AST_CONSTRUCT_FIELD（.name = value），数组/
 * 元组构造字段是普通值表达式。只有具名字段构造才多行展开。
 *
 * open_tok / close_tok 是 '{' / '}' 在 token 池中的下标，用于注释挂载范围。 */
static void render_construct_fields(fmt_t *f, ast_node_t *head,
                                    uint32_t open_tok, uint32_t close_tok) {
    if (!head) {
        /* 空构造：紧凑 {} */
        return;
    }
    /* 非具名字段构造（数组/元组值）：保持单行紧凑 */
    if (head->kind != AST_CONSTRUCT_FIELD) {
        sb_ch(&f->sb, ' ');
        render_comma_list(f, head);
        sb_ch(&f->sb, ' ');
        return;
    }
    size_t nf = sibling_count(head);
    if (nf < 2) {
        /* 单字段：单行紧凑 { .f = v } */
        sb_ch(&f->sb, ' ');
        render_comma_list(f, head);
        sb_ch(&f->sb, ' ');
        return;
    }
    /* 多字段：多行展开 */
    f->indent++;
    uint32_t prev_end = open_tok + 1;  /* '{' 之后 */
    size_t last_line = tok_line(
        (const token_t *)vec_get(f->tokens, open_tok));
    for (ast_node_t *fld = head; fld; fld = fld->next) {
        /* 字段前换行 + 注释挂载 */
        if (!f->at_line_start) sb_ch(&f->sb, '\n');
        f->at_line_start = true;
        last_line = mount_comments(f, prev_end, fld->tok_begin, last_line,
                                   /*inline_only=*/false);
        /* 空行保留 */
        size_t cur_line = tok_line(
            (const token_t *)vec_get(f->tokens, fld->tok_begin));
        if (cur_line >= last_line + 2) {
            sb_ch(&f->sb, '\n');
            f->at_line_start = true;
        }
        if (f->at_line_start) sb_indent(&f->sb, f->indent);
        f->at_line_start = false;
        render_expr(f, fld);
        /* 非末尾字段补 ',' */
        if (fld->next) sb_ch(&f->sb, ',');
        prev_end = fld->tok_end;
        /* 字段后行内注释 */
        uint32_t inline_end = fld->next ? fld->next->tok_begin : close_tok;
        size_t fld_end_line = tok_line(
            (const token_t *)vec_get(f->tokens, prev_end - 1));
        last_line = mount_comments(f, prev_end, inline_end, fld_end_line,
                                   /*inline_only=*/true);
        last_line = fld_end_line;
    }
    /* 块尾注释 + '}' */
    if (!f->at_line_start) sb_ch(&f->sb, '\n');
    f->at_line_start = true;
    mount_comments(f, prev_end, close_tok, last_line, /*inline_only=*/false);
    f->indent--;
    if (f->at_line_start) sb_indent(&f->sb, f->indent);
    f->at_line_start = false;
}

/* ---- 括号渲染与优先级 ---- */

/* 运算符优先级表（数值 = 绑定力，越大越紧）。赋值/三元最低（0）。 */
typedef struct { const char *op; int lp; int rp; } op_entry_t;

/* 按 strslice（长度感知）查表；token 文本是源码切片，非 NUL 结尾。 */
static bool op_binding_len(const char *ptr, size_t len, int *lp, int *rp) {
    static const op_entry_t ops[] = {
        { "as", 21, 22 }, { "is", 21, 22 }, { "extends", 11, 12 },
        { "||", 1, 2 }, { "&&", 3, 4 }, { "|", 5, 6 }, { "^", 7, 8 },
        { "&", 9, 10 }, { "==", 11, 12 }, { "!=", 11, 12 },
        { "<", 13, 14 }, { ">", 13, 14 }, { "<=", 13, 14 }, { ">=", 13, 14 },
        { "<<", 15, 16 }, { ">>", 15, 16 },
        { "+", 17, 18 }, { "-", 17, 18 },
        { "*", 19, 20 }, { "/", 19, 20 }, { "%", 19, 20 },
        { NULL, 0, 0 },
    };
    for (size_t i = 0; ops[i].op; i++) {
        size_t n = strlen(ops[i].op);
        if (len == n && memcmp(ptr, ops[i].op, n) == 0) {
            *lp = ops[i].lp;
            *rp = ops[i].rp;
            return true;
        }
    }
    return false;
}

/* 表达式节点自身的左/右结合绑定力（用于决定是否加括号）。
 * 赋值（= / += / ...）与三元：最低（0），右结合。 */
static void node_prec(ast_node_t *n, int *lp, int *rp, bool *right_assoc) {
    *right_assoc = false;
    switch (n->kind) {
        case AST_ASSIGN: {
            *lp = 0;
            *rp = 0;
            *right_assoc = true;   /* a = b = c：右结合 */
            break;
        }
        case AST_TERNARY: {
            *lp = 0;
            *rp = 0;
            *right_assoc = true;   /* a ? b : c ? d : e */
            break;
        }
        case AST_BINARY: {
            strslice_t op = token_strslice(((ast_binary_t *)n)->op);
            /* op 是 token pool 引用（源码切片，非 NUL 结尾），按长度比较。
               未登记的运算符（表缺项）必须回落到原子优先级——否则 lp/rp
               保持未初始化，need_paren 会读出垃圾值，导致括号加得随机
               （表现为同一文件两遍格式化结果不同 = 非幂等，且属 UB）。 */
            if (!op_binding_len(op.ptr, op.len, lp, rp)) {
                *lp = 26;
                *rp = 26;
            }
            break;
        }
        case AST_UNARY: {
            *lp = 23;
            *rp = 23;
            break;
        }
        default:
            *lp = 26;   /* 原子/后缀：最高 */
            *rp = 26;
            break;
    }
}

/* 是否需要括号包裹 child（parent 的某侧子节点）。
 *   child_prec < bound → 加括号（优先级不足）。
 *   等优先级：同 op 左结合（child 在 lhs）不括号；右结合（child 在 rhs
 *   或 child 是右结合运算符）时，若 op 相同则不括号（幂等），否则括号。
 *   特殊：child 是三元时，作为父级 rhs 必须括号（歧义保护）。
 */
static bool need_paren(ast_node_t *child, ast_node_t *parent, bool child_on_rhs,
                       int bound) {
    if (!child) return false;
    int lp, rp;
    bool ra;
    node_prec(child, &lp, &rp, &ra);

    if (child->kind == AST_TERNARY) {
        /* 三元嵌套在三元/赋值/二元 rhs 上：必须括号（a ? b : c ? d : e
           例外见下——右结合同层不括号） */
        if (parent->kind == AST_TERNARY && child_on_rhs) {
            /* a ? b : (c ? d : e) 解析为 a ? b : (c?d:e)？否——
               parser 右结合：else = c ? d : e 天然嵌套，不括号 */
            return false;
        }
        return true;
    }
    if (child->kind == AST_ASSIGN) {
        /* 赋值嵌套：a = b = c 合法（右结合，无括号）；作为 rhs 且同 op 不括号 */
        if (child_on_rhs && parent->kind == AST_ASSIGN) return false;
        return true;
    }
    if (lp < bound) return true;   /* 优先级不足 → 必须加括号 */

    if (lp == bound && child_on_rhs && parent->kind == AST_BINARY) {
        /* 等优先级：右结合链（a - (b - c) 中 child=rhs 且同 op）需括号，
           除非 child 也是同 op 且右结合性允许。C 语义：左结合运算符
           a - (b - c) 必须括号；a + (b + c) 语义等价但保留括号更清晰。
           注意 token 文本是源码切片（非 NUL 结尾），不能 strcmp。 */
        const token_t *p_tok = ((ast_binary_t *)parent)->op;
        const token_t *c_tok = ((ast_binary_t *)child)->op;
        size_t plen = 0, clen = 0;
        const char *pt = token_get_text(p_tok, &plen);
        const char *ct = token_get_text(c_tok, &clen);
        if (plen == clen && plen > 0 && memcmp(pt, ct, plen) == 0) return false;
        return true;
    }
    return false;
}

/* 渲染表达式并视需要加括号。 */
static void render_expr_paren(fmt_t *f, ast_node_t *child, ast_node_t *parent,
                              bool child_on_rhs, int bound) {
    if (need_paren(child, parent, child_on_rhs, bound)) {
        sb_ch(&f->sb, '(');
        render_expr(f, child);
        sb_ch(&f->sb, ')');
    } else {
        render_expr(f, child);
    }
}

/* ---- 叶子渲染 ---- */

static void render_ident(fmt_t *f, ast_node_t *n) {
    strslice_t s = ((ast_ident_t *)n)->name;
    sb_put(&f->sb, s.ptr, s.len);
}

static void render_int_lit(fmt_t *f, ast_node_t *n) {
    ast_int_lit_t *lit = (ast_int_lit_t *)n;
    size_t tlen = 0;
    const char *text = token_get_text(
        (const token_t *)vec_get(f->tokens, n->tok_begin), &tlen);
    if (text) sb_put(&f->sb, text, tlen);   /* 原样保留进制/前缀 */
    if (lit->type.ptr) sb_put(&f->sb, lit->type.ptr, lit->type.len);  /* 类型后缀 */
}

/* ---- 节点渲染（表达式） ---- */

static void render_expr(fmt_t *f, ast_node_t *node) {
    if (!node) return;
    switch (node->kind) {
        case AST_IDENT: {
            render_ident(f, node);
            break;
        }
        case AST_INT_LIT: {
            render_int_lit(f, node);
            break;
        }
        case AST_FLOAT_LIT: {
            size_t tlen = 0;
            const char *text = token_get_text(
                (const token_t *)vec_get(f->tokens, node->tok_begin), &tlen);
            if (text) sb_put(&f->sb, text, tlen);
            ast_float_lit_t *fl = (ast_float_lit_t *)node;
            if (fl->type.ptr) sb_put(&f->sb, fl->type.ptr, fl->type.len);
            break;
        }
        case AST_BOOL_LIT: {
            sb_str(&f->sb, ((ast_bool_lit_t *)node)->value ? "true" : "false");
            break;
        }
        case AST_STRING_LIT: {
            /* 输出源码原文（含引号与转义） */
            size_t tlen = 0;
            const char *text = token_get_text(
                (const token_t *)vec_get(f->tokens, node->tok_begin), &tlen);
            if (text) sb_put(&f->sb, text, tlen);
            break;
        }
        case AST_CHAR_LIT: {
            size_t tlen = 0;
            const char *text = token_get_text(
                (const token_t *)vec_get(f->tokens, node->tok_begin), &tlen);
            if (text) sb_put(&f->sb, text, tlen);
            break;
        }
        case AST_UNDEF: {
            sb_str(&f->sb, "undefined");
            break;
        }
        case AST_NIL: {
            sb_str(&f->sb, "nil");
            break;
        }
        case AST_SCOPE_REF: {
            ast_scope_ref_t *er = (ast_scope_ref_t *)node;
            render_expr(f, er->type_expr);
            sb_str(&f->sb, "::");
            sb_put(&f->sb, er->variant.ptr, er->variant.len);
            break;
        }
        case AST_ARRAY: {
            ast_array_t *a = (ast_array_t *)node;
            sb_ch(&f->sb, '[');
            render_expr(f, a->length);
            sb_ch(&f->sb, ']');
            render_expr(f, a->base_type);
            break;
        }
        case AST_TUPLE: {
            ast_tuple_t *t = (ast_tuple_t *)node;
            sb_ch(&f->sb, '<');
            render_comma_list(f, t->elem_types);
            sb_ch(&f->sb, '>');
            break;
        }
        case AST_CONST: {
            sb_str(&f->sb, "const ");
            render_expr(f, ((ast_const_t *)node)->sub);
            break;
        }
        case AST_VOLATILE: {
            sb_str(&f->sb, "volatile ");
            render_expr(f, ((ast_volatile_t *)node)->sub);
            break;
        }
        case AST_OPTION: {
            sb_ch(&f->sb, '?');
            render_expr(f, ((ast_option_t *)node)->sub);
            break;
        }
        case AST_SIZEOF: {
            sb_str(&f->sb, "sizeof(");
            render_expr(f, ((ast_sizeof_t *)node)->operand);
            sb_ch(&f->sb, ')');
            break;
        }
        case AST_ALIGNOF: {
            sb_str(&f->sb, "alignof(");
            render_expr(f, ((ast_alignof_t *)node)->operand);
            sb_ch(&f->sb, ')');
            break;
        }
        case AST_TYPEOF: {
            sb_str(&f->sb, "typeof(");
            render_expr(f, ((ast_typeof_t *)node)->operand);
            sb_ch(&f->sb, ')');
            break;
        }
        case AST_FUNC_TYPE: {
            ast_func_type_t *ft = (ast_func_type_t *)node;
            sb_str(&f->sb, "func");
            sb_ch(&f->sb, '(');
            render_comma_list(f, ft->params);
            sb_ch(&f->sb, ')');
            if (ft->return_type) {
                sb_str(&f->sb, " -> ");
                render_expr(f, ft->return_type);
            }
            break;
        }
        case AST_UNARY: {
            ast_unary_t *u = (ast_unary_t *)node;
            size_t olen = 0;
            const char *op = token_get_text(u->op, &olen);
            sb_put(&f->sb, op, olen);
            /* 一元前缀紧贴操作数；操作数优先级低于 23 时加括号（- (a + b)） */
            render_expr_paren(f, u->operand, node, false, 23);
            break;
        }
        case AST_BINARY: {
            ast_binary_t *b = (ast_binary_t *)node;
            int lp, rp;
            size_t olen = 0;
            const char *op = token_get_text(b->op, &olen);
            op_binding_len(op, olen, &lp, &rp);
            render_expr_paren(f, b->lhs, node, false, lp);
            sb_ch(&f->sb, ' ');
            sb_put(&f->sb, op, olen);
            sb_ch(&f->sb, ' ');
            render_expr_paren(f, b->rhs, node, true, rp);
            break;
        }
        case AST_ASSIGN: {
            ast_assign_t *a = (ast_assign_t *)node;
            size_t olen = 0;
            const char *op = token_get_text(a->op, &olen);
            render_expr_paren(f, a->target, node, false, 0);
            sb_ch(&f->sb, ' ');
            sb_put(&f->sb, op, olen);
            sb_ch(&f->sb, ' ');
            render_expr_paren(f, a->value, node, true, 0);
            break;
        }
        case AST_TERNARY: {
            ast_ternary_t *t = (ast_ternary_t *)node;
            render_expr(f, t->cond);
            sb_str(&f->sb, " ? ");
            render_expr_paren(f, t->then_branch, node, false, 0);
            sb_str(&f->sb, " : ");
            render_expr_paren(f, t->else_branch, node, true, 0);
            break;
        }
        case AST_CALL: {
            ast_call_t *c = (ast_call_t *)node;
            render_expr(f, c->callee);
            sb_ch(&f->sb, '(');
            render_comma_list(f, c->args);
            sb_ch(&f->sb, ')');
            break;
        }
        case AST_MEMBER: {
            ast_member_t *m = (ast_member_t *)node;
            render_expr(f, m->object);
            sb_ch(&f->sb, '.');
            sb_put(&f->sb, m->field.ptr, m->field.len);
            break;
        }
        case AST_INDEX: {
            ast_index_t *ix = (ast_index_t *)node;
            render_expr(f, ix->object);
            sb_ch(&f->sb, '[');
            render_comma_list(f, ix->indices);
            sb_ch(&f->sb, ']');
            break;
        }
        case AST_UNWRAP: {
            /* 后缀解包 `a.!`（assert）。**点必须由 fmt 补上**：parser 按
               "`.` 分支 advance 后看后随单字符 symbol"识别，`u->op` 只是那
               个单字符 token（`!`），直接 sb_put 会输出 `a!` → 重新解析报
               `expected ';'`（格式化把源码改坏）。操作数是后缀族，绑定力最高
               （26），优先级更低者保括号，与 `.*` / `.&` 同款。 */
            ast_unwrap_t *u = (ast_unwrap_t *)node;
            render_expr_paren(f, u->operand, node, false, 26);
            size_t olen = 0;
            const char *op = token_get_text(u->op, &olen);
            sb_ch(&f->sb, '.');
            sb_put(&f->sb, op, olen);
            break;
        }
        /* M3 后缀族：x.& 取地址 / r.* 解引用。三者（与成员/下标/解包）都是
           **后缀**，绑定力最高（26），因此操作数里优先级更低者必须保住括号：
           `(a as ref *i32).*` 若渲染成 `a as ref *i32.*` 会被重解析成
           `a as ref *(i32.*)`，语义改变（此前这两个 kind 无渲染分支，会被
           静默丢弃 —— `clux format` 直接吃掉表达式，属数据损坏）。 */
        case AST_ADDR: {
            ast_addr_t *ad = (ast_addr_t *)node;
            render_expr_paren(f, ad->operand, node, false, 26);
            sb_str(&f->sb, ".&");
            break;
        }
        case AST_DEREF: {
            ast_deref_t *dr = (ast_deref_t *)node;
            render_expr_paren(f, dr->operand, node, false, 26);
            sb_str(&f->sb, ".*");
            break;
        }
        case AST_MOVE: {
            /* move(x) / clone(x)：op token 决定是哪一个 */
            ast_move_t *mv = (ast_move_t *)node;
            size_t olen = 0;
            const char *op = token_get_text(mv->op, &olen);
            sb_put(&f->sb, op, olen);
            sb_ch(&f->sb, '(');
            render_expr(f, mv->operand);
            sb_ch(&f->sb, ')');
            break;
        }
        case AST_CONSTRUCT: {
            ast_construct_t *c = (ast_construct_t *)node;
            sb_ch(&f->sb, '.');
            if (c->type) render_expr(f, c->type);
            /* 定位 '{' / '}' token 下标做注释挂载边界 */
            uint32_t open_tok = find_punct_token(f, node->tok_begin,
                                                 node->tok_end, "{");
            uint32_t close_tok = find_punct_token(f, open_tok + 1,
                                                  node->tok_end, "}");
            sb_ch(&f->sb, '{');
            render_construct_fields(f, c->fields, open_tok, close_tok);
            sb_ch(&f->sb, '}');
            break;
        }
        case AST_CONSTRUCT_FIELD: {
            ast_construct_field_t *cf = (ast_construct_field_t *)node;
            sb_ch(&f->sb, '.');
            sb_put(&f->sb, cf->name.ptr, cf->name.len);
            sb_str(&f->sb, " = ");
            render_expr(f, cf->value);
            break;
        }
        case AST_FILL: {
            ast_fill_t *fl = (ast_fill_t *)node;
            sb_ch(&f->sb, '<');
            render_expr(f, fl->value);
            sb_str(&f->sb, ", ");
            render_expr(f, fl->count);
            sb_ch(&f->sb, '>');
            break;
        }
        case AST_SPREAD: {
            /* 实参展开 `...expr`（漏渲染会导致格式化把展开悄悄改成单实参） */
            ast_spread_t *sp = (ast_spread_t *)node;
            sb_str(&f->sb, "...");
            render_expr(f, sp->operand);
            break;
        }
        case AST_FUNC_DEF: {
            /* 函数字面量（表达式位置） */
            render_func_literal(f, node);
            break;
        }
        case AST_ERROR: {
            /* 错误恢复：不渲染 */
            break;
        }
        case AST_SLICE_TYPE: {
            ast_slice_type_t *st = (ast_slice_type_t *)node;
            if (st->ownership) {
                size_t olen = 0;
                const char *ot = token_get_text(st->ownership, &olen);
                sb_put(&f->sb, ot, olen);
                sb_ch(&f->sb, ' ');
            }
            sb_str(&f->sb, "[]");
            render_expr(f, st->elem_type);
            break;
        }
        case AST_SLICE: {
            ast_slice_t *sl = (ast_slice_t *)node;
            render_expr(f, sl->object);
            sb_ch(&f->sb, '[');
            if (sl->low) render_expr(f, sl->low);
            sb_ch(&f->sb, ':');
            if (sl->high) render_expr(f, sl->high);
            sb_ch(&f->sb, ']');
            break;
        }
        case AST_MAKE: {
            ast_make_t *m = (ast_make_t *)node;
            sb_str(&f->sb, "make(");
            render_expr(f, m->elem_type);
            sb_str(&f->sb, ", ");
            render_expr(f, m->length);
            for (ast_node_t *it = m->inits; it; it = it->next) {
                sb_str(&f->sb, ", ");
                render_expr(f, it);
            }
            sb_ch(&f->sb, ')');
            break;
        }
        /* M3 类型/分配族：own|ref|fatal *T、new T{...}、'<a,b> T。
           同样此前**无渲染分支 → 静默丢弃**（`var p: ref *i32 = ...` 的类型
           会被整个吃掉）。 */
        case AST_PTR: {
            ast_ptr_t *pt = (ast_ptr_t *)node;
            if (pt->ownership) {
                size_t olen = 0;
                const char *ot = token_get_text(pt->ownership, &olen);
                sb_put(&f->sb, ot, olen);
                sb_ch(&f->sb, ' ');
            }
            sb_ch(&f->sb, '*');
            render_expr(f, pt->base_type);
            break;
        }
        case AST_OWNED_TYPE: {
            /* fatal T（复合类型的将亡值变体）：修饰词 + 空格 + 基础类型 */
            ast_owned_type_t *ot = (ast_owned_type_t *)node;
            if (ot->ownership) {
                size_t olen = 0;
                const char *otext = token_get_text(ot->ownership, &olen);
                sb_put(&f->sb, otext, olen);
                sb_ch(&f->sb, ' ');
            }
            render_expr(f, ot->base_type);
            break;
        }
        case AST_NEW: {
            /* new T{...}：type 为 NULL 即匿名构造（new { .x = 1 }） */
            ast_new_t *nw = (ast_new_t *)node;
            sb_str(&f->sb, "new ");
            if (nw->type) render_expr(f, nw->type);
            uint32_t open_tok = find_punct_token(f, node->tok_begin,
                                                 node->tok_end, "{");
            uint32_t close_tok = find_punct_token(f, open_tok + 1,
                                                  node->tok_end, "}");
            sb_ch(&f->sb, '{');
            render_construct_fields(f, nw->fields, open_tok, close_tok);
            sb_ch(&f->sb, '}');
            break;
        }
        case AST_SCOPE_ANNOT: {
            /* '<a,b> T（'*' 记 global，仅首元素合法） */
            ast_scope_annot_t *sa = (ast_scope_annot_t *)node;
            sb_str(&f->sb, "'<");
            for (size_t i = 0; i < sa->count; i++) {
                if (i > 0) sb_ch(&f->sb, ',');
                sb_put(&f->sb, sa->names[i].ptr, sa->names[i].len);
            }
            sb_str(&f->sb, "> ");
            render_expr(f, sa->sub);
            break;
        }
        default: {
            break;
        }
    }
}

/* ---- 语句渲染 ---- */

static void render_stmt(fmt_t *f, ast_node_t *node) {
    if (!node) return;
    /* 装饰器前缀 `[[<expr>]]`（只出现在 var / func 声明上）：必须原样渲染，
       否则格式化会把"装饰后的声明"悄悄变回普通声明（语义丢失）。 */
    if (node->kind == AST_FUNC_DEF || node->kind == AST_VAR_DEF) {
        ast_node_t *decs = (node->kind == AST_FUNC_DEF)
                               ? ((ast_func_def_t *)node)->decorators
                               : ((ast_var_def_t *)node)->decorators;
        for (ast_node_t *d = decs; d; d = d->next) {
            sb_str(&f->sb, "[[");
            render_expr(f, d);
            sb_str(&f->sb, "]] ");
        }
    }
    switch (node->kind) {
        case AST_VAR_DEF: {
            ast_var_def_t *v = (ast_var_def_t *)node;
            /* M6 FFI：`extern var <库>::<名>: T;` —— 必须原样渲染（含 `extern var`
               与 `lib::` 前缀、无初始化器）。漏掉这一步的话 format 会把它改写成
               普通 `var name: T = ...`（丢库名 + 丢 `extern`），与 import/reexport/
               extern cfunc 漏分支同类的**静默数据损坏**。 */
            if (v->is_extern_var) {
                sb_str(&f->sb, "extern var ");
                if (v->lib_name.len > 0) {
                    sb_put(&f->sb, v->lib_name.ptr, v->lib_name.len);
                    sb_str(&f->sb, "::");
                }
                sb_put(&f->sb, v->name.ptr, v->name.len);
                if (v->type_expr) {
                    sb_str(&f->sb, ": ");
                    render_expr(f, v->type_expr);
                }
                sb_ch(&f->sb, ';');
                break;
            }
            if (v->is_comptime) sb_str(&f->sb, "comptime ");
            sb_str(&f->sb, "var ");
            sb_put(&f->sb, v->name.ptr, v->name.len);
            if (v->type_expr) {
                sb_str(&f->sb, ": ");
                render_expr(f, v->type_expr);
            }
            sb_str(&f->sb, " = ");
            render_expr(f, v->init);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_TYPE_DEF: {
            ast_type_def_t *t = (ast_type_def_t *)node;
            sb_str(&f->sb, "type ");
            sb_put(&f->sb, t->name.ptr, t->name.len);
            sb_str(&f->sb, " = ");
            render_expr(f, t->expr);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_ENUM_DEF: {
            ast_enum_def_t *e = (ast_enum_def_t *)node;
            sb_str(&f->sb, "enum ");
            sb_put(&f->sb, e->name.ptr, e->name.len);
            sb_str(&f->sb, ": ");
            render_expr(f, e->underlying_type);
            sb_str(&f->sb, " {");
            bool first = true;
            for (ast_node_t *v = e->variants; v; v = v->next) {
                if (!first) sb_str(&f->sb, ", ");
                ast_enum_variant_t *ev = (ast_enum_variant_t *)v;
                sb_put(&f->sb, ev->name.ptr, ev->name.len);
                sb_str(&f->sb, " = ");
                render_expr(f, ev->value);
                first = false;
            }
            sb_str(&f->sb, "}");
            break;
        }
        case AST_STRUCT_DEF: {
            /* struct 体多行展开：每个字段/嵌入/成员独占一行，4 空格缩进。
             * 字段/嵌入后补 ';'，func 成员自带函数体块。注释按源码位置挂载
             *（项间独立行注释 + 项后行内注释），空行保留——与 render_block
             * 同款规则。空 struct 紧凑 `{}`。 */
            ast_struct_def_t *s = (ast_struct_def_t *)node;
            sb_str(&f->sb, "struct ");
            sb_put(&f->sb, s->name.ptr, s->name.len);
            sb_str(&f->sb, " {");

            bool any = s->fields || s->embeds || s->members;
            if (!any) {
                sb_str(&f->sb, "}");
                f->at_line_start = false;
                break;
            }

            /* 字段 + 嵌入按**合并序列**（嵌入的 field_index 决定插入位次）。
             * members 追加到末尾。统一收集到 all_items 数组，一次遍历多行渲染。 */
            size_t nfs = 0;
            for (ast_node_t *fn = s->fields; fn; fn = fn->next) nfs++;

            ast_node_t *all_items[512];
            size_t nall = 0;
            size_t cap = sizeof(all_items) / sizeof(all_items[0]);
            for (size_t fi = 0; fi <= nfs && nall < cap; fi++) {
                for (ast_node_t *e = s->embeds; e && nall < cap; e = e->next) {
                    ast_struct_embed_t *emb = (ast_struct_embed_t *)e;
                    if (emb->field_index != fi) continue;
                    all_items[nall++] = e;
                }
                if (fi < nfs) {
                    ast_node_t *fn = s->fields;
                    for (size_t skip = 0; skip < fi && fn; skip++)
                        fn = fn->next;
                    if (fn && nall < cap) all_items[nall++] = fn;
                }
            }
            for (ast_node_t *m = s->members; m && nall < cap; m = m->next)
                all_items[nall++] = m;

            f->indent++;
            uint32_t prev_end = node->tok_begin + 1;  /* '{' 之后 */
            size_t last_line =
                tok_line((const token_t *)vec_get(f->tokens, node->tok_begin));

            for (size_t k = 0; k < nall; k++) {
                ast_node_t *it = all_items[k];
                if (!it) continue;

                /* 项前注释（独立行） */
                if (!f->at_line_start) sb_ch(&f->sb, '\n');
                f->at_line_start = true;
                last_line = mount_comments(f, prev_end, it->tok_begin, last_line,
                                           /*inline_only=*/false);
                /* 空行保留 */
                size_t cur_line = tok_line(
                    (const token_t *)vec_get(f->tokens, it->tok_begin));
                if (cur_line >= last_line + 2) {
                    sb_ch(&f->sb, '\n');
                    f->at_line_start = true;
                }
                if (f->at_line_start) sb_indent(&f->sb, f->indent);
                f->at_line_start = false;

                /* 渲染项本身 */
                if (it->kind == AST_STRUCT_FIELD) {
                    ast_struct_field_t *sf = (ast_struct_field_t *)it;
                    sb_put(&f->sb, sf->name.ptr, sf->name.len);
                    sb_str(&f->sb, ": ");
                    render_expr(f, sf->type);
                    sb_ch(&f->sb, ';');
                } else if (it->kind == AST_STRUCT_EMBED) {
                    ast_struct_embed_t *emb = (ast_struct_embed_t *)it;
                    sb_str(&f->sb, "...");
                    sb_put(&f->sb, emb->name.ptr, emb->name.len);
                    sb_ch(&f->sb, ';');
                } else {
                    /* var / func 成员：自带收尾（var ';'；func 函数体块） */
                    render_node(f, it);
                }
                prev_end = it->tok_end;

                /* 项后行内注释 */
                uint32_t inline_end = (k + 1 < nall)
                    ? all_items[k + 1]->tok_begin
                    : node->tok_end;
                size_t it_end_line =
                    tok_line((const token_t *)vec_get(f->tokens, prev_end - 1));
                last_line = mount_comments(f, prev_end, inline_end, it_end_line,
                                           /*inline_only=*/true);
                last_line = it_end_line;
            }

            /* 块尾注释 + '}' */
            if (!f->at_line_start) sb_ch(&f->sb, '\n');
            f->at_line_start = true;
            mount_comments(f, prev_end, node->tok_end, last_line,
                           /*inline_only=*/false);
            f->indent--;
            if (f->at_line_start) sb_indent(&f->sb, f->indent);
            f->at_line_start = false;
            sb_ch(&f->sb, '}');
            f->at_line_start = false;
            break;
        }
        case AST_CUNION_DEF: {
            /* C 语义 union：与 struct 同款平铺渲染（member 名: 类型; ...） */
            ast_cunion_def_t *c = (ast_cunion_def_t *)node;
            sb_str(&f->sb, "cunion ");
            sb_put(&f->sb, c->name.ptr, c->name.len);
            sb_str(&f->sb, " {");
            if (!c->fields) {
                sb_str(&f->sb, "}");
            } else {
                sb_ch(&f->sb, ' ');
                render_semi_list(f, c->fields);
                sb_str(&f->sb, " }");
            }
            break;
        }
        case AST_UNION_DEF: {
            /* tag union：与 struct/cunion 同款平铺渲染（member 名: 类型; ...）。
               漏掉本分支会让 union 声明被渲染成空 + 一个 ';'（数据损坏）。 */
            ast_union_def_t *u = (ast_union_def_t *)node;
            sb_str(&f->sb, "union ");
            sb_put(&f->sb, u->name.ptr, u->name.len);
            sb_str(&f->sb, " {");
            if (!u->fields) {
                sb_str(&f->sb, "}");
            } else {
                sb_ch(&f->sb, ' ');
                render_semi_list(f, u->fields);
                sb_str(&f->sb, " }");
            }
            break;
        }
        case AST_EXTERN_LIB: {
            /* extern lib <alias> from "<name>";——AST 中 name 已去引号，渲染补回。
               漏掉本分支会让声明被渲染成空（数据损坏），与 AST_IMPORT 同理。
               name 是**通用库名**（如 "c"），不是文件名；显式路径也原样保留。 */
            ast_extern_lib_t *el = (ast_extern_lib_t *)node;
            sb_str(&f->sb, "extern lib ");
            sb_put(&f->sb, el->alias.ptr, el->alias.len);
            sb_str(&f->sb, " from \"");
            sb_put(&f->sb, el->name.ptr, el->name.len);
            sb_str(&f->sb, "\"");
            /* 条件后缀必须原样渲染：漏掉它 = 格式化把"仅在 Windows 生效"的
               声明变成"永远生效"（整条声明被静默改语义，比丢声明更隐蔽）。 */
            if (el->cond.len > 0) {
                sb_str(&f->sb, " assert(");
                sb_put(&f->sb, el->cond.ptr, el->cond.len);
                sb_str(&f->sb, ")");
            }
            sb_str(&f->sb, ";");
            break;
        }        case AST_IMPORT: {
            /* import <alias> from "<path>";——AST 中的 path 已去掉引号
               （parse_import 不做转义展开），故渲染时补回引号。
               漏掉本分支会让 import 语句被渲染成空 + 一个 ';'（数据损坏）。 */
            ast_import_t *im = (ast_import_t *)node;
            sb_str(&f->sb, "import ");
            sb_put(&f->sb, im->alias.ptr, im->alias.len);
            sb_str(&f->sb, " from \"");
            sb_put(&f->sb, im->path.ptr, im->path.len);
            sb_str(&f->sb, "\";");
            break;
        }
        case AST_REEXPORT: {
            /* export * from "<path>";——代理导出（AST 中 path 已去引号，
               渲染时补回；与 AST_IMPORT 同款：漏分支会静默删除该声明）。 */
            ast_reexport_t *re = (ast_reexport_t *)node;
            sb_str(&f->sb, "export * from \"");
            sb_put(&f->sb, re->path.ptr, re->path.len);
            sb_str(&f->sb, "\";");
            break;
        }
        case AST_STRUCT_FIELD: {
            ast_struct_field_t *sf = (ast_struct_field_t *)node;
            sb_put(&f->sb, sf->name.ptr, sf->name.len);
            sb_str(&f->sb, ": ");
            render_expr(f, sf->type);
            break;
        }
        case AST_STRUCT_EMBED: {
            /* 结构体嵌入 `...Name;`（结构体组合的语法面）。位置由
               AST_STRUCT_DEF 分支按 field_index 恢复，这里只渲染自身。 */
            ast_struct_embed_t *emb = (ast_struct_embed_t *)node;
            sb_str(&f->sb, "...");
            sb_put(&f->sb, emb->name.ptr, emb->name.len);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_COMPTIME_IF: {
            ast_comptime_if_t *ci = (ast_comptime_if_t *)node;
            sb_str(&f->sb, "comptime if (");
            render_expr(f, ci->cond);
            sb_ch(&f->sb, ')');
            render_block(f, ci->then_body);
            if (ci->else_body) {
                if (ci->else_body->kind == AST_COMPTIME_IF) {
                    sb_str(&f->sb, " else ");
                    render_stmt(f, ci->else_body);
                } else {
                    sb_str(&f->sb, " else");
                    render_block(f, ci->else_body);
                }
            }
            break;
        }
        case AST_IF: {
            ast_if_t *i = (ast_if_t *)node;
            sb_str(&f->sb, "if (");
            render_expr(f, i->cond);
            sb_ch(&f->sb, ')');
            render_block(f, i->then_body);
            if (i->else_body) {
                if (i->else_body->kind == AST_IF) {
                    sb_str(&f->sb, " else ");
                    render_stmt(f, i->else_body);
                } else {
                    /* else 后接块：空格由 render_block 的 " {" 提供 */
                    sb_str(&f->sb, " else");
                    render_block(f, i->else_body);
                }
            }
            break;
        }
        case AST_SWITCH: {
            ast_switch_t *sw = (ast_switch_t *)node;
            sb_str(&f->sb, "switch (");
            render_expr(f, sw->cond);
            sb_ch(&f->sb, ')');
            sb_str(&f->sb, " {");
            bool any = false;
            for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
                if (any) sb_ch(&f->sb, ' ');
                ast_switch_case_t *sc = (ast_switch_case_t *)cs;
                sb_ch(&f->sb, '(');
                render_comma_list(f, sc->patterns);
                sb_ch(&f->sb, ')');
                sb_str(&f->sb, " -> ");
                render_block(f, sc->body);
                any = true;
            }
            if (sw->default_body) {
                if (any) sb_ch(&f->sb, ' ');
                sb_str(&f->sb, "default -> ");
                render_block(f, sw->default_body);
            }
            sb_str(&f->sb, "}");
            break;
        }
        case AST_WHILE: {
            ast_while_t *w = (ast_while_t *)node;
            sb_str(&f->sb, "while (");
            render_expr(f, w->cond);
            sb_ch(&f->sb, ')');
            render_block(f, w->body);
            break;
        }
        case AST_DOWHILE: {
            ast_dowhile_t *d = (ast_dowhile_t *)node;
            sb_str(&f->sb, "do");
            render_block(f, d->body);
            sb_str(&f->sb, " while (");
            render_expr(f, d->cond);
            sb_ch(&f->sb, ')');
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_FOR: {
            ast_for_t *fo = (ast_for_t *)node;
            sb_str(&f->sb, "for (");
            if (fo->init) render_stmt(f, fo->init);
            sb_ch(&f->sb, ' ');
            if (fo->cond) render_expr(f, fo->cond);
            sb_str(&f->sb, "; ");
            if (fo->update) render_expr(f, fo->update);
            sb_ch(&f->sb, ')');
            render_block(f, fo->body);
            break;
        }
        case AST_FOREACH: {
            /* foreach ([var] name [: T] of iterable) { … }——语法糖的**源码形态**
               必须完整回显（fmt 不走脱糖，见 docs/architecture-m1.md §1.x；
               漏渲染会让格式化产物丢掉整个循环体 = 静默数据损坏）。 */
            ast_foreach_t *fe = (ast_foreach_t *)node;
            sb_str(&f->sb, "foreach (");
            if (fe->is_decl) sb_str(&f->sb, "var ");
            sb_put(&f->sb, fe->name.ptr, fe->name.len);
            if (fe->type_expr) {
                sb_str(&f->sb, ": ");
                render_expr(f, fe->type_expr);
            }
            sb_str(&f->sb, " of ");
            render_expr(f, fe->iterable);
            sb_ch(&f->sb, ')');
            render_block(f, fe->body);
            break;
        }
        case AST_RETURN: {
            ast_return_t *r = (ast_return_t *)node;
            sb_str(&f->sb, "return");
            if (r->value) {
                sb_ch(&f->sb, ' ');
                render_expr(f, r->value);
            }
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_BREAK: {
            sb_str(&f->sb, "break;");
            break;
        }
        case AST_CONTINUE: {
            sb_str(&f->sb, "continue;");
            break;
        }
        case AST_EMPTY_STMT: {
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_BLOCK: {
            render_block(f, node);
            break;
        }
        case AST_DEFER: {
            /* defer [annot] [captures] { body }：
               渲染必须完整回显捕获列表与作用域标注——漏渲染会让格式化产物
               丢掉引用语义（捕获的变量在块体里变成未声明）或丢掉标注（ref
               捕获的强制标注消失 → 产物不再能编译），属静默数据损坏。
               空捕获列表（`||` / 省略）统一渲染为省略形态（两者 AST 相同）。 */
            ast_defer_t *d = (ast_defer_t *)node;
            sb_str(&f->sb, "defer");
            if (d->closure) {
                if (d->closure->scope_annot_count > 0) {
                    sb_ch(&f->sb, ' ');
                    render_scope_annot_names(f, d->closure->scope_annot_names,
                                             d->closure->scope_annot_count);
                }
                if (d->closure->captures) {
                    sb_ch(&f->sb, ' ');
                    render_captures(f, d->closure->captures);
                }
                render_block(f, (ast_node_t *)d->closure->body);
            }
            break;
        }
        case AST_EXPR_STMT: {
            render_expr(f, ((ast_expr_stmt_t *)node)->expr);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_ASSIGN: {
            render_expr(f, node);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_FUNC_DEF: {
            /* 语句级函数定义 */
            render_func_def(f, node);
            break;
        }
        default: {
            render_expr(f, node);
            sb_ch(&f->sb, ';');
            break;
        }
    }
}

/* ---- 块渲染 ---- */

/* 渲染块 `{ stmts }`：非空块展开多行 +1 缩进；空块紧凑 `{}`。
 * 语句间 gap 注释（[上一语句末, 当前语句末)）在语句前挂载。 */
static void render_block(fmt_t *f, ast_node_t *node) {
    ast_block_t *b = (ast_block_t *)node;
    sb_str(&f->sb, " {");
    uint32_t prev_end = b->base.tok_begin + 1;  /* `{` 之后 */
    size_t last_line =
        tok_line((const token_t *)vec_get(f->tokens, b->base.tok_begin));

    if (!b->stmts) {
        /* 空块：块内注释挂载（如 `{ // hi }`），无注释则紧凑 `{}` */
        mount_comments(f, prev_end, b->base.tok_end, last_line,
                       /*inline_only=*/false);
        sb_ch(&f->sb, '}');
        f->at_line_start = false;
        return;
    }

    f->indent++;
    /* 首语句前的注释（`{` 后、首语句前） */
    last_line = mount_comments(f, prev_end, b->stmts->tok_begin, last_line,
                               /*inline_only=*/false);
    for (ast_node_t *s = b->stmts; s; s = s->next) {
        /* 紧跟 `}` 后的空语句 `;`（如嵌套函数定义 `func ... { ... };` 末尾
           的分号）与 `}` 处于同一行时，合并输出 `};` 而非把 `;` 排到下一行。
           prev_end 指向前语句末，prev_end-1 即前语句最后一个 token（`}`）。 */
        if (s->kind == AST_EMPTY_STMT && prev_end > 0) {
            const token_t *prev_last = (const token_t *)vec_get(
                f->tokens, prev_end - 1);
            size_t prev_line = tok_line(prev_last);
            size_t semi_line = tok_line(
                (const token_t *)vec_get(f->tokens, s->tok_begin));
            if (token_is(prev_last, "}") && prev_line == semi_line) {
                sb_ch(&f->sb, ';');
                f->at_line_start = false;
                prev_end = s->tok_end;
                /* last_line 保持为 prev_line（`}` 的行号），供下一语句的
                   空行判断与注释挂载使用。 */
                continue;
            }
        }

        /* 语句分隔换行 */
        if (!f->at_line_start) sb_ch(&f->sb, '\n');
        f->at_line_start = true;
        /* 语句前注释（独立行注释在此挂载，返回最后注释行号） */
        last_line = mount_comments(f, prev_end, s->tok_begin, last_line,
                                   /*inline_only=*/false);
        /* 空行保留：本语句源码行号比上次输出的源码行号 ≥2 → 补空行 */
        size_t cur_line = tok_line(
            (const token_t *)vec_get(f->tokens, s->tok_begin));
        if (cur_line >= last_line + 2) {
            sb_ch(&f->sb, '\n');
            f->at_line_start = true;
        }
        if (f->at_line_start) sb_indent(&f->sb, f->indent);
        f->at_line_start = false;
        render_stmt(f, s);
        prev_end = s->tok_end;
        /* 语句后行内注释：`stmt; // x` 同行注释立即挂载；独立行注释留给
           下一语句前的挂载（保持空行判断准确性） */
        uint32_t inline_end = s->next ? s->next->tok_begin : b->base.tok_end;
        size_t stmt_end_line =
            tok_line((const token_t *)vec_get(f->tokens, prev_end - 1));
        last_line = mount_comments(f, prev_end, inline_end, stmt_end_line,
                                   /*inline_only=*/true);
        last_line = stmt_end_line;
    }
    /* 块尾注释：最后语句尾 → '}' 前（保持块内缩进，`}` 前独立行） */
    if (!f->at_line_start) sb_ch(&f->sb, '\n');
    f->at_line_start = true;
    mount_comments(f, prev_end, b->base.tok_end, last_line,
                   /*inline_only=*/false);
    f->indent--;
    if (f->at_line_start) sb_indent(&f->sb, f->indent);
    f->at_line_start = false;
    sb_ch(&f->sb, '}');
    f->at_line_start = false;
}

/* ---- 函数渲染 ---- */

static void render_param(fmt_t *f, ast_node_t *n) {
    ast_var_def_t *v = (ast_var_def_t *)n;
    sb_put(&f->sb, v->name.ptr, v->name.len);
    sb_str(&f->sb, ": ");
    render_expr(f, v->type_expr);
}

static void render_captures(fmt_t *f, ast_node_t *captures) {
    sb_ch(&f->sb, '|');
    bool first = true;
    for (ast_node_t *c = captures; c; c = c->next) {
        if (!first) sb_str(&f->sb, ", ");
        ast_var_def_t *v = (ast_var_def_t *)c;
        if (v->type_expr || v->init) {
            /* 括号 VALUE DECL：(name[:type] = init) */
            sb_ch(&f->sb, '(');
            sb_put(&f->sb, v->name.ptr, v->name.len);
            if (v->type_expr) {
                sb_str(&f->sb, ": ");
                render_expr(f, v->type_expr);
            }
            sb_str(&f->sb, " = ");
            render_expr(f, v->init);
            sb_ch(&f->sb, ')');
        } else {
            sb_put(&f->sb, v->name.ptr, v->name.len);
        }
        first = false;
    }
    sb_ch(&f->sb, '|');
}

/* 闭包 / defer 的作用域标注 `'<a,b>`（m3-design §9）：' 与 < 紧贴（lexer 只在
 * 相邻时才合成 "'<" 复合 token），名字间 ", "，无标注时不输出任何字符。
 * 此前 render_func_def / render_func_literal **完全没有**这个渲染分支——
 * `func '<x> |r| ...` 的标注会被静默吃掉，产物里捕获 ref 的强制标注消失
 * （格式化后再编译直接报错），与 import/reexport/unwrap 漏分支同类的静默
 * 数据损坏，这里一并补上。 */
static void render_scope_annot_names(fmt_t *f, const strslice_t *names,
                                     size_t count) {
    if (!names || count == 0) return;
    sb_str(&f->sb, "'<");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_str(&f->sb, ", ");
        sb_put(&f->sb, names[i].ptr, names[i].len);
    }
    sb_ch(&f->sb, '>');
}

static void render_func_def(fmt_t *f, ast_node_t *node) {
    ast_func_def_t *fn = (ast_func_def_t *)node;
    /* 装饰器前缀（顶层函数走这条渲染路径，不经过 render_stmt） */
    for (ast_node_t *d = fn->decorators; d; d = d->next) {
        sb_str(&f->sb, "[[");
        render_expr(f, d);
        sb_str(&f->sb, "]] ");
    }
    if (fn->is_comptime) sb_str(&f->sb, "comptime ");
    /* M6 FFI：cfunc 定义必须原样渲染为 `cfunc`——渲染成 `func` 会静默改变语义
       （C ABI 导出 vs 普通函数），与 import/reexport 漏分支同类问题。 */
    sb_str(&f->sb, fn->is_extern_cfunc ? "extern cfunc "
                            : (fn->is_cfunc ? "cfunc " : "func "));
    if (fn->is_extern_cfunc && fn->lib_name.len > 0) {
        sb_put(&f->sb, fn->lib_name.ptr, fn->lib_name.len);
        sb_str(&f->sb, "::");
    }
    if (fn->scope_annot_count > 0) {
        render_scope_annot_names(f, fn->scope_annot_names,
                                 fn->scope_annot_count);
        sb_ch(&f->sb, ' ');
    }
    if (fn->captures) {
        render_captures(f, fn->captures);
        sb_ch(&f->sb, ' ');
    }
    sb_put(&f->sb, fn->name.ptr, fn->name.len);
    sb_ch(&f->sb, '(');
    bool first = true;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        if (!first) sb_str(&f->sb, ", ");
        render_param(f, p);
        first = false;
    }
    if (fn->is_variadic) {
        if (!first) sb_str(&f->sb, ", ");
        sb_str(&f->sb, "...");
    }
    sb_ch(&f->sb, ')');
    sb_str(&f->sb, ": ");
    render_expr(f, fn->return_expr);
    if (fn->is_extern_cfunc) {
        sb_ch(&f->sb, ';');  /* extern cfunc 声明无体 */
    } else {
        render_block(f, fn->body);
    }
}

/* 函数字面量（表达式位置）：func ['<a>] [|caps|] [name](params): type { body }
 * name 可选（无 name = 匿名）；有 name 时仅作显示名（不绑定符号），
 * 格式化须保留以维持源码语义；作用域标注同样必须保留（见
 * render_scope_annot_names 的说明）。 */
static void render_func_literal(fmt_t *f, ast_node_t *node) {
    ast_func_def_t *fn = (ast_func_def_t *)node;
    sb_str(&f->sb, "func");
    if (fn->scope_annot_count > 0) {
        sb_ch(&f->sb, ' ');
        render_scope_annot_names(f, fn->scope_annot_names,
                                 fn->scope_annot_count);
    }
    if (fn->captures) {
        sb_ch(&f->sb, ' ');
        render_captures(f, fn->captures);
    }
    if (fn->name.ptr) {
        sb_ch(&f->sb, ' ');
        sb_put(&f->sb, fn->name.ptr, fn->name.len);
    }
    sb_ch(&f->sb, '(');
    bool first = true;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        if (!first) sb_str(&f->sb, ", ");
        render_param(f, p);
        first = false;
    }
    if (fn->is_variadic) {
        if (!first) sb_str(&f->sb, ", ");
        sb_str(&f->sb, "...");
    }
    sb_ch(&f->sb, ')');
    sb_str(&f->sb, ": ");
    render_expr(f, fn->return_expr);
    if (fn->is_extern_cfunc) {
        sb_ch(&f->sb, ';');  /* extern cfunc 声明无体 */
    } else {
        render_block(f, fn->body);
    }
}

/* ---- 顶层：PROGRAM ---- */

static void render_program(fmt_t *f, ast_node_t *node) {
    ast_program_t *prog = (ast_program_t *)node;
    uint32_t prev_end = 0;
    size_t last_line = 0;
    bool first = true;
    for (ast_node_t *fn = prog->funcs; fn; fn = fn->next) {
        /* 顶层项之间的分隔换行：仅当上一项没有停在行首时才补。
           上一项若以独立行注释结尾，其文本已自带换行（at_line_start=true），
           此处再补一个 '\n' 会凭空多出一个空行——那正是"单遍不幂等"的根因
           （第二遍的空白保留规则又会合法地补一个空行，于是收敛到两个空行）。 */
        if (!first && !f->at_line_start) sb_ch(&f->sb, '\n');
        f->at_line_start = true;
        /* 只挂载函数之前的注释（[prev_end, tok_begin)）；函数体内的
           注释由 render_block 挂载（保持缩进/行内位置）。 */
        size_t cmt_line = mount_comments(f, prev_end, fn->tok_begin, last_line,
                                         /*inline_only=*/false);
        /* 空行保留：函数/注释后与上一输出源码行号 ≥2 → 补空行。
           顶层不缩进（indent=0），与块内规则一致。 */
        size_t cur_line = tok_line(
            (const token_t *)vec_get(f->tokens, fn->tok_begin));
        if (cur_line >= cmt_line + 2) {
            sb_ch(&f->sb, '\n');
            f->at_line_start = true;
        }
        if (f->at_line_start) sb_indent(&f->sb, f->indent);
        f->at_line_start = false;
        /* export 前缀：parser 把 `export` 记为声明节点上的 is_exported 标志
           （不是独立节点），渲染时必须还原，否则模块导出表静默丢失。 */
        if (fn->is_exported) sb_str(&f->sb, "export ");
        render_node(f, fn);
        prev_end = fn->tok_end;
        last_line = tok_line((const token_t *)vec_get(f->tokens, prev_end - 1));
        /* 函数尾注释：本函数尾 → 下一函数前 */
        uint32_t end = fn->next ? fn->next->tok_begin : (uint32_t)vec_len(f->tokens);
        cmt_line = mount_comments(f, prev_end, end, last_line,
                                  /*inline_only=*/false);
        last_line = cmt_line;
        first = false;
    }
    /* 结尾补一个换行（空输入除外） */
    if (!first) sb_ch(&f->sb, '\n');
    f->at_line_start = true;
}

/* 通用节点入口（顶层 funcs / 兄弟链渲染用） */
static void render_node(fmt_t *f, ast_node_t *node) {
    if (!node) return;
    if (node->kind == AST_FUNC_DEF) {
        render_func_def(f, node);
        return;
    }
    render_stmt(f, node);
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
