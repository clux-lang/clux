#ifndef _H_CLUX_PARSER_AST_KIND_
#define _H_CLUX_PARSER_AST_KIND_

typedef enum {
    /* --- 顶层 --- */
    AST_PROGRAM,         /* 函数定义列表 */
    AST_FUNC_DEF,        /* func name(params):type { body } */

    /* --- 语句 --- */
    AST_VAR_DEF,         /* var name[:type] [= init]; */
    AST_ASSIGN,          /* name = expr; / name += expr; */
    AST_IF,              /* if cond { then } [else { else_body }] */
    AST_WHILE,           /* while cond { body } */
    AST_FOR,             /* for (init; cond; update) { body } */
    AST_RETURN,          /* return [expr]; */
    AST_BREAK,           /* break; */
    AST_CONTINUE,        /* continue; */
    AST_BLOCK,           /* { stmts... } */
    AST_EXPR_STMT,       /* expr;（表达式作为语句） */
    /* AST_DISCARD removed: _ = expr is AST_ASSIGN, discard semantics in Sema */

    /* --- 表达式 --- */
    AST_BINARY,          /* lhs op rhs */
    AST_UNARY,           /* op expr */
    AST_CALL,            /* callee(args...) */
    AST_MEMBER,          /* expr.field */
    AST_INDEX,           /* expr[expr, ...]（下标 / 泛型实例化，语义阶段区分） */
    AST_ARRAY,           /* [N]T 数组类型表达式（N=长度，T=基础类型） */
    AST_CONSTRUCT,       /* .T { f1, f2, ... } 类型字面量构造（'.' 前导） */
    AST_INT_LIT,         /* 整数字面量 */
    AST_FLOAT_LIT,       /* 浮点字面量 */
    AST_BOOL_LIT,        /* true / false */
    AST_STRING_LIT,      /* "..."（escape 展开后文本） */
    AST_CHAR_LIT,        /* 'a'（u8 码点值） */
    AST_IDENT,           /* 标识符引用 */
    AST_UNDEF,           /* undefined（未初始化声明标记，sema 数据流分析消费） */

    AST_ERROR,           /* 解析错误恢复节点（记录错误位置，占位） */

    /* --- 类型修饰（类型即表达式，M2 关键架构决策 6）---
     * const/volatile 是真实类型 kind（有 vtable 代理），以嵌套节点表达
     * 递归修饰：const i32 → AST_CONST(AST_IDENT("i32"))；
     * volatile const i32 → AST_VOLATILE(AST_CONST(...))。
     * 无顺序约束，const const i32 嵌套重复合法（语义上幂等，消费层收敛）。 */
    AST_CONST,           /* const <type-expr> */
    AST_VOLATILE,        /* volatile <type-expr> */
    AST_TYPE_REF,        /* 类型引用：sema 登记的具名类型（__type_N）→ LOAD_TYPE <id> */
    AST_TERNARY,         /* cond ? then : else 三元条件表达式 */

    AST_KIND_COUNT,      /* 哨兵值，用于数组索引 */
} ast_kind_t;

#endif /* _H_CLUX_PARSER_AST_KIND_ */
