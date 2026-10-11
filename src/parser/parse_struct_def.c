#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h" /* 结构体嵌入：`...Name;` */
#include "parser/ast_var_def.h"   /* 命名空间成员：var */
#include "parser/ast_func_def.h"  /* 命名空间成员：func */
#include "parser/ast_program.h"   /* 展开时追加到 prog->funcs */
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_error.h"
#include <stdio.h>

/* 兄弟链节点计数（结构体嵌入节点的 field_index 用：`...Base;` 出现在自己第
 * 几个字段之前 = 此刻已解析的字段数）。 */
static size_t count_siblings(const ast_node_t *head) {
    size_t n = 0;
    for (const ast_node_t *n2 = head; n2; n2 = n2->next) n++;
    return n;
}

/* 解析结构体定义 struct Name { field: type; ... }
 *
 * 语法：struct 名字 { 标识符 : 类型表达式 ; ... }
 * - 字段必须显式写 ': 类型'（缺 ':' 报错）
 * - 字段以分号分隔（与 enum variant 的逗号分隔不同）
 * - 字段列表可为空（{} 合法，C 语义 size=1）
 * - 体内可声明命名空间成员（var/func）与**嵌入**（`...Base;`，结构体组合）
 */
ast_node_t *parse_struct_def(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "struct")) return NULL;
    advance(p);
    skip_trivia(p);

    /* 结构体名：标识符 */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected struct name after 'struct'");
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 字段列表：{ ... } */
    if (!expect_symbol(p, "{")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '{' to start struct field list");
    }
    skip_trivia(p);

    ast_node_t *fields = NULL, *fields_last = NULL;
    ast_node_t *members = NULL, *members_last = NULL;
    ast_node_t *embeds = NULL, *embeds_last = NULL;

    for (;;) {
        skip_trivia(p);
        /* '}' 结束字段列表（含尾随分号后直接 '}' 的情况） */
        if (check_symbol(p, "}")) break;

        /* ---- 结构体嵌入（结构体组合）：`...Base;` ----
         * 纯语法：只记录名字与"出现在自己第几个字段之前"（此时 fields 链的
         * 长度就是 field_index）。字段的实际组合（遍历 Base 的 **type** 复制
         * 字段）由 sema 在**目标类型 seal 之前**按此下标完成——见
         * ast_struct_embed.h。
         * `...` 是 lexer 的**三字符** SYMBOL token（M6 可变参数复用同款 token，
         * 二者语境不重叠）。 */
        if (check_symbol(p, "...")) {
            uint32_t eb = p->pos;
            advance(p);
            skip_trivia(p);
            if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
                return ast_error_new(p->diag, p->tokens, p->arena, eb, p->pos,
                                     "expected struct name after '...'");
            }
            strslice_t ename = token_strslice(cur_token(p));
            advance(p);
            skip_trivia(p);

            ast_node_t *en = ast_struct_embed_new(p->arena, eb, p->pos);
            if (!en) return NULL;
            ast_struct_embed_t *emb = (ast_struct_embed_t *)en;
            emb->name        = ename;
            emb->field_index = (size_t)count_siblings(fields);
            ast_append(&embeds, &embeds_last, NULL, en);

            if (check_symbol(p, ";")) {
                advance(p);
                skip_trivia(p);
                continue;
            }
            break;
        }

        /* ---- 命名空间成员（语法糖）----
         *   var  data = 123;                         // 静态属性（本质：全局变量）
         *   func getId(self: ref *const T): i32 {…}  // 方法（本质：全局函数）
         * 复用顶层 parse_var_def / parse_func_def（名字先用**成员名**，展开阶段
         * 统一改写成 `T::name`）。 */
        if (check_keyword(p, "var")) {
            ast_node_t *mv = parse_var_def(p);
            if (!mv || mv->kind == AST_ERROR) return mv;
            ast_append(&members, &members_last, NULL, mv);
            continue;
        }
        if (check_keyword(p, "func")) {
            ast_node_t *mf = parse_func_def(p);
            if (!mf || mf->kind == AST_ERROR) return mf;
            ast_append(&members, &members_last, NULL, mf);
            continue;
        }

        /* 字段名：标识符 */
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected field name in struct definition");
        }
        uint32_t fb = p->pos;
        strslice_t fname = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);

        /* 字段类型：: expr（必须） */
        if (!check_symbol(p, ":")) {
            return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                 "expected ': type' after struct field name");
        }
        advance(p);
        skip_trivia(p);

        ast_node_t *ftype = parse_expr(p);
        if (!ftype || ftype->kind == AST_ERROR) {
            if (!ftype) {
                return ast_error_new(p->diag, p->tokens, p->arena, p->pos, p->pos,
                                     "expected field type expression after ':'");
            }
            return ftype;
        }
        skip_trivia(p);

        ast_node_t *fn = ast_struct_field_new(p->arena, fb, p->pos);
        if (!fn) return NULL;
        ((ast_struct_field_t *)fn)->name = fname;
        ((ast_struct_field_t *)fn)->type = ftype;
        ast_append(&fields, &fields_last, NULL, fn);

        /* 分号分隔，} 结束 */
        if (check_symbol(p, ";")) {
            advance(p);
            skip_trivia(p);
            continue;
        }
        break;
    }

    if (!expect_symbol(p, "}")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '}' to end struct field list");
    }

    ast_node_t *node = ast_struct_def_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_struct_def_t *)node)->name        = name;
    ((ast_struct_def_t *)node)->fields       = fields;
    ((ast_struct_def_t *)node)->fields_last  = fields_last;
    ((ast_struct_def_t *)node)->members      = members;
    ((ast_struct_def_t *)node)->members_last = members_last;
    ((ast_struct_def_t *)node)->embeds       = embeds;
    ((ast_struct_def_t *)node)->embeds_last  = embeds_last;
    return node;
}

/* ---- 命名空间成员展开：struct 体内的 var/func → 模块级全局声明 ----
 *
 * 把 `<结构体名>::<成员名>` 作为**声明名**（与 sema 的限定名解析、运行期的
 * 全局名字绑定一致），节点本身复用（改名 + 摘掉 next），追加到 prog->funcs
 * 尾部——此后它们就是普通的全局变量 / 函数，sema 与 compiler 无需特殊处理。
 *
 * 调用时机：compile_module 在 parser_parse 之后、sema 之前（fmt 路径不调用，
 * 成员已随结构体一起渲染，避免重复打印）。 */
void ast_expand_struct_members(arena_t *arena, ast_node_t *prog) {
    if (!arena || !prog || prog->kind != AST_PROGRAM) return;
    ast_program_t *p = (ast_program_t *)prog;

    /* 先收集结构体（遍历中会往 funcs 追加，故用快照式两阶段：先记录结构体）。 */
    for (ast_node_t *d = p->funcs; d; d = d->next) {
        if (d->kind != AST_STRUCT_DEF) continue;
        ast_struct_def_t *sd = (ast_struct_def_t *)d;
        if (!sd->members) continue;

        for (ast_node_t *m = sd->members; m;) {
            ast_node_t *next = m->next; /* 追加过程中会改 next，先存 */
            strslice_t mname;
            if (m->kind == AST_VAR_DEF) {
                mname = ((ast_var_def_t *)m)->name;
            } else if (m->kind == AST_FUNC_DEF) {
                mname = ((ast_func_def_t *)m)->name;
            } else {
                m = next;
                continue;
            }
            size_t total = sd->name.len + 2 + mname.len;
            char *mangled = (char *)arena_alloc(arena, total, 1);
            if (!mangled) return;
            memcpy(mangled, sd->name.ptr, sd->name.len);
            mangled[sd->name.len] = ':';
            mangled[sd->name.len + 1] = ':';
            if (mname.len) memcpy(mangled + sd->name.len + 2, mname.ptr, mname.len);
            strslice_t q = strslice_from_bytes(mangled, total);
            if (m->kind == AST_VAR_DEF) {
                ((ast_var_def_t *)m)->name = q;
            } else {
                ((ast_func_def_t *)m)->name = q;
            }
            m->next = NULL;
            ast_append(&p->funcs, &p->funcs_last, NULL, m);
            m = next;
        }
        sd->members = NULL;
        sd->members_last = NULL;
    }
}
