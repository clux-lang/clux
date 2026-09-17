#include "parser/ast_nil.h"
#include "parser/parse_utils.h"

ast_node_t *parse_nil(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "nil")) { p->pos = tb; return NULL; }

    const token_t *t = cur_token(p);
    (void)t;
    advance(p);

    return ast_nil_new(p->arena, tb, p->pos);
}
