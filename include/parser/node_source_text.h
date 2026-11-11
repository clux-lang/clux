#ifndef _H_PARSER_NODE_SOURCE_TEXT_
#define _H_PARSER_NODE_SOURCE_TEXT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 源码原文片段：叶子节点，无子节点。
   location 指向源码中的原始文本区间，文本内容即该区间（不复制、不转义）。
   供 static_assert / assert 在编译期引用表达式源码。 */
node_t *create_source_text_node(allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
