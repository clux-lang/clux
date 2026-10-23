#include "cbackend/c_writer.h"
#include "core/string.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void w(string_t *sb, const char *s) { string_append_cstr(sb, s); }

static void wf(string_t *sb, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    string_append_cstr(sb, buf);
}

static void write_expr(string_t *sb, const cir_expr_t *e) {
    if (!e) {
        w(sb, "0");
        return;
    }
    switch (e->kind) {
    case CIR_EXPR_INT:
        wf(sb, "%lld", (long long)e->int_value);
        break;
    case CIR_EXPR_BOOL:
        w(sb, e->bool_value ? "true" : "false");
        break;
    case CIR_EXPR_IDENT:
        w(sb, e->text ? e->text : "0");
        break;
    case CIR_EXPR_UNARY:
        w(sb, "(");
        w(sb, e->op ? e->op : "");
        write_expr(sb, e->lhs);
        w(sb, ")");
        break;
    case CIR_EXPR_BINARY:
        w(sb, "(");
        write_expr(sb, e->lhs);
        w(sb, " ");
        w(sb, e->op ? e->op : "?");
        w(sb, " ");
        write_expr(sb, e->rhs);
        w(sb, ")");
        break;
    case CIR_EXPR_CALL: {
        w(sb, e->text ? e->text : "<null>");
        w(sb, "(");
        bool first = true;
        for (const cir_expr_t *a = e->args; a; a = a->next) {
            if (!first) w(sb, ", ");
            write_expr(sb, a);
            first = false;
        }
        w(sb, ")");
        break;
    }
    case CIR_EXPR_ASSIGN:
        w(sb, "(");
        write_expr(sb, e->lhs);
        w(sb, " ");
        w(sb, e->op ? e->op : "=");
        w(sb, " ");
        write_expr(sb, e->rhs);
        w(sb, ")");
        break;
    case CIR_EXPR_MEMBER:
        w(sb, "(");
        write_expr(sb, e->lhs);
        w(sb, ").");
        w(sb, e->text ? e->text : "clux_f_field");
        break;
    case CIR_EXPR_INDEX:
        w(sb, "(");
        write_expr(sb, e->lhs);
        if (e->array_data) w(sb, ").data[");
        else w(sb, ")[");
        write_expr(sb, e->rhs);
        w(sb, "]");
        break;
    case CIR_EXPR_CONSTRUCT:
        w(sb, "(");
        w(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        w(sb, "){");
        if (e->type && e->type->kind == CIR_TYPE_ARRAY) w(sb, ".data = {");
        {
            bool first = true;
            for (const cir_init_t *i = e->inits; i; i = i->next) {
                if (!first) w(sb, ", ");
                if (i->name) {
                    w(sb, ".");
                    w(sb, i->name);
                    w(sb, " = ");
                }
                write_expr(sb, i->value);
                first = false;
            }
        }
        if (e->type && e->type->kind == CIR_TYPE_ARRAY) w(sb, "}");
        w(sb, "}");
        break;
    case CIR_EXPR_NEW:
        w(sb, "(");
        w(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        w(sb, " *)clux_new(sizeof(");
        w(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        w(sb, "), ");
        if (e->inits) {
            w(sb, "&(");
            w(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
            w(sb, "){");
            if (e->type && e->type->kind == CIR_TYPE_ARRAY) w(sb, ".data = {");
            bool first = true;
            for (const cir_init_t *i = e->inits; i; i = i->next) {
                if (!first) w(sb, ", ");
                if (i->name) {
                    w(sb, ".");
                    w(sb, i->name);
                    w(sb, " = ");
                }
                write_expr(sb, i->value);
                first = false;
            }
            if (e->type && e->type->kind == CIR_TYPE_ARRAY) w(sb, "}");
            w(sb, "}");
        } else {
            w(sb, "NULL");
        }
        w(sb, ")");
        break;
    default:
        w(sb, "0");
        break;
    }
}

static void write_params(string_t *sb, const cir_signature_t *sig,
                         bool with_names, const cir_function_t *fn) {
    for (size_t i = 0; i < sig->param_count; i++) {
        w(sb, ", ");
        w(sb, sig->params[i] ? sig->params[i]->c_name : "void");
        if (with_names && fn && i < fn->param_count && fn->params[i].name) {
            w(sb, " ");
            w(sb, fn->params[i].name);
        }
    }
}

static void write_signature_def(string_t *sb, const cir_signature_t *sig) {
    wf(sb, "struct %s {\n", sig->tag);
    wf(sb, "    %s (*fn)(void *env", sig->ret ? sig->ret->c_name : "void");
    for (size_t i = 0; i < sig->param_count; i++) {
        w(sb, ", ");
        w(sb, sig->params[i] ? sig->params[i]->c_name : "void");
    }
    w(sb, ");\n");
    w(sb, "    void *env;\n");
    w(sb, "};\n\n");
}

static void write_function_decl(string_t *sb, const cir_function_t *fn) {
    wf(sb, "static %s %s(void *env", fn->sig->ret ? fn->sig->ret->c_name : "void",
       fn->name);
    write_params(sb, fn->sig, true, fn);
    w(sb, ");\n");
}

static void write_type_defs(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next)
        if (t->kind == CIR_TYPE_STRUCT || t->kind == CIR_TYPE_ARRAY)
            wf(sb, "struct %s;\n", t->c_name + 7);
    w(sb, "\n");
}

static void emit_type(string_t *sb, cir_type_t *t) {
    if (!t || t->emitted) return;
    switch (t->kind) {
    case CIR_TYPE_STRUCT:
        t->emitted = true;
        for (cir_field_t *f = t->fields; f; f = f->next) {
            if (f->type &&
                (f->type->kind == CIR_TYPE_STRUCT ||
                 f->type->kind == CIR_TYPE_ARRAY))
                emit_type(sb, f->type);
        }
        wf(sb, "%s {\n", t->c_name);
        for (cir_field_t *f = t->fields; f; f = f->next) {
            wf(sb, "    %s %s;\n",
               f->type && f->type->c_name ? f->type->c_name : "int32_t",
               f->name ? f->name : "field");
        }
        w(sb, "};\n\n");
        break;
    case CIR_TYPE_ARRAY:
        if (t->elem) emit_type(sb, t->elem);
        wf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_array");
        wf(sb, "    %s data[%zu];\n",
           t->elem && t->elem->c_name ? t->elem->c_name : "int32_t",
           t->array_len);
        w(sb, "};\n\n");
        t->emitted = true;
        break;
    default:
        t->emitted = true;
        break;
    }
}

static void emit_all_types(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next) emit_type(sb, t);
}

static void write_function_object(string_t *sb, const cir_function_t *fn) {
    if (!fn->object || !fn->sig) return;
    wf(sb, "static const struct %s %s = { %s, NULL };\n", fn->sig->tag,
       fn->object, fn->name);
}

static void write_c_string(string_t *sb, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\' || *p == '"') string_append_char(sb, '\\');
        string_append_char(sb, *p);
    }
}

static void write_indent(string_t *sb, int indent) {
    for (int i = 0; i < indent; i++) w(sb, "    ");
}

static void write_cond(string_t *sb, const cir_expr_t *e) {
    if (e && e->kind == CIR_EXPR_BINARY) {
        write_expr(sb, e->lhs);
        w(sb, " ");
        w(sb, e->op ? e->op : "?");
        w(sb, " ");
        write_expr(sb, e->rhs);
    } else {
        write_expr(sb, e);
    }
}

static void write_stmt_list(string_t *sb, const cir_stmt_t *s, int indent);

static void write_stmt(string_t *sb, const cir_stmt_t *s, int indent) {
    if (!s) return;
    write_indent(sb, indent);
    switch (s->kind) {
    case CIR_STMT_VAR:
        wf(sb, "%s %s", s->type && s->type->c_name ? s->type->c_name : "int32_t",
           s->name ? s->name : "clux_v_tmp");
        if (s->expr) {
            w(sb, " = ");
            write_expr(sb, s->expr);
        }
        w(sb, ";\n");
        break;
    case CIR_STMT_ASSIGN:
        if (s->lhs) write_expr(sb, s->lhs);
        else wf(sb, "%s", s->name ? s->name : "clux_v_tmp");
        w(sb, " ");
        w(sb, s->op ? s->op : "=");
        w(sb, " ");
        write_expr(sb, s->expr);
        w(sb, ";\n");
        break;
    case CIR_STMT_EXPR:
        write_expr(sb, s->expr);
        w(sb, ";\n");
        break;
    case CIR_STMT_RETURN:
        if (s->cleanup_return) {
            if (s->expr) {
                w(sb, "clux_retval = ");
                write_expr(sb, s->expr);
                w(sb, ";\n");
            }
            w(sb, "goto clux_cleanup;\n");
        } else if (s->expr) {
            w(sb, "return ");
            write_expr(sb, s->expr);
            w(sb, ";\n");
        } else {
            w(sb, "return;\n");
        }
        break;
    case CIR_STMT_IF:
        w(sb, "if (");
        write_cond(sb, s->expr);
        w(sb, ") {\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "}");
        if (s->else_body) {
            w(sb, " else {\n");
            write_stmt_list(sb, s->else_body, indent + 1);
            write_indent(sb, indent);
            w(sb, "}");
        }
        w(sb, "\n");
        break;
    case CIR_STMT_WHILE:
        w(sb, "while (");
        write_cond(sb, s->expr);
        w(sb, ") {\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "}\n");
        break;
    case CIR_STMT_FOR:
        w(sb, "for (");
        if (s->init && s->init->kind == CIR_STMT_VAR) {
            wf(sb, "%s %s",
               s->init->type && s->init->type->c_name ? s->init->type->c_name
                                                       : "int32_t",
               s->init->name ? s->init->name : "clux_v_i");
            if (s->init->expr) {
                w(sb, " = ");
                write_expr(sb, s->init->expr);
            }
        } else if (s->init && s->init->kind == CIR_STMT_ASSIGN) {
            wf(sb, "%s %s ", s->init->name ? s->init->name : "clux_v_i",
               s->init->op ? s->init->op : "=");
            write_expr(sb, s->init->expr);
        }
        w(sb, "; ");
        if (s->expr) write_cond(sb, s->expr);
        w(sb, "; ");
        if (s->update) write_expr(sb, s->update);
        w(sb, ") {\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "}\n");
        break;
    case CIR_STMT_DOWHILE:
        w(sb, "do {\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "} while (");
        write_cond(sb, s->expr);
        w(sb, ");\n");
        break;
    case CIR_STMT_BLOCK:
        w(sb, "{\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "}\n");
        break;
    case CIR_STMT_BREAK:
        w(sb, "break;\n");
        break;
    case CIR_STMT_CONTINUE:
        w(sb, "continue;\n");
        break;
    case CIR_STMT_FREE:
        w(sb, "clux_free(");
        write_expr(sb, s->expr);
        w(sb, ");\n");
        break;
    default:
        break;
    }
}

static void write_stmt_list(string_t *sb, const cir_stmt_t *s, int indent) {
    for (const cir_stmt_t *it = s; it; it = it->next)
        write_stmt(sb, it, indent);
}

static void write_function_def(string_t *sb, const cir_function_t *fn) {
    wf(sb, "static %s %s(void *env", fn->sig->ret ? fn->sig->ret->c_name : "void",
       fn->name);
    write_params(sb, fn->sig, true, fn);
    w(sb, ") {\n");
    w(sb, "    (void)env;\n");
    bool needs_cleanup = fn->cleanup != NULL;
    if (needs_cleanup && fn->sig->ret &&
        fn->sig->ret->kind != CIR_TYPE_VOID)
        wf(sb, "    %s clux_retval;\n", fn->sig->ret->c_name);
    write_stmt_list(sb, fn->body, 1);
    if (needs_cleanup) {
        w(sb, "clux_cleanup:\n");
        for (const cir_stmt_t *b = fn->cleanup; b; b = b->next) {
            w(sb, "    {\n");
            write_stmt_list(sb, b->body, 2);
            w(sb, "    }\n");
        }
        if (fn->sig->ret && fn->sig->ret->kind != CIR_TYPE_VOID)
            w(sb, "    return clux_retval;\n");
        else
            w(sb, "    return;\n");
    }
    w(sb, "}\n\n");
}

static void reverse_functions(cir_function_t **head) {
    cir_function_t *prev = NULL;
    cir_function_t *cur = *head;
    while (cur) {
        cir_function_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

static void reverse_signatures(cir_signature_t **head) {
    cir_signature_t *prev = NULL;
    cir_signature_t *cur = *head;
    while (cur) {
        cir_signature_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

static void reverse_globals(cir_global_t **head) {
    cir_global_t *prev = NULL;
    cir_global_t *cur = *head;
    while (cur) {
        cir_global_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

int cir_write_file(const cir_unit_t *unit, const char *path,
                   allocator_t *alloc) {
    if (!unit || !path || !alloc) return 1;
    string_t *sb = string_new(alloc);
    if (!sb) return 1;

    w(sb, "/* generated by clux C backend */\n");
    for (size_t i = 0; i < unit->include_count; i++)
        wf(sb, "#include <%s>\n", unit->includes[i]);
    if (unit->uses_alloc) {
        w(sb, "#include <stdlib.h>\n");
        w(sb, "#include <string.h>\n");
        w(sb, "static void *clux_new(size_t n, const void *src) {\n");
        w(sb, "    void *p = calloc(1, n);\n");
        w(sb, "    if (!p) abort();\n");
        w(sb, "    if (src) memcpy(p, src, n);\n");
        w(sb, "    return p;\n");
        w(sb, "}\n");
        w(sb, "#define clux_free free\n");
    }
    w(sb, "\n");

    if (unit->types) {
        w(sb, "/* ---- composite type declarations ---- */\n");
        write_type_defs(sb, unit->types);
        w(sb, "/* ---- composite type definitions ---- */\n");
        emit_all_types(sb, unit->types);
    }

    cir_signature_t *sigs = unit->signatures;
    reverse_signatures(&sigs);
    if (sigs) {
        w(sb, "/* ---- composite type declarations ---- */\n");
        for (cir_signature_t *s = sigs; s; s = s->next)
            wf(sb, "struct %s;\n", s->tag);
        w(sb, "\n");

        w(sb, "/* ---- composite type definitions ---- */\n");
        for (cir_signature_t *s = sigs; s; s = s->next)
            write_signature_def(sb, s);
    }

    cir_function_t *fns = unit->functions;
    reverse_functions(&fns);
    if (fns) {
        w(sb, "/* ---- function declarations ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next)
            write_function_decl(sb, f);
        w(sb, "\n");

        w(sb, "/* ---- global variables and function objects ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next)
            write_function_object(sb, f);
        cir_global_t *globals = unit->globals;
        reverse_globals(&globals);
        for (cir_global_t *g = globals; g; g = g->next) {
            wf(sb, "static %s %s",
               g->type && g->type->c_name ? g->type->c_name : "int32_t",
               g->name ? g->name : "clux_v_global");
            if (g->init) {
                w(sb, " = ");
                write_expr(sb, g->init);
            }
            w(sb, ";\n");
        }
        w(sb, "\n");
    }

    if (fns) {
        w(sb, "/* ---- function implementations ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next) {
            if (f->source_line > 0) {
                wf(sb, "#line %d \"", f->source_line);
                write_c_string(sb, unit->source_path ? unit->source_path
                                                     : "<clux>");
                w(sb, "\"\n");
            }
            write_function_def(sb, f);
        }
    }

    for (cir_function_t *f = fns; f; f = f->next) {
        if (!f->is_entry) continue;
        w(sb, "int main(void) {\n");
        if (f->sig && f->sig->ret && f->sig->ret->kind != CIR_TYPE_VOID) {
            wf(sb, "    return (int)%s.fn(%s.env);\n", f->object, f->object);
        } else {
            wf(sb, "    %s.fn(%s.env);\n", f->object, f->object);
            w(sb, "    return 0;\n");
        }
        w(sb, "}\n");
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        string_free(&sb);
        return 1;
    }
    size_t n = string_len(sb);
    int ok = fwrite(string_cstr(sb), 1, n, fp) == n;
    fclose(fp);
    string_free(&sb);
    return ok ? 0 : 1;
}
