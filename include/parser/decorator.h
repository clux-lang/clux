#ifndef _H_PARSER_DECORATOR_
#define _H_PARSER_DECORATOR_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/node_decorator.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 解析一个装饰器。调用前当前 token 不一定是 '['，不是 `[[` 时返回 NULL
   且游标不动。`[[` 与 `]]` 都不在词法表中（与嵌套下标冲突），
   这里按相邻字符位置组合：起始两个 '[' 必须相邻，结束的两个 ']' 允许
   中间有 trivia。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_decorator(allocator_t *allocator, parser_t *parser);

/* 当前 token 是否开启一个装饰器（`[[` 两个相邻的 '['）。游标不动。 */
bool parser_peek_decorator(parser_t *parser);

/* 跳过 0 个或多个装饰器，返回其后的 token（不消费任何 token）。
   装饰器内部可能出现方括号，因此按 `[` / `]` 深度扫描，深度归零后的
   下一个 token 即声明关键字。 */
token_t *parser_peek_token_after_decorators(parser_t *parser);

/* 跳过 0 个或多个装饰器后，返回其后的 token 在 token 流中的位置
   （不消费任何 token）。用于需要继续前瞻关键字的场景。 */
size_t parser_get_position_after_decorators(parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
