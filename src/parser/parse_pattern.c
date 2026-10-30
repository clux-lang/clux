#include "parser/ast_pattern.h"

#include "parser/ast_error.h"
#include "parser/ast_int_lit.h"
#include "parser/lexer.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"

#include <string.h>

/* ===========================================================================
 * M7 `extends` PATTERN 解析（docs/m7-design.md §27–§29）
 *
 *   pattern   := or
 *   or        := and ('|' and)*
 *   and       := atom ('&' atom)*
 *   atom      := '(' pattern ')' | '?' | '?*' atom | '*' atom
 *              | own/ref/fatal/share/weak ('*' atom | '[]' atom)
 *              | '[]' atom | '[' ('?'|INT) ']' atom
 *              | '<' '?' '>' | '<' (pattern (',' pattern)*)? '>'
 *              | func '(' pattern,... ')' '->' atom
 *              | NAME ('[' pattern,... ']')?
 *              | const|volatile atom
 * ========================================================================= */

static ast_node_t *parse_pattern_atom(parser_t *p);

static bool is_pattern_name_token(const token_t *t) {
    token_kind_t k = token_get_kind(t);
    return k == TOKEN_TYPE_IDENTIFIER || k == TOKEN_TYPE_KEYWORD;
}

static ast_node_t *parse_pattern_or(parser_t *p);

/* `[` 之后：长度 '?' 或整数字面量，然后 ']' + 元素 PATTERN。 */
static ast_node_t *parse_pattern_array(parser_t *p, uint32_t tb) {
    ast_node_t *length = NULL;
    if (check_symbol(p, "?")) {
        advance(p);
        skip_trivia(p);
    } else {
        length = parse_int_lit(p);
        if (!length) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "array length in 'extends' pattern must be an "
                                 "integer literal or '?' (variables are not "
                                 "allowed)");
        }
        skip_trivia(p);
    }
    if (!expect_symbol(p, "]")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ']' in array pattern");
    }
    skip_trivia(p);
    ast_node_t *elem = parse_pattern_atom(p);
    if (!elem || elem->kind == AST_ERROR) {
        if (!elem)
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected element pattern after ']'");
        return elem;
    }
    ast_node_t *n = ast_pat_array_new(p->arena, tb, p->pos);
    if (!n) return NULL;
    ((ast_pat_array_t *)n)->length = length;
    ((ast_pat_array_t *)n)->elem_type = elem;
    return n;
}

/* `<...>` / `<?>` 元组 PATTERN。 */
static ast_node_t *parse_pattern_tuple(parser_t *p, uint32_t tb) {
    advance(p); /* '<' */
    skip_trivia(p);

    ast_node_t *n = ast_pat_tuple_new(p->arena, tb, p->pos);
    if (!n) return NULL;
    ast_pat_tuple_t *tp = (ast_pat_tuple_t *)n;

    if (check_symbol(p, "?")) {
        /* 可能是 <?>（任意元数）或 <?, ...>（首元素通配） */
        uint32_t save = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, ">")) {
            advance(p);
            skip_trivia(p);
            tp->any_arity = true;
            tp->base.tok_end = p->pos;
            return n;
        }
        p->pos = save; /* 回退，按普通元素解析 */
        skip_trivia(p);
    }

    if (!check_symbol(p, ">")) {
        for (;;) {
            ast_node_t *e = parse_pattern_or(p);
            if (!e || e->kind == AST_ERROR) {
                if (!e)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected tuple element pattern");
                return e;
            }
            e->next = NULL;
            if (!tp->elem_types) tp->elem_types = e;
            else tp->elem_types_last->next = e;
            tp->elem_types_last = e;
            skip_trivia(p);
            if (!check_symbol(p, ",")) break;
            advance(p);
            skip_trivia(p);
        }
    }
    if (!expect_symbol(p, ">")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '>' after tuple pattern");
    }
    skip_trivia(p);
    tp->base.tok_end = p->pos;
    return n;
}

static ast_node_t *parse_pattern_func(parser_t *p, uint32_t tb) {
    advance(p); /* func / cfunc */
    skip_trivia(p);
    if (!expect_symbol(p, "(")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '(' in function pattern");
    }
    skip_trivia(p);
    ast_node_t *params = NULL, *params_last = NULL;
    if (!check_symbol(p, ")")) {
        for (;;) {
            ast_node_t *e = parse_pattern_or(p);
            if (!e || e->kind == AST_ERROR) {
                if (!e)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected function parameter pattern");
                return e;
            }
            e->next = NULL;
            if (!params) params = e;
            else params_last->next = e;
            params_last = e;
            skip_trivia(p);
            if (!check_symbol(p, ",")) break;
            advance(p);
            skip_trivia(p);
        }
    }
    if (!expect_symbol(p, ")")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ')' after function pattern parameters");
    }
    skip_trivia(p);
    if (!expect_symbol(p, "->")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected '->' in function pattern");
    }
    skip_trivia(p);
    ast_node_t *ret = parse_pattern_atom(p);
    if (!ret || ret->kind == AST_ERROR) {
        if (!ret)
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected return pattern after '->'");
        return ret;
    }
    ast_node_t *n = ast_pat_func_new(p->arena, tb, p->pos);
    if (!n) return NULL;
    ((ast_pat_func_t *)n)->params = params;
    ((ast_pat_func_t *)n)->params_last = params_last;
    ((ast_pat_func_t *)n)->return_pat = ret;
    return n;
}

static ast_node_t *parse_pattern_atom(parser_t *p) {
    skip_trivia(p);
    uint32_t tb = p->pos;

    /* 分组 */
    if (check_symbol(p, "(")) {
        advance(p);
        skip_trivia(p);
        ast_node_t *inner = parse_pattern_or(p);
        if (!inner || inner->kind == AST_ERROR) return inner;
        skip_trivia(p);
        if (!expect_symbol(p, ")")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ')' after grouped pattern");
        }
        skip_trivia(p);
        return inner;
    }

    /* '?' 单类型通配，或 '?*<pat>' 指针模式 */
    if (check_symbol(p, "?")) {
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "*")) {
            advance(p);
            skip_trivia(p);
            ast_node_t *base = parse_pattern_atom(p);
            if (!base || base->kind == AST_ERROR) {
                if (!base)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected pointee pattern after '?*'");
                return base;
            }
            ast_node_t *n = ast_pat_ptr_new(p->arena, tb, p->pos);
            if (!n) return NULL;
            ((ast_pat_ptr_t *)n)->wildcard = true;
            ((ast_pat_ptr_t *)n)->base_type = base;
            return n;
        }
        return ast_pat_wild_new(p->arena, tb, p->pos);
    }

    /* 裸 '*<pat>' */
    if (check_symbol(p, "*")) {
        advance(p);
        skip_trivia(p);
        ast_node_t *base = parse_pattern_atom(p);
        if (!base || base->kind == AST_ERROR) {
            if (!base)
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected pointee pattern after '*'");
            return base;
        }
        ast_node_t *n = ast_pat_ptr_new(p->arena, tb, p->pos);
        if (!n) return NULL;
        ((ast_pat_ptr_t *)n)->base_type = base;
        return n;
    }

    /* 所有权前缀已从语言移除：own/ref/fatal 与 RC 的 share/weak 一律报错。 */
    if (check_keyword(p, "own") || check_keyword(p, "ref") ||
        check_keyword(p, "fatal") || check_keyword(p, "share") ||
        check_keyword(p, "weak")) {
        const token_t *bad = cur_token(p);
        p->has_error = true;
        return ast_error_new(
            p->diag, p->tokens, p->arena, tb, p->pos + 1,
            "'%.*s' was removed with compile-time ownership; write '*T' / "
            "'[]T' without an ownership prefix",
            (int)token_strslice(bad).len, token_strslice(bad).ptr);
    }

    /* 裸切片 []<pat> */
    if (check_symbol(p, "[")) {
        advance(p);
        skip_trivia(p);
        if (check_symbol(p, "]")) {
            advance(p);
            skip_trivia(p);
            ast_node_t *elem = parse_pattern_atom(p);
            if (!elem || elem->kind == AST_ERROR) {
                if (!elem)
                    return ast_error_new(p->diag, p->tokens, p->arena, tb,
                                         p->pos,
                                         "expected element pattern after '[]'");
                return elem;
            }
            ast_node_t *n = ast_pat_slice_new(p->arena, tb, p->pos);
            if (!n) return NULL;
            ((ast_pat_slice_t *)n)->elem_type = elem;
            return n;
        }
        ast_node_t *arr = parse_pattern_array(p, tb);
        return arr;
    }

    /* 元组 */
    if (check_symbol(p, "<")) return parse_pattern_tuple(p, tb);

    /* 函数类型 */
    if (check_keyword(p, "func") || check_keyword(p, "cfunc"))
        return parse_pattern_func(p, tb);

    /* const / volatile 限定 */
    if (check_keyword(p, "const") || check_keyword(p, "volatile")) {
        bool is_vol = check_keyword(p, "volatile");
        advance(p);
        skip_trivia(p);
        ast_node_t *sub = parse_pattern_atom(p);
        if (!sub || sub->kind == AST_ERROR) {
            if (!sub)
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected pattern after qualifier");
            return sub;
        }
        ast_node_t *n = ast_pat_qual_new(p->arena, tb, p->pos);
        if (!n) return NULL;
        ((ast_pat_qual_t *)n)->is_volatile = is_vol;
        ((ast_pat_qual_t *)n)->sub = sub;
        return n;
    }

    /* 具名类型 / 泛型实例 */
    if (is_pattern_name_token(cur_token(p))) {
        strslice_t qualifier = STRSLICE_EMPTY;
        strslice_t name = token_strslice(cur_token(p));
        advance(p);
        skip_trivia(p);
        /* 限定名约束：`T extends mod::IW[T]`。 */
        if (check_symbol(p, "::")) {
            advance(p);
            skip_trivia(p);
            if (!is_pattern_name_token(cur_token(p))) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected interface name after '::'");
            }
            qualifier = name;
            name = token_strslice(cur_token(p));
            advance(p);
            skip_trivia(p);
        }
        ast_node_t *args = NULL, *args_last = NULL;
        if (check_symbol(p, "[")) {
            advance(p);
            skip_trivia(p);
            if (!check_symbol(p, "]")) {
                for (;;) {
                    ast_node_t *a = parse_pattern_or(p);
                    if (!a || a->kind == AST_ERROR) {
                        if (!a)
                            return ast_error_new(p->diag, p->tokens, p->arena,
                                                 tb, p->pos,
                                                 "expected type argument pattern");
                        return a;
                    }
                    a->next = NULL;
                    if (!args) args = a;
                    else args_last->next = a;
                    args_last = a;
                    skip_trivia(p);
                    if (!check_symbol(p, ",")) break;
                    advance(p);
                    skip_trivia(p);
                }
            }
            if (!expect_symbol(p, "]")) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected ']' after type argument patterns");
            }
            skip_trivia(p);
        }
        ast_node_t *n = ast_pat_name_new(p->arena, tb, p->pos);
        if (!n) return NULL;
        ((ast_pat_name_t *)n)->name = name;
        ((ast_pat_name_t *)n)->qualifier = qualifier;
        ((ast_pat_name_t *)n)->args = args;
        ((ast_pat_name_t *)n)->args_last = args_last;
        return n;
    }

    return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                         "expected type pattern after 'extends'");
}

static ast_node_t *parse_pattern_and(parser_t *p) {
    ast_node_t *lhs = parse_pattern_atom(p);
    if (!lhs || lhs->kind == AST_ERROR) return lhs;
    for (;;) {
        skip_trivia(p);
        if (!check_symbol(p, "&")) break;
        uint32_t tb = lhs->tok_begin;
        advance(p);
        skip_trivia(p);
        ast_node_t *rhs = parse_pattern_atom(p);
        if (!rhs || rhs->kind == AST_ERROR) {
            if (!rhs)
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected pattern after '&'");
            return rhs;
        }
        ast_node_t *n = ast_pat_logic_new(p->arena, AST_PAT_AND, tb, p->pos);
        if (!n) return NULL;
        ((ast_pat_logic_t *)n)->lhs = lhs;
        ((ast_pat_logic_t *)n)->rhs = rhs;
        lhs = n;
    }
    return lhs;
}

static ast_node_t *parse_pattern_or(parser_t *p) {
    ast_node_t *lhs = parse_pattern_and(p);
    if (!lhs || lhs->kind == AST_ERROR) return lhs;
    for (;;) {
        skip_trivia(p);
        if (!check_symbol(p, "|")) break;
        uint32_t tb = lhs->tok_begin;
        advance(p);
        skip_trivia(p);
        ast_node_t *rhs = parse_pattern_and(p);
        if (!rhs || rhs->kind == AST_ERROR) {
            if (!rhs)
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected pattern after '|'");
            return rhs;
        }
        ast_node_t *n = ast_pat_logic_new(p->arena, AST_PAT_OR, tb, p->pos);
        if (!n) return NULL;
        ((ast_pat_logic_t *)n)->lhs = lhs;
        ((ast_pat_logic_t *)n)->rhs = rhs;
        lhs = n;
    }
    return lhs;
}

ast_node_t *parse_pattern(parser_t *p) {
    if (!p) return NULL;
    return parse_pattern_or(p);
}
