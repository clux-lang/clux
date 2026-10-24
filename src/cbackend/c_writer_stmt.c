#include "cbackend/c_writer_internal.h"

void cw_write_params(string_t *sb, const cir_signature_t *sig,
                         bool with_names, const cir_function_t *fn) {
    for (size_t i = 0; i < sig->param_count; i++) {
        cw_put(sb, ", ");
        cw_put(sb, sig->params[i] ? sig->params[i]->c_name : "void");
        if (with_names && fn && i < fn->param_count && fn->params[i].name) {
            cw_put(sb, " ");
            cw_put(sb, fn->params[i].name);
        }
    }
}

void cw_write_signature_def(string_t *sb, const cir_signature_t *sig) {
    cw_printf(sb, "struct %s {\n", sig->tag);
    cw_printf(sb, "    %s (*fn)(void *env", sig->ret ? sig->ret->c_name : "void");
    for (size_t i = 0; i < sig->param_count; i++) {
        cw_put(sb, ", ");
        cw_put(sb, sig->params[i] ? sig->params[i]->c_name : "void");
    }
    cw_put(sb, ");\n");
    cw_put(sb, "    void *env;\n");
    cw_put(sb, "    void *(*env_clone)(void *);\n");
    cw_put(sb, "    void (*env_dispose)(void *, void *);\n");
    cw_put(sb, "};\n\n");
}

void cw_write_function_decl(string_t *sb, const cir_function_t *fn) {
    /* 导出函数：非 static，头文件里是 `extern` 声明；其余文件内 `static`。 */
    cw_printf(sb, "%s%s %s(void *env",
              fn->is_exported ? "extern " : "static ",
              fn->sig->ret ? fn->sig->ret->c_name : "void", fn->name);
    cw_write_params(sb, fn->sig, true, fn);
    cw_put(sb, ");\n");
}

void cw_write_function_object(string_t *sb, const cir_function_t *fn) {
    if (!fn->object || !fn->sig || !fn->object_used) return;
    cw_printf(sb, "static const struct %s %s = { %s, NULL, NULL, NULL };\n",
              fn->sig->tag, fn->object, fn->name);
}

void cw_write_indent(string_t *sb, int indent) {
    for (int i = 0; i < indent; i++) cw_put(sb, "    ");
}

void cw_write_cond(string_t *sb, const cir_expr_t *e) {
    if (e && e->kind == CIR_EXPR_BINARY) {
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " ");
        cw_put(sb, e->op ? e->op : "?");
        cw_put(sb, " ");
        cw_write_expr(sb, e->rhs);
    } else {
        cw_write_expr(sb, e);
    }
}

void cw_write_stmt_list(string_t *sb, const cir_stmt_t *s, int indent) {
    for (const cir_stmt_t *it = s; it; it = it->next)
        cw_write_stmt(sb, it, indent);
}

void cw_write_stmt(string_t *sb, const cir_stmt_t *s, int indent) {
    if (!s) return;
    cw_write_indent(sb, indent);
    switch (s->kind) {
    case CIR_STMT_VAR:
        cw_printf(sb, "%s %s", s->type && s->type->c_name ? s->type->c_name : "int32_t",
           s->name ? s->name : "clux_v_tmp");
        if (s->expr) {
            cw_put(sb, " = ");
            cw_write_expr(sb, s->expr);
        }
        cw_put(sb, ";\n");
        break;
    case CIR_STMT_ASSIGN:
        if (s->lhs) cw_write_expr(sb, s->lhs);
        else cw_printf(sb, "%s", s->name ? s->name : "clux_v_tmp");
        cw_put(sb, " ");
        cw_put(sb, s->op ? s->op : "=");
        cw_put(sb, " ");
        cw_write_expr(sb, s->expr);
        cw_put(sb, ";\n");
        break;
    case CIR_STMT_EXPR:
        cw_write_expr(sb, s->expr);
        cw_put(sb, ";\n");
        break;
    case CIR_STMT_RETURN:
        if (s->cleanup_return) {
            if (s->expr) {
                cw_put(sb, "clux_retval = ");
                cw_write_expr(sb, s->expr);
                cw_put(sb, ";\n");
            }
            cw_put(sb, "goto clux_cleanup;\n");
        } else if (s->expr) {
            cw_put(sb, "return ");
            cw_write_expr(sb, s->expr);
            cw_put(sb, ";\n");
        } else {
            cw_put(sb, "return;\n");
        }
        break;
    case CIR_STMT_IF:
        cw_put(sb, "if (");
        cw_write_cond(sb, s->expr);
        cw_put(sb, ") {\n");
        cw_write_stmt_list(sb, s->body, indent + 1);
        cw_write_indent(sb, indent);
        cw_put(sb, "}");
        if (s->else_body) {
            cw_put(sb, " else {\n");
            cw_write_stmt_list(sb, s->else_body, indent + 1);
            cw_write_indent(sb, indent);
            cw_put(sb, "}");
        }
        cw_put(sb, "\n");
        break;
    case CIR_STMT_WHILE:
        cw_put(sb, "while (");
        cw_write_cond(sb, s->expr);
        cw_put(sb, ") {\n");
        cw_write_stmt_list(sb, s->body, indent + 1);
        cw_write_indent(sb, indent);
        cw_put(sb, "}\n");
        break;
    case CIR_STMT_FOR:
        cw_put(sb, "for (");
        if (s->init && s->init->kind == CIR_STMT_VAR) {
            cw_printf(sb, "%s %s",
               s->init->type && s->init->type->c_name ? s->init->type->c_name
                                                       : "int32_t",
               s->init->name ? s->init->name : "clux_v_i");
            if (s->init->expr) {
                cw_put(sb, " = ");
                cw_write_expr(sb, s->init->expr);
            }
        } else if (s->init && s->init->kind == CIR_STMT_ASSIGN) {
            cw_printf(sb, "%s %s ", s->init->name ? s->init->name : "clux_v_i",
               s->init->op ? s->init->op : "=");
            cw_write_expr(sb, s->init->expr);
        }
        cw_put(sb, "; ");
        if (s->expr) cw_write_cond(sb, s->expr);
        cw_put(sb, "; ");
        if (s->update) cw_write_expr(sb, s->update);
        cw_put(sb, ") {\n");
        cw_write_stmt_list(sb, s->body, indent + 1);
        cw_write_indent(sb, indent);
        cw_put(sb, "}\n");
        break;
    case CIR_STMT_DOWHILE:
        cw_put(sb, "do {\n");
        cw_write_stmt_list(sb, s->body, indent + 1);
        cw_write_indent(sb, indent);
        cw_put(sb, "} while (");
        cw_write_cond(sb, s->expr);
        cw_put(sb, ");\n");
        break;
    case CIR_STMT_BLOCK:
        cw_put(sb, "{\n");
        cw_write_stmt_list(sb, s->body, indent + 1);
        cw_write_indent(sb, indent);
        cw_put(sb, "}\n");
        break;
    case CIR_STMT_BREAK:
        cw_put(sb, "break;\n");
        break;
    case CIR_STMT_CONTINUE:
        cw_put(sb, "continue;\n");
        break;
    case CIR_STMT_FREE:
        if (s->expr && s->expr->kind == CIR_EXPR_CLOSURE_RELEASE) {
            cw_write_expr(sb, s->expr);
            cw_put(sb, ";\n");
        } else {
            cw_put(sb, "clux_free(");
            cw_write_expr(sb, s->expr);
            cw_put(sb, ");\n");
        }
        break;
    default:
        break;
    }
}

void cw_write_function_def(string_t *sb, const cir_function_t *fn) {
    cw_printf(sb, "%s%s %s(void *env",
              fn->is_exported ? "" : "static ",
              fn->sig->ret ? fn->sig->ret->c_name : "void", fn->name);
    cw_write_params(sb, fn->sig, true, fn);
    cw_put(sb, ") {\n");
    cw_put(sb, "    (void)env;\n");
    if (fn->closure_env && fn->closure_env->c_name)
        cw_printf(sb, "    %s clux_v___ctx_env = *(%s *)env;\n",
           fn->closure_env->c_name, fn->closure_env->c_name);
    bool needs_cleanup = fn->cleanup != NULL;
    if (needs_cleanup && fn->sig->ret &&
        fn->sig->ret->kind != CIR_TYPE_VOID)
        cw_printf(sb, "    %s clux_retval;\n", fn->sig->ret->c_name);
    cw_write_stmt_list(sb, fn->body, 1);
    if (needs_cleanup) {
        cw_put(sb, "clux_cleanup:\n");
        for (const cir_stmt_t *b = fn->cleanup; b; b = b->next) {
            cw_put(sb, "    {\n");
            cw_write_stmt_list(sb, b->body, 2);
            cw_put(sb, "    }\n");
        }
        if (fn->sig->ret && fn->sig->ret->kind != CIR_TYPE_VOID)
            cw_put(sb, "    return clux_retval;\n");
        else
            cw_put(sb, "    return;\n");
    }
    cw_put(sb, "}\n\n");
}
