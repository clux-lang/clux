#ifndef _H_PARSER_DECORATOR_
#define _H_PARSER_DECORATOR_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 装饰器节点 [[ <expr> ]]（白皮书 §3.2、§9.1）。
   不在主节点树上：由顶层声明、struct 字段/方法等特定语句主动调用，
   作为它们的子节点挂载。children 只有 EXPR 一个槽。 */
#define CLUX_DECORATOR_EXPR 0
#define CLUX_DECORATOR_CHILDREN_SIZE 1

/* 解析一个装饰器。调用前当前 token 不一定是 '['，不是 `[[` 时返回 NULL
   且游标不动。`[[` 与 `]]` 都不在词法表中（与嵌套下标冲突），
   这里按相邻字符位置组合：起始两个 '[' 必须相邻，结束的两个 ']' 允许
   中间有 trivia。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_decorator(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
