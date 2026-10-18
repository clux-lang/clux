#include "parser/ast_ident.h"
#include "parser/parse_utils.h"

ast_node_t *parse_ident(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) { p->pos = tb; return NULL; }

    const token_t *t = cur_token(p);
    /* 保留名：`__` 前缀禁止（编译器生成名 / 未来魔法方法）。裸 `_` 在此放行
       —— 它是赋值目标的丢弃标记（`_ = expr`），语义在 sema 处理。 */
    {
        strslice_t probe = token_strslice(t);
        if (probe.len >= 2 && probe.ptr && probe.ptr[0] == '_' &&
            probe.ptr[1] == '_') {
            ast_node_t *rerr = parse_reserved_ident_error(p, t, "identifier");
            if (rerr) return rerr;
        }
    }
    strslice_t name = token_strslice(t);
    advance(p);

    ast_node_t *node = ast_ident_new(p->arena, tb, p->pos);
    ((ast_ident_t *)node)->name = name;
    return node;
}
