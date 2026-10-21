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

void render_node(fmt_t *f, ast_node_t *node);

void render_func_def(fmt_t *f, ast_node_t *node) {
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
void render_func_literal(fmt_t *f, ast_node_t *node) {
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

void render_program(fmt_t *f, ast_node_t *node) {
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
void render_node(fmt_t *f, ast_node_t *node) {
    if (!node) return;
    if (node->kind == AST_FUNC_DEF) {
        render_func_def(f, node);
        return;
    }
    render_stmt(f, node);
}
