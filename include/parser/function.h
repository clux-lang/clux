#ifndef _H_PARSER_FUNC_
#define _H_PARSER_FUNC_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 函数定义 / 函数类型 / 闭包，统一为一个节点。
   KEYWORD 区分 func 与 cfunc；NAME 为函数名（匿名函数与 cfunc 为 NULL）；
   GENERICS 为泛型参数列表（无泛型参数时为 NULL）；
   CAPTURES 为捕获列表（与形参列表同构，无捕获时为 NULL）；
   PARAMS 为形参列表节点；RETURN 为返回类型表达式；
   BODY 为函数体块，仅定义有值，函数类型为 NULL。
   定义与类型的分歧点在形参括号之后：':' 走定义，'->' 走类型。 */
#define CLUX_EXPR_FUNC_KEYWORD 0
#define CLUX_EXPR_FUNC_NAME 1
#define CLUX_EXPR_FUNC_GENERICS 2
#define CLUX_EXPR_FUNC_CAPTURES 3
#define CLUX_EXPR_FUNC_PARAMS 4
#define CLUX_EXPR_FUNC_RETURN 5
#define CLUX_EXPR_FUNC_BODY 6
#define CLUX_EXPR_FUNC_CHILDREN_SIZE 7

/* 泛型参数列表 [T, U, ...]：children 为 CLUX_NODE_GENERIC_PARAM 或 rest。 */

/* 单个泛型参数，按白皮书 §6.1–§6.5 的写法：
     T                      —— 类型参数
     N: comptime usize      —— 编译期值参数（TYPE 非 NULL）
     W extends IWriter[W]   —— 约束（CONSTRAINT 非 NULL）
     T = i32                —— 默认值（DEFAULT 非 NULL）
   NAME 必有；其余槽位按需出现，顺序为 TYPE → CONSTRAINT → DEFAULT。 */
#define CLUX_GENERIC_PARAM_NAME 0
#define CLUX_GENERIC_PARAM_TYPE 1
#define CLUX_GENERIC_PARAM_CONSTRAINT 2
#define CLUX_GENERIC_PARAM_DEFAULT 3
#define CLUX_GENERIC_PARAM_CHILDREN_SIZE 4

/* 形参列表：children 为 CLUX_NODE_FUNC_PARAM 或 rest 表达式，数量可变。 */

/* 单个形参：NAME / TYPE / DEFAULT 均可为 NULL。
   `name: type` 有 NAME；函数类型里的裸 `type` 只有 TYPE。
   只有闭包的捕获列表允许 `= default`（DEFAULT 非 NULL），
   函数参数没有默认值。 */
#define CLUX_FUNC_PARAM_NAME 0
#define CLUX_FUNC_PARAM_TYPE 1
#define CLUX_FUNC_PARAM_DEFAULT 2
#define CLUX_FUNC_PARAM_CHILDREN_SIZE 3

/* 函数定义 / 函数类型 / 闭包（白皮书 §3.3、§2.7）。
   调用前当前 token 不一定是 func/cfunc，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_func(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
