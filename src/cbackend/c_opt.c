#include "cbackend/c_opt.h"

#include <string.h>

static void scan_expr(cir_unit_t *unit, const cir_expr_t *e);

static void mark_object(cir_unit_t *unit, const char *object) {
    if (!object) return;
    for (cir_function_t *f = unit->functions; f; f = f->next)
        if (f->object && strcmp(f->object, object) == 0) {
            f->object_used = true;
            return;
        }
}

static void scan_expr(cir_unit_t *unit, const cir_expr_t *e) {
    for (const cir_expr_t *it = e; it; it = it->next) {
        if (it->kind == CIR_EXPR_FUNC_OBJ) mark_object(unit, it->text);
        scan_expr(unit, it->lhs);
        scan_expr(unit, it->rhs);
        scan_expr(unit, it->high);
        scan_expr(unit, it->args);
        for (const cir_init_t *in = it->inits; in; in = in->next)
            scan_expr(unit, in->value);
    }
}

static void scan_stmt(cir_unit_t *unit, const cir_stmt_t *s) {
    for (const cir_stmt_t *it = s; it; it = it->next) {
        scan_expr(unit, it->expr);
        scan_expr(unit, it->lhs);
        scan_expr(unit, it->update);
        scan_stmt(unit, it->init);
        scan_stmt(unit, it->body);
        scan_stmt(unit, it->else_body);
    }
}

void cbackend_optimize(cir_unit_t *unit) {
    if (!unit) return;
    for (cir_function_t *f = unit->functions; f; f = f->next)
        f->object_used = false;
    for (cir_function_t *f = unit->functions; f; f = f->next) {
        scan_stmt(unit, f->body);
        scan_stmt(unit, f->cleanup);
    }
    for (cir_global_t *g = unit->globals; g; g = g->next)
        scan_expr(unit, g->init);
}
