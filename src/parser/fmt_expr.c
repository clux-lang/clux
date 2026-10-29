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
#include "parser/ast_pack_type.h"
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
#include "parser/ast_new.h"
#include "parser/ast_addr.h"
#include "parser/ast_deref.h"
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

void render_expr(fmt_t *f, ast_node_t *node);

/* ---- 括号渲染与优先级 ---- */

/* 运算符优先级表（数值 = 绑定力，越大越紧）。赋值/三元最低（0）。 */
typedef struct { const char *op; int lp; int rp; } op_entry_t;


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

void render_expr(fmt_t *f, ast_node_t *node) {
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
            if (b->rhs) {
                switch (b->rhs->kind) {
                case AST_PAT_WILD:
                case AST_PAT_OR:
                case AST_PAT_AND:
                case AST_PAT_PTR:
                case AST_PAT_SLICE:
                case AST_PAT_ARRAY:
                case AST_PAT_TUPLE:
                case AST_PAT_NAME:
                case AST_PAT_FUNC:
                case AST_PAT_QUAL:
                    render_pattern(f, b->rhs);
                    break;
                default:
                    render_expr_paren(f, b->rhs, node, true, rp);
                    break;
                }
            }
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
        case AST_PACK_TYPE: {
            /* 函数参数类型 `...Args`（泛型参数包） */
            ast_pack_type_t *pt = (ast_pack_type_t *)node;
            sb_str(&f->sb, "...");
            sb_put(&f->sb, pt->name.ptr, pt->name.len);
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
            sb_ch(&f->sb, '*');
            render_expr(f, pt->base_type);
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
