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
#include "parser/ast_block.h"
#include "parser/ast_if.h"
#include "parser/ast_switch.h"
#include "parser/ast_while.h"
#include "parser/ast_for.h"
#include "parser/ast_return.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_unary.h"
#include "parser/ast_call.h"
#include "parser/ast_member.h"
#include "parser/ast_index.h"
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
#include "parser/ast_enum_ref.h"
#include "parser/ast_ternary.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_undef.h"
#include "parser/ast_nil.h"
#include "parser/ast_error.h"
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

/* ---- 括号渲染与优先级 ---- */

/* 运算符优先级表（数值 = 绑定力，越大越紧）。赋值/三元最低（0）。 */
typedef struct { const char *op; int lp; int rp; } op_entry_t;

/* 按 strslice（长度感知）查表；token 文本是源码切片，非 NUL 结尾。 */
static bool op_binding_len(const char *ptr, size_t len, int *lp, int *rp) {
    static const op_entry_t ops[] = {
        { "as", 21, 22 }, { "extends", 11, 12 },
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
            /* op 是 token pool 引用（源码切片，非 NUL 结尾），按长度比较 */
            op_binding_len(op.ptr, op.len, lp, rp);
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
        case AST_ENUM_REF: {
            ast_enum_ref_t *er = (ast_enum_ref_t *)node;
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
            ast_unwrap_t *u = (ast_unwrap_t *)node;
            render_expr(f, u->operand);
            size_t olen = 0;
            const char *op = token_get_text(u->op, &olen);
            sb_put(&f->sb, op, olen);
            break;
        }
        case AST_CONSTRUCT: {
            ast_construct_t *c = (ast_construct_t *)node;
            sb_ch(&f->sb, '.');
            if (c->type) render_expr(f, c->type);
            sb_ch(&f->sb, '{');
            render_comma_list(f, c->fields);
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
        case AST_FUNC_DEF: {
            /* 函数字面量（表达式位置） */
            render_func_literal(f, node);
            break;
        }
        case AST_ERROR: {
            /* 错误恢复：不渲染 */
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
    switch (node->kind) {
        case AST_VAR_DEF: {
            ast_var_def_t *v = (ast_var_def_t *)node;
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
            ast_struct_def_t *s = (ast_struct_def_t *)node;
            sb_str(&f->sb, "struct ");
            sb_put(&f->sb, s->name.ptr, s->name.len);
            sb_str(&f->sb, " {");
            if (!s->fields) {
                sb_str(&f->sb, "}");
            } else {
                sb_ch(&f->sb, ' ');
                render_semi_list(f, s->fields);
                sb_str(&f->sb, " }");
            }
            break;
        }
        case AST_STRUCT_FIELD: {
            ast_struct_field_t *sf = (ast_struct_field_t *)node;
            sb_put(&f->sb, sf->name.ptr, sf->name.len);
            sb_str(&f->sb, ": ");
            render_expr(f, sf->type);
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

static void render_func_def(fmt_t *f, ast_node_t *node) {
    ast_func_def_t *fn = (ast_func_def_t *)node;
    if (fn->is_comptime) sb_str(&f->sb, "comptime ");
    sb_str(&f->sb, "func ");
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
    sb_ch(&f->sb, ')');
    sb_str(&f->sb, ": ");
    render_expr(f, fn->return_expr);
    render_block(f, fn->body);
}

/* 函数字面量（表达式位置）：func [name](params): type { body }
 * name 可选（无 name = 匿名）；有 name 时仅作显示名（不绑定符号），
 * 格式化须保留以维持源码语义。 */
static void render_func_literal(fmt_t *f, ast_node_t *node) {
    ast_func_def_t *fn = (ast_func_def_t *)node;
    sb_str(&f->sb, "func");
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
    sb_ch(&f->sb, ')');
    sb_str(&f->sb, ": ");
    render_expr(f, fn->return_expr);
    render_block(f, fn->body);
}

/* ---- 顶层：PROGRAM ---- */

static void render_program(fmt_t *f, ast_node_t *node) {
    ast_program_t *prog = (ast_program_t *)node;
    uint32_t prev_end = 0;
    size_t last_line = 0;
    bool first = true;
    for (ast_node_t *fn = prog->funcs; fn; fn = fn->next) {
        if (!first) sb_ch(&f->sb, '\n');
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
