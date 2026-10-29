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
#include "parser/ast_interface_def.h"
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

void render_block(fmt_t *f, ast_node_t *node);
void render_captures(fmt_t *f, ast_node_t *captures);
void render_scope_annot_names(fmt_t *f, const strslice_t *names, size_t count);

/* ---- 语句渲染 ---- */

void render_stmt(fmt_t *f, ast_node_t *node) {
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
            for (ast_node_t *d = t->decorators; d; d = d->next) {
                sb_str(&f->sb, "[[");
                render_expr(f, d);
                sb_str(&f->sb, "]] ");
            }
            sb_str(&f->sb, "type ");
            sb_put(&f->sb, t->name.ptr, t->name.len);
            render_generic_params(f, &t->generic);
            sb_str(&f->sb, " = ");
            render_expr(f, t->expr);
            sb_ch(&f->sb, ';');
            break;
        }
        case AST_ENUM_DEF: {
            ast_enum_def_t *e = (ast_enum_def_t *)node;
            for (ast_node_t *d = e->decorators; d; d = d->next) {
                sb_str(&f->sb, "[[");
                render_expr(f, d);
                sb_str(&f->sb, "]] ");
            }
            sb_str(&f->sb, "enum ");
            sb_put(&f->sb, e->name.ptr, e->name.len);
            render_generic_params(f, &e->generic);
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
        case AST_INTERFACE_DEF: {
            ast_interface_def_t *id = (ast_interface_def_t *)node;
            sb_str(&f->sb, "interface ");
            sb_put(&f->sb, id->name.ptr, id->name.len);
            render_generic_params(f, &id->generic);
            sb_str(&f->sb, " {");
            if (!id->members && !id->embeds) {
                sb_ch(&f->sb, '}');
                break;
            }
            sb_ch(&f->sb, '\n');
            f->indent++;
            for (ast_node_t *e = id->embeds; e; e = e->next) {
                sb_indent(&f->sb, f->indent);
                sb_str(&f->sb, "...");
                ast_struct_embed_t *emb = (ast_struct_embed_t *)e;
                if (emb->type_expr)
                    render_expr(f, emb->type_expr);
                else
                    sb_put(&f->sb, emb->name.ptr, emb->name.len);
                sb_str(&f->sb, ";\n");
            }
            for (ast_node_t *m = id->members; m; m = m->next) {
                sb_indent(&f->sb, f->indent);
                if (m->kind == AST_VAR_DEF) {
                    ast_var_def_t *vd = (ast_var_def_t *)m;
                    sb_str(&f->sb, "var ");
                    sb_put(&f->sb, vd->name.ptr, vd->name.len);
                    sb_str(&f->sb, ": ");
                    render_expr(f, vd->type_expr);
                    sb_str(&f->sb, ";\n");
                } else if (m->kind == AST_FUNC_DEF) {
                    ast_func_def_t *fn = (ast_func_def_t *)m;
                    sb_str(&f->sb, "func ");
                    sb_put(&f->sb, fn->name.ptr, fn->name.len);
                    sb_ch(&f->sb, '(');
                    bool first = true;
                    for (ast_node_t *p = fn->params; p; p = p->next) {
                        if (!first) sb_str(&f->sb, ", ");
                        render_param(f, p);
                        first = false;
                    }
                    sb_str(&f->sb, "): ");
                    render_expr(f, fn->return_expr);
                    sb_str(&f->sb, ";\n");
                }
            }
            f->indent--;
            sb_indent(&f->sb, f->indent);
            sb_ch(&f->sb, '}');
            break;
        }
        case AST_STRUCT_DEF: {
            ast_struct_def_t *s0 = (ast_struct_def_t *)node;
            if (s0->is_bitfield) {
                for (ast_node_t *d = s0->decorators; d; d = d->next) {
                    sb_str(&f->sb, "[[");
                    render_expr(f, d);
                    sb_str(&f->sb, "]] ");
                }
                sb_str(&f->sb, "bitfield ");
                sb_put(&f->sb, s0->name.ptr, s0->name.len);
                sb_str(&f->sb, ": ");
                render_expr(f, s0->bitfield_underlying);
                sb_str(&f->sb, " {");
                f->indent++;
                for (size_t i = 0; i < s0->bitfield_field_count; i++) {
                    sb_ch(&f->sb, '\n');
                    sb_indent(&f->sb, f->indent);
                    ast_bitfield_field_t *bf = &s0->bitfield_fields[i];
                    sb_put(&f->sb, bf->name.ptr, bf->name.len);
                    char num[32];
                    snprintf(num, sizeof num, ": %u;", bf->width);
                    sb_str(&f->sb, num);
                }
                f->indent--;
                sb_ch(&f->sb, '\n');
                sb_indent(&f->sb, f->indent);
                sb_ch(&f->sb, '}');
                f->at_line_start = false;
                break;
            }
            /* struct 体多行展开：每个字段/嵌入/成员独占一行，4 空格缩进。
             * 字段/嵌入后补 ';'，func 成员自带函数体块。注释按源码位置挂载
             *（项间独立行注释 + 项后行内注释），空行保留——与 render_block
             * 同款规则。空 struct 紧凑 `{}`。 */
            ast_struct_def_t *s = (ast_struct_def_t *)node;
            for (ast_node_t *d = s->decorators; d; d = d->next) {
                sb_str(&f->sb, "[[");
                render_expr(f, d);
                sb_str(&f->sb, "]] ");
            }
            sb_str(&f->sb, "struct ");
            sb_put(&f->sb, s->name.ptr, s->name.len);
            render_generic_params(f, &s->generic);
            if (s->implements) {
                sb_str(&f->sb, " implements ");
                bool first_iface = true;
                for (ast_node_t *im = s->implements; im; im = im->next) {
                    if (!first_iface) sb_str(&f->sb, " & ");
                    render_expr(f, im);
                    first_iface = false;
                }
            }
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
                    for (ast_node_t *d = sf->decorators; d; d = d->next) {
                        sb_str(&f->sb, "[[");
                        render_expr(f, d);
                        sb_str(&f->sb, "]] ");
                    }
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
            for (ast_node_t *d = c->decorators; d; d = d->next) {
                sb_str(&f->sb, "[[");
                render_expr(f, d);
                sb_str(&f->sb, "]] ");
            }
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
            for (ast_node_t *d = u->decorators; d; d = d->next) {
                sb_str(&f->sb, "[[");
                render_expr(f, d);
                sb_str(&f->sb, "]] ");
            }
            sb_str(&f->sb, "union ");
            sb_put(&f->sb, u->name.ptr, u->name.len);
            render_generic_params(f, &u->generic);
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
void render_block(fmt_t *f, ast_node_t *node) {
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

void render_param(fmt_t *f, ast_node_t *n) {
    ast_var_def_t *v = (ast_var_def_t *)n;
    sb_put(&f->sb, v->name.ptr, v->name.len);
    sb_str(&f->sb, ": ");
    render_expr(f, v->type_expr);
}

void render_captures(fmt_t *f, ast_node_t *captures) {
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
void render_scope_annot_names(fmt_t *f, const strslice_t *names,
                                     size_t count) {
    if (!names || count == 0) return;
    sb_str(&f->sb, "'<");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_str(&f->sb, ", ");
        sb_put(&f->sb, names[i].ptr, names[i].len);
    }
    sb_ch(&f->sb, '>');
}
