#include "parser/parse_stmt.h"
#include "parser/parse_expr.h"
#include "parser/parse_comptime_if.h"
#include "parser/parse_foreach.h"
#include "parser/parse_utils.h"
#include "parser/ast_ident.h"
#include "parser/ast_assign.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_var_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_block.h"
#include "parser/ast_defer.h"
#include "parser/ast_if.h"
#include "parser/ast_switch.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_for.h"
#include "parser/ast_return.h"
#include "parser/ast_error.h"
#include "core/strslice.h"

/* ================================================================ */
/* parse_stmt: 语句分派器                                             */
/* ================================================================ */

ast_node_t *parse_stmt(parser_t *p) {
    /* 装饰器 `[[<expr>]]` **只允许顶层**（全局 var / 全局 func / struct 成员）。
       局部变量/局部函数上出现装饰器 → 明确拒绝：装饰器展开是"全局绑定"语义
       （`var a = ann(v)` / `var f = ann(源函数)`，依赖模块初始化期折叠），
       在函数体里没有对应形态。这里先看清 `[[` 再报，避免落成难懂的
       "expected expression"。 */
    if (check_symbol(p, "[")) {
        size_t save = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "[")) {
            p->pos = (uint32_t)save;
            uint32_t tb = p->pos;
            (void)parse_decorator_prefixes(p);
            skip_trivia(p);
            return ast_error_new(
                p->diag, p->tokens, p->arena, tb, p->pos,
                "decorators `[[...]]` are only allowed on top-level 'var' / "
                "'func' declarations (and struct members), not on local "
                "declarations");
        }
        p->pos = (uint32_t)save; /* 单个 `[`：数组/下标表达式，交给表达式语句 */
    }
    /* `cfunc` 只能**顶层**（且必须有名字）：cfunc 没有闭包/捕获环境，局部声明
       没有对应运行时形态（它的唯一用途是把函数地址交给 C，或作为
       `cfunc(...)` 类型的值参与 FFI 边界）。这里给出明确诊断，而不是落到
       表达式语句的 "expected ';'"。 */
    if (check_keyword(p, "cfunc")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        return ast_error_new(
            p->diag, p->tokens, p->arena, tb, p->pos,
            "cfunc definitions are only allowed at top level (a cfunc has no "
            "closure environment, so a local one has no meaning); declare it "
            "globally and use `cfunc_ptr(name)` when passing it to C");
    }
    if (check_keyword(p, "comptime")) return parse_comptime_stmt(p);
    /* foreach：for-each 语法糖（前脱糖展开成索引 for 循环） */
    if (check_keyword(p, "foreach")) return parse_foreach(p);
    if (check_keyword(p, "var"))    return parse_var_def(p);
    if (check_keyword(p, "func"))   return parse_func_def(p);
    if (check_keyword(p, "type"))   return parse_type_def(p);
    if (check_keyword(p, "enum"))   return parse_enum_def(p);
    if (check_keyword(p, "struct"))  return parse_struct_def(p);
    if (check_keyword(p, "union"))   return parse_union_def(p);
    if (check_keyword(p, "cunion")) return parse_cunion_def(p);
    if (check_keyword(p, "if"))     return parse_if(p);
    if (check_keyword(p, "switch")) return parse_switch(p);
    if (check_keyword(p, "while"))  return parse_while(p);
    if (check_keyword(p, "do"))     return parse_dowhile(p);
    if (check_keyword(p, "for"))    return parse_for(p);
    if (check_keyword(p, "return")) return parse_return(p);
    if (check_keyword(p, "break"))  return parse_break(p);
    if (check_keyword(p, "continue")) return parse_continue(p);
    if (check_keyword(p, "defer"))  return parse_defer(p);
    /* 空语句：单独的分号（;）——无操作占位 */
    if (check_symbol(p, ";")) {
        uint32_t tb = p->pos;
        advance(p);
        return ast_node_new(p->arena, AST_EMPTY_STMT, tb, p->pos);
    }
    /* 裸块语句：{ stmt; ... }（独立作用域） */
    if (check_symbol(p, "{"))   return parse_block(p);
    /* 最后尝试表达式语句（赋值已是表达式的一种） */
    return parse_assign_or_expr_stmt(p);
}

/* ================================================================ */
/* parse_comptime_stmt: comptime 前缀（comptime var / comptime func） */
/* ================================================================ */

ast_node_t *parse_comptime_stmt(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "comptime")) return NULL;
    advance(p);
    skip_trivia(p);

    if (check_keyword(p, "var")) {
        ast_node_t *n = parse_var_def(p);
        if (n && n->kind != AST_ERROR) ((ast_var_def_t *)n)->is_comptime = true;
        return n;
    }
    if (check_keyword(p, "func")) {
        ast_node_t *n = parse_func_def(p);
        if (n && n->kind != AST_ERROR) ((ast_func_def_t *)n)->is_comptime = true;
        return n;
    }
    /* comptime if：编译期条件语句（sema Pass 3a 做语法树裁剪） */
    if (check_keyword(p, "if")) return parse_comptime_if(p);
    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                         "expected 'var', 'func' or 'if' after 'comptime'");
}

/* ================================================================ */
/* parse_break / parse_continue: break; / continue;                  */
/* ================================================================ */

ast_node_t *parse_break(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "break")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after 'break'");
    }

    return ast_node_new(p->arena, AST_BREAK, tb, p->pos);
}

ast_node_t *parse_continue(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "continue")) return NULL;
    advance(p);
    skip_trivia(p);

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after 'continue'");
    }

    return ast_node_new(p->arena, AST_CONTINUE, tb, p->pos);
}

/* ================================================================ */
/* parse_assign_or_expr_stmt: 表达式 + ; → 语句                      */
/*                                                                  */
/* 赋值已在 parse_expr 中作为表达式处理（最低优先级、右结合）。         */
/* 此函数只负责消费末尾 ; 并将表达式包装为适当的语句节点。              */
/* ================================================================ */

ast_node_t *parse_assign_or_expr_stmt(parser_t *p) {
    uint32_t tb = p->pos;

    ast_node_t *expr = parse_expr(p);
    if (!expr) return NULL;
    if (expr->kind == AST_ERROR) return expr;

    skip_trivia(p);
    if (!expect_symbol(p, ";")) {
        if (expr->kind == AST_ASSIGN) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ';' after assignment");
        }
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after expression statement");
    }

    /* 赋值已经是完整语句节点 */
    if (expr->kind == AST_ASSIGN) return expr;

    /* 普通表达式 → 包装为 AST_EXPR_STMT */
    ast_node_t *stmt = ast_expr_stmt_new(p->arena, tb, p->pos);
    ((ast_expr_stmt_t *)stmt)->expr = expr;
    return stmt;
}
