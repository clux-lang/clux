#ifndef _H_PARSER_PROGRAM_
#define _H_PARSER_PROGRAM_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 程序根：解析整个 token 流为按源码顺序排列的顶层语句。
   调用前不要求游标停在有效 token 上，函数内部会跳过 trivia。
   保证每次循环至少消费一个 token：语句解析失败（返回 NULL）或解析成功但
   游标未前进时，报错并主动 parser_next，避免在同一位置反复失败。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_program(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
