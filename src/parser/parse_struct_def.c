#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h" /* 结构体嵌入：`...Name;` */
#include "parser/ast_var_def.h"   /* 命名空间成员：var */
#include "parser/ast_func_def.h"  /* 命名空间成员：func */
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
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "struct name");
        if (rerr) return rerr;
    }
    strslice_t name = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* M7 泛型参数表：struct Vec[T] { ... } */
    ast_generic_params_t generic = {0};
    if (check_symbol(p, "[")) {
        ast_node_t *gerr = parse_generic_params(p, &generic);
        if (gerr) return gerr;
    }

    /* M7 §31.5：implements 子句用 `&` 组合多个接口（与约束组合一致）：
           struct File implements IWriter[File] & IReader[File] { ... } */
    ast_node_t *implements = NULL, *implements_last = NULL;
    if (check_keyword(p, "implements")) {
        advance(p);
        skip_trivia(p);
        for (;;) {
            ast_node_t *iface = parse_type_with_index(p);
            if (!iface || iface->kind == AST_ERROR) {
                if (!iface)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected interface type after "
                                         "'implements'");
                return iface;
            }
            ast_append(&implements, &implements_last, NULL, iface);
            skip_trivia(p);
            if (!check_symbol(p, "&")) break;
            advance(p);
            skip_trivia(p);
        }
    }

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

        /* 装饰器前缀 `[[<expr>]]`：只允许修饰 var / func 成员
           （`[[dec]] func m(self: ref *S) {...}` 是方法装饰器）。 */
        ast_node_t *member_decorators = parse_decorator_prefixes(p);
        if (member_decorators && member_decorators->kind == AST_ERROR)
            return member_decorators;
        skip_trivia(p);
        if (check_symbol(p, "}")) {
            if (member_decorators) {
                return ast_error_new(p->diag, p->tokens, p->arena,
                                     member_decorators->tok_begin, p->pos,
                                     "decorators `[[...]]` must be followed by "
                                     "a 'var' or 'func' member declaration");
            }
            break;
        }

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
            {
                ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "struct name");
                if (rerr) return rerr;
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
            if (member_decorators) ((ast_var_def_t *)mv)->decorators = member_decorators;
            ast_append(&members, &members_last, NULL, mv);
            continue;
        }
        if (check_keyword(p, "func")) {
            ast_node_t *mf = parse_func_def(p);
            if (!mf || mf->kind == AST_ERROR) return mf;
            if (member_decorators) ((ast_func_def_t *)mf)->decorators = member_decorators;
            ast_append(&members, &members_last, NULL, mf);
            continue;
        }
        if (member_decorators && member_decorators->kind == AST_ERROR)
            return member_decorators;
        if (member_decorators) {
            /* 字段装饰器：[[metadata(...)]] / 普通标记，字段节点保存。 */
        }

        /* 字段名：标识符 */
        if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected field name in struct definition");
        }
        {
            ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "field name");
            if (rerr) return rerr;
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

        ast_node_t *ftype = true
                                ? parse_raw_type_atom(p)
                                : parse_expr(p);
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
        ((ast_struct_field_t *)fn)->decorators = member_decorators;
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
    ((ast_struct_def_t *)node)->generic      = generic;
    ((ast_struct_def_t *)node)->implements   = implements;
    ((ast_struct_def_t *)node)->implements_last = implements_last;
    return node;
}
