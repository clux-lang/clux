#include "parser/lexer.h"
#include "fmt_internal.h"

#include "parser/ast_int_lit.h"
#include "parser/ast_pattern.h"

#include <stdio.h>

/* `extends` PATTERN 渲染（只用于函数/类型声明）。逻辑节点一律加括号：
 * 输出比输入更保守，但结构等价且幂等。 */

static bool pat_needs_parens(ast_node_t *n) {
    return n && (n->kind == AST_PAT_OR || n->kind == AST_PAT_AND);
}

static void render_pat_atom(fmt_t *f, ast_node_t *n);

static void render_pat_list(fmt_t *f, ast_node_t *head) {
    bool first = true;
    for (ast_node_t *p = head; p; p = p->next) {
        if (!first) sb_str(&f->sb, ", ");
        render_pat_atom(f, p);
        first = false;
    }
}

static const char *pat_own_prefix(ast_pat_own_t own) {
    switch (own) {
    case PAT_OWN_ANY:   return "?";
    case PAT_OWN_OWN:   return "own";
    case PAT_OWN_REF:   return "ref";
    case PAT_OWN_FATAL: return "fatal";
    case PAT_OWN_SHARE: return "share";
    case PAT_OWN_WEAK:  return "weak";
    default:            return NULL;
    }
}

static void render_pat_atom(fmt_t *f, ast_node_t *n) {
    if (!n) return;
    switch (n->kind) {
    case AST_PAT_WILD:
        sb_ch(&f->sb, '?');
        break;
    case AST_PAT_OR:
    case AST_PAT_AND: {
        ast_pat_logic_t *l = (ast_pat_logic_t *)n;
        sb_ch(&f->sb, '(');
        render_pat_atom(f, l->lhs);
        sb_str(&f->sb, n->kind == AST_PAT_OR ? " | " : " & ");
        render_pat_atom(f, l->rhs);
        sb_ch(&f->sb, ')');
        break;
    }
    case AST_PAT_PTR: {
        ast_pat_ptr_t *p = (ast_pat_ptr_t *)n;
        const char *pre = pat_own_prefix(p->own);
        if (p->own == PAT_OWN_ANY) {
            sb_str(&f->sb, "?*"); /* 规范写法 `?*?`（无空格） */
        } else if (pre) {
            sb_str(&f->sb, pre);
            sb_str(&f->sb, " *");
        } else {
            sb_ch(&f->sb, '*');
        }
        if (pat_needs_parens(p->base_type)) {
            sb_ch(&f->sb, '(');
            render_pat_atom(f, p->base_type);
            sb_ch(&f->sb, ')');
        } else {
            render_pat_atom(f, p->base_type);
        }
        break;
    }
    case AST_PAT_SLICE: {
        ast_pat_slice_t *s = (ast_pat_slice_t *)n;
        const char *pre = pat_own_prefix(s->own);
        if (pre) {
            sb_str(&f->sb, pre);
            sb_ch(&f->sb, ' ');
        }
        sb_str(&f->sb, "[]");
        if (pat_needs_parens(s->elem_type)) {
            sb_ch(&f->sb, '(');
            render_pat_atom(f, s->elem_type);
            sb_ch(&f->sb, ')');
        } else {
            render_pat_atom(f, s->elem_type);
        }
        break;
    }
    case AST_PAT_ARRAY: {
        ast_pat_array_t *a = (ast_pat_array_t *)n;
        sb_ch(&f->sb, '[');
        if (a->length) {
            char buf[32];
            snprintf(buf, sizeof buf, "%llu",
                     (unsigned long long)((ast_int_lit_t *)a->length)->value);
            sb_str(&f->sb, buf);
        } else {
            sb_ch(&f->sb, '?');
        }
        sb_ch(&f->sb, ']');
        if (pat_needs_parens(a->elem_type)) {
            sb_ch(&f->sb, '(');
            render_pat_atom(f, a->elem_type);
            sb_ch(&f->sb, ')');
        } else {
            render_pat_atom(f, a->elem_type);
        }
        break;
    }
    case AST_PAT_TUPLE: {
        ast_pat_tuple_t *t = (ast_pat_tuple_t *)n;
        sb_ch(&f->sb, '<');
        if (t->any_arity) {
            sb_ch(&f->sb, '?');
        } else {
            render_pat_list(f, t->elem_types);
        }
        sb_ch(&f->sb, '>');
        break;
    }
    case AST_PAT_NAME: {
        ast_pat_name_t *nm = (ast_pat_name_t *)n;
        if (nm->qualifier.ptr && nm->qualifier.len) {
            sb_put(&f->sb, nm->qualifier.ptr, nm->qualifier.len);
            sb_str(&f->sb, "::");
        }
        sb_put(&f->sb, nm->name.ptr, nm->name.len);
        if (nm->args) {
            sb_ch(&f->sb, '[');
            render_pat_list(f, nm->args);
            sb_ch(&f->sb, ']');
        }
        break;
    }
    case AST_PAT_FUNC: {
        ast_pat_func_t *fn = (ast_pat_func_t *)n;
        sb_str(&f->sb, "func(");
        render_pat_list(f, fn->params);
        sb_str(&f->sb, ")->");
        render_pat_atom(f, fn->return_pat);
        break;
    }
    case AST_PAT_QUAL: {
        ast_pat_qual_t *q = (ast_pat_qual_t *)n;
        sb_str(&f->sb, q->is_volatile ? "volatile " : "const ");
        render_pat_atom(f, q->sub);
        break;
    }
    default:
        render_expr(f, n);
        break;
    }
}

void render_pattern(fmt_t *f, ast_node_t *pat) {
    if (!f || !pat) return;
    render_pat_atom(f, pat);
}
