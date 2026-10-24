#include "parser/parser.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_interface_def.h"
#include "parser/ast_test.h"
#include "parser/ast_union_def.h"
#include "parser/ast_error.h"
#include "parser/ast_import.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_reexport.h"
#include "parser/parse_expr.h"
#include "parser/parse_stmt.h"
#include "parser/parse_utils.h"
#include "parser/ast_cunion_def.h"
#include "parser/lexer.h"
#include "parser/ast_node.h"
#include "core/vec.h"
#include "core/panic.h"

/* ---- Internal: class for parser_t ---- */

static class_t g_parser_class = {
    .name = "clux.parser",
    .size = sizeof(char),
    .clone_fn = NULL,
    .move_fn = NULL,
    .dispose_fn = NULL,
};

/* ---- Public API ---- */

parser_t *parser_create(allocator_t *alloc, arena_t *arena, vec_t *tokens) {
    if (!alloc || !arena || !tokens) return NULL;

    parser_t *p = (parser_t *)allocator_new_ex(
        alloc,
        g_parser_class.name,
        /*size=*/1,
        /*move_fn=*/NULL,
        /*clone_fn=*/NULL,
        /*dispose_fn=*/NULL,
        /*count=*/sizeof(parser_t));
    if (!p) panic("parser: out of memory");

    p->alloc     = alloc;
    p->arena     = arena;
    p->tokens    = tokens;
    p->pos       = 0;
    p->has_error = false;
    p->recover_partial = false;   /* 默认关闭：不影响 driver/测试既有行为 */
    p->diag      = NULL;
    return p;
}

void parser_destroy(parser_t **pp) {
    if (!pp || !*pp) return;
    parser_t *p = *pp;
    allocator_free(p->alloc, (void **)pp);
}

/* ---- parse_program: 顶层入口 ---- */

/**
 * 检查 token pool 中是否含有词法错误。
 * 如有：输出诊断 → 置 has_error → 返回 true。
 */
static bool has_lexical_errors(parser_t *p) {
    size_t count = vec_len(p->tokens);
    for (size_t i = 0; i < count; i++) {
        const token_t *t = (const token_t *)vec_get(p->tokens, i);
        if (token_get_kind(t) == TOKEN_TYPE_ERROR) {
            const location_t *loc = token_get_location(t);
            const char *msg = token_get_error_message(t);
            fprintf(stderr, "%s:%zu:%zu: error: %s\n",
                    loc ? loc->filename : "<unknown>",
                    loc ? loc->begin.line : 0,
                    loc ? loc->begin.column : 0,
                    msg ? msg : "lexical error");
            p->has_error = true;
            return true;
        }
    }
    return false;
}

/* 装饰器前缀 `[[<expr>]]`（零或多个）：返回表达式兄弟链（源码序，NULL = 无）。
 *
 * 只在**声明起始位置**调用，因此看到 `[` 就只可能是装饰器（顶层 / struct 体内
 * 没有以 `[` 开头的语句）。表达式沿用既有 parse_expr——`[[dec(1)]]`、
 * `[[a.b]]` 都自然支持。
 *
 * 出错时把诊断交给外层（解析错误节点已由 parse_expr/expect 记录），返回已收集
 * 的部分，避免二次报错。 */
ast_node_t *parse_decorator_prefixes(parser_t *p) {
    ast_node_t *head = NULL, *last = NULL;
    for (;;) {
        skip_trivia(p);
        if (!check_symbol(p, "[")) break;
        size_t save = p->pos;
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        if (!check_symbol(p, "[")) { /* 单个 `[`：不是装饰器 */
            p->pos = (uint32_t)save;
            break;
        }
        advance(p);
        skip_trivia(p);
        ast_node_t *expr = parse_expr(p);
        if (!expr || expr->kind == AST_ERROR) break;
        skip_trivia(p);
        if (!expect_symbol(p, "]")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ']]' to close decorator");
        }
        skip_trivia(p);
        if (!expect_symbol(p, "]")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ']]' to close decorator");
        }
        expr->next = NULL;
        expr->tok_begin = tb;
        if (last)
            last->next = expr;
        else
            head = expr;
        last = expr;
    }
    return head;
}

/* 顶层 test 块的判定：标识符 `test` 后跟字符串。不能把 test 做成全局
   关键字——现有代码/测试里有名为 test 的普通函数。 */
static bool at_test_block(parser_t *p) {
    const token_t *t = cur_token(p);
    if (!t || !token_is(t, "test")) return false;
    uint32_t save = p->pos;
    advance(p);
    skip_trivia(p);
    bool is_str = check_kind(p, TOKEN_TYPE_STRING);
    p->pos = save;
    return is_str;
}

ast_node_t *parse_program(parser_t *p) {
    uint32_t tb = p->pos;

    /* parse_program 没有父节点，入口自行跳 trivia */
    skip_trivia(p);

    ast_node_t *funcs      = NULL;
    ast_node_t *funcs_last = NULL;

    while (!at_end(p)) {
        ast_node_t *func = NULL;
        ast_node_t *decorators = parse_decorator_prefixes(p);
        if (at_end(p)) break;
        if (at_test_block(p)) {
            func = parse_test_block(p);
        } else if (check_keyword(p, "comptime")) {
            /* comptime 前缀：comptime func 标记 + 挂链；comptime var（全局
               编译期常量）挂 funcs 链，sema pass_globals 消费后摘除。 */
            uint32_t ctb = p->pos;
            advance(p);
            skip_trivia(p);
            if (check_keyword(p, "func")) {
                func = parse_func_def(p);
                if (func && func->kind != AST_ERROR)
                    ((ast_func_def_t *)func)->is_comptime = true;
            } else if (check_keyword(p, "var")) {
                func = parse_var_def(p);
                if (func && func->kind != AST_ERROR)
                    ((ast_var_def_t *)func)->is_comptime = true;
            } else {
                func = ast_error_new(p->diag, p->tokens, p->arena, ctb, p->pos,
                                     "expected 'func' or 'var' after 'comptime'");
            }
        } else if (check_keyword(p, "cfunc")) {
            /* M6 FFI：cfunc 定义（有体，C ABI）——与 func 同构，仅标志与关键字不同。 */
            func = parse_cfunc_def(p);
        } else if (check_keyword(p, "import")) {
            /* M5: import <alias> from "<path>"; — 导入模块为命名空间。
               sema 遇到此节点递归编译被导入模块，在当前模块全局作用域注册
               <alias> 为 MODULE 符号。compiler 不生成字节码（模块加载在 sema
               阶段完成）。 */
            func = parse_import(p);
        } else if (check_keyword(p, "extern")) {
            /* M6 FFI：extern lib <alias> from "<name>";（动态库声明）
               判据是关键字，不做后缀判类。extern cfunc 在后续阶段接入同一前缀。 */
            uint32_t xtb = p->pos;
            advance(p);
            skip_trivia(p);
            if (check_keyword(p, "lib")) {
                func = parse_extern_lib(p, xtb);
            } else if (check_keyword(p, "cfunc")) {
                func = parse_extern_cfunc(p, xtb);
            } else if (check_keyword(p, "func")) {
                /* M6 FFI：`extern func <库>::<名>(...): T;` —— 与 `extern cfunc`
                   完全等价（限定名已表明它来自外部库）。规范写法是 `extern cfunc`
                   （显式标注 C ABI），fmt 统一渲染成它。 */
                func = parse_extern_cfunc_kw(p, xtb, "func");
            } else if (check_keyword(p, "var")) {
                /* M6 FFI：extern var <库>::<名>: T;（动态库导出的数据符号） */
                func = parse_extern_var(p, xtb);
            } else {
                func = ast_error_new(p->diag, p->tokens, p->arena, xtb, p->pos,
                                     "expected 'lib' after 'extern' "
                                     "(M6: extern lib / extern cfunc / extern func / "
                                     "extern var)");
            }        } else if (check_keyword(p, "export")) {
            /* M5: export <decl> — 前缀修饰，标记全局定义进入模块导出表。
               消费 export 关键字后递归调用现有声明解析器，设置 is_exported=true。
               允许 export func / export var / export type / export enum /
               export struct / export union / export cunion；
               另有 export * from "path"（代理导出 → AST_REEXPORT）。 */
            uint32_t etb = p->pos;
            advance(p);
            skip_trivia(p);
            bool is_reexport = false;
            if (check_symbol(p, "*")) {
                func = parse_reexport(p);
                is_reexport = true;
                if (func && func->kind != AST_ERROR) {
                    /* 诊断跨度从 `export` 起（parse_reexport 从 '*' 起算） */
                    func->tok_begin = etb;
                }
            } else if (check_keyword(p, "func")) {
                func = parse_func_def(p);
            } else if (check_keyword(p, "cfunc")) {
                /* `export cfunc`：C ABI 函数（无 env）也可导出，
                   头文件里是 extern 函数声明。 */
                func = parse_cfunc_def(p);
            } else if (check_keyword(p, "var")) {
                func = parse_var_def(p);
            } else if (check_keyword(p, "type")) {
                func = parse_type_def(p);
            } else if (check_keyword(p, "enum")) {
                func = parse_enum_def(p);
            } else if (check_keyword(p, "struct")) {
                func = parse_struct_def(p);
            } else if (check_keyword(p, "union")) {
                func = parse_union_def(p);
            } else if (check_keyword(p, "cunion")) {
                func = parse_cunion_def(p);
            } else if (check_keyword(p, "interface")) {
                func = parse_interface_def(p);
            } else {
                func = ast_error_new(p->diag, p->tokens, p->arena, etb, p->pos,
                                     "expected declaration after 'export'");
            }
            /* export 前缀只标记"声明"进入导出表；代理导出由 AST_REEXPORT
               自身承载语义，不设 is_exported。 */
            if (func && func->kind != AST_ERROR && !is_reexport)
                func->is_exported = true;
        } else if (check_keyword(p, "type")) {
            /* 全局类型定义 type name = <type-expr>; 挂 funcs 链：
               sema pass1b 求值折叠（AST_TYPE_REF）后保留——进入字节码，
               运行时 LOAD_TYPE/PUSH; PUSH_UNDEFINED; DEFINE 绑定 type value。 */
            func = parse_type_def(p);
        } else if (check_keyword(p, "enum")) {
            /* 顶层枚举定义 enum Name:Underlying { Var = val, ... } 挂
               funcs 链：sema pass1b 折叠 variant 值 + 登记类型，进入字节码，
               运行时 hoist 构造 enum 类型 + 绑定名字。 */
            func = parse_enum_def(p);
        } else if (check_keyword(p, "bitfield")) {
            func = parse_bitfield_def(p);
        } else if (check_keyword(p, "struct")) {
            /* 顶层结构体定义 struct Name { field: type; ... } 挂 funcs 链：
               sema pass1b 折叠字段类型 + 登记类型，进入字节码，运行时 hoist
               构造 struct 类型 + 绑定名字。 */
            func = parse_struct_def(p);
        } else if (check_keyword(p, "union")) {
            /* 顶层 tag union 定义 union Name { field: type; ... } 挂
               funcs 链：sema pass1b 折叠字段类型 + 登记类型（tag 整数前缀
               + member payload 联合体布局），进入字节码，运行时 hoist 构造
               union 类型 + 绑定名字。 */
            func = parse_union_def(p);
        } else if (check_keyword(p, "cunion")) {
            /* 顶层 C 语义 union 定义 cunion Name { field: type; ... } 挂
               funcs 链：sema pass1b 折叠字段类型 + 登记类型（无 tag，所有
               member 共享 offset 0，C FFI 布局），进入字节码，运行时 hoist
               构造 cunion 类型 + 绑定名字。 */
            func = parse_cunion_def(p);
        } else if (check_keyword(p, "interface")) {
            /* M7 §31：interface 是编译期实体（无运行时表示），仍挂在
               prog->funcs 链上供 sema 登记符号/检查 implements。 */
            func = parse_interface_def(p);
        } else if (check_keyword(p, "var")) {
            /* 全局变量定义 var name[:type] = init; 挂 funcs 链（非 comptime）。
               sema pass_globals 折叠 init 为字面量/函数引用后保留——进入
               字节码，运行时 DEFINE 到模块 root_scope（与全局函数绑定同层，
               函数体经 root_scope 查找可见）。区别于 comptime var（编译期
               常量，pass_globals 摘除不进运行时）。 */
            func = parse_var_def(p);
        } else if (check_symbol(p, ";")) {
            /* 顶层空语句：单独分号（;）——无操作占位，跳过 */
            advance(p);
            skip_trivia(p);
            continue;
        } else {
            func = parse_func_def(p);
        }
        if (decorators && decorators->kind == AST_ERROR) {
            func = decorators; /* 装饰器语法错误：替换整个声明 */
        } else if (decorators && func && func->kind != AST_ERROR) {
            if (func->kind == AST_FUNC_DEF) {
                ((ast_func_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_VAR_DEF) {
                ((ast_var_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_STRUCT_DEF) {
                ((ast_struct_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_ENUM_DEF) {
                ((ast_enum_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_UNION_DEF) {
                ((ast_union_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_CUNION_DEF) {
                ((ast_cunion_def_t *)func)->decorators = decorators;
            } else if (func->kind == AST_TYPE_DEF) {
                ((ast_type_def_t *)func)->decorators = decorators;
            } else {
                func = ast_error_new(
                    p->diag, p->tokens, p->arena, func->tok_begin, p->pos,
                    "decorators `[[...]]` are only allowed on 'var' and "
                    "'func' declarations");
            }
        }
        if (!func) {
            /* 语法错误。recover_partial（formatter 用）时保留错误前的 AST：
               已解析的顶层节点仍挂到 PROGRAM，tok_end 停在错误 token 处。 */
            if (p->recover_partial) {
                p->has_error = true;
                break;
            }
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected function definition at top level");
        }
        if (func->kind == AST_ERROR) {
            if (p->recover_partial) {
                p->has_error = true;
                break;
            }
            return func;
        }

        ast_append(&funcs, &funcs_last, NULL, func);
        skip_trivia(p);
    }

    ast_node_t *node = ast_program_new(p->arena, tb, p->pos);
    ast_program_t *prog = (ast_program_t *)node;
    prog->funcs      = funcs;
    prog->funcs_last = funcs_last;
    return node;
}

ast_node_t *parser_parse(parser_t *p) {
  if (!p) return NULL;

  /* 词法错误检查：有词法错误则不进入解析 */
  if (has_lexical_errors(p)) return NULL;

  return parse_program(p);
}
