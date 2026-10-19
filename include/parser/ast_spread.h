#ifndef _H_CLUX_PARSER_AST_SPREAD_
#define _H_CLUX_PARSER_AST_SPREAD_

#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/* 实参展开 `...expr`（仅出现在**函数调用的实参位置**）：把编译期已知长度的
 * 数组/元组"摊开"成多个实参。
 *
 *   fn(a, ...b, c)   ⇒   fn(a, b[0], b[1], …, c)
 *
 * 只存在于 parser 与 sema 之间：sema 在解析实参之前把它展开成 AST_INDEX 链，
 * compiler / CTFE 看不到本节点。
 *
 * 操作数必须是**可复读的位置表达式**（变量 / 成员访问）：展开后每个元素都是
 * 一次 `operand[i]` 读取，若操作数带副作用（调用、构造…）就会被重复求值，
 * 因此 sema 只接受标识符 / 成员访问。 */
typedef struct {
  ast_node_t base;      /* kind = AST_SPREAD */
  ast_node_t *operand;  /* 被展开的表达式（数组 / 元组，长度须编译期已知） */
} ast_spread_t;

static inline ast_node_t *ast_spread_new(arena_t *arena, uint32_t tok_begin,
                                         uint32_t tok_end) {
  ast_spread_t *n = (ast_spread_t *)arena_calloc(
      arena, 1, sizeof(ast_spread_t), ALIGNOF(max_align_t));
  if (!n) return NULL;
  n->base.kind = AST_SPREAD;
  n->base.tok_begin = tok_begin;
  n->base.tok_end = tok_end;
  return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SPREAD_ */
