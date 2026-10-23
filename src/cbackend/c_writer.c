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
        wf(sb, "%s %s ", s->name ? s->name : "clux_v_tmp",
           s->op ? s->op : "=");
        write_expr(sb, s->expr);
        w(sb, ";\n");
        break;
    case CIR_STMT_EXPR:
        write_expr(sb, s->expr);
        w(sb, ";\n");
        break;
    case CIR_STMT_RETURN:
        if (s->expr) {
            w(sb, "return ");
            write_expr(sb, s->expr);
            w(sb, ";\n");
        } else {
            w(sb, "return;\n");
        }
        break;
    case CIR_STMT_IF:
        w(sb, "if (");
        write_expr(sb, s->expr);
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
        write_expr(sb, s->expr);
        w(sb, ") {\n");
        write_stmt_list(sb, s->body, indent + 1);
        write_indent(sb, indent);
        w(sb, "}\n");
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
    write_stmt_list(sb, fn->body, 1);
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

int cir_write_file(const cir_unit_t *unit, const char *path,
                   allocator_t *alloc) {
    if (!unit || !path || !alloc) return 1;
    string_t *sb = string_new(alloc);
    if (!sb) return 1;

    w(sb, "/* generated by clux C backend */\n");
    for (size_t i = 0; i < unit->include_count; i++)
        wf(sb, "#include <%s>\n", unit->includes[i]);
    w(sb, "\n");

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
