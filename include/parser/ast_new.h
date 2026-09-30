#ifndef _H_CLUX_PARSER_AST_NEW_
#define _H_CLUX_PARSER_AST_NEW_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/* new 表达式（m3-design §8.1）：new T{...} —— .T{...} 的堆分配形态，
 * 返回 own *T。type 为类型表达式（NULL = 匿名构造，sema 依上下文推断），
 * fields 为构造字段值表达式兄弟链（与 AST_CONSTRUCT 同构）。 */
typedef struct {
    ast_node_t  base;
    ast_node_t *type;          /* 类型表达式（NULL = 匿名构造） */
    ast_node_t *fields;        /* 字段值表达式兄弟链（head） */
    ast_node_t *fields_last;   /* 字段值表达式兄弟链（tail） */
} ast_new_t;

static inline ast_node_t *ast_new_new(arena_t *arena,
                                      uint32_t tok_begin, uint32_t tok_end) {
    ast_new_t *n = (ast_new_t *)arena_calloc(
        arena, 1, sizeof(ast_new_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_NEW;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_NEW_ */
