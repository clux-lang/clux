#ifndef _H_CLUX_PARSER_PARSER_
#define _H_CLUX_PARSER_PARSER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/arena.h"
#include "core/allocator.h"
#include "core/vec.h"
#include "parser/ast_node.h"
#include "diag/diagnostic.h"
#include <stdbool.h>
#include <stdint.h>

/* ---- Parser context ---- */

typedef struct {
    allocator_t *alloc;       /* token / 临时分配 */
    arena_t     *arena;       /* AST 节点分配 */
    vec_t       *tokens;      /* token pool（由 driver 构建） */
    uint32_t     pos;         /* 当前游标（token pool 下标） */
    bool         has_error;   /* 已发生语法/词法错误（语法错误即终止，仅词法检查用） */
    diag_buf_t  *diag;        /* 共享诊断缓冲区；NULL 时错误不记录（如单测） */
} parser_t;

/**
 * 创建 parser 上下文。alloc 用于临时分配，arena 用于 AST 节点分配。
 * tokens 由 driver 构建的 token pool，parser 只持有引用。
 * 返回 NULL 表示参数无效或 OOM。
 */
parser_t *parser_create(allocator_t *alloc, arena_t *arena, vec_t *tokens);

/** 销毁 parser 上下文（不释放 arena 或 tokens）。Nullifies *pp。 */
void parser_destroy(parser_t **pp);

/**
 * 执行解析，返回 AST_PROGRAM 根节点。
 * 词法错误时返回 NULL（has_error = true）。
 * 语法错误时返回 AST_ERROR 节点（has_error = true）。
 */
ast_node_t *parser_parse(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_PARSER_ */
