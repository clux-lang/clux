#include "cbackend/c_writer_internal.h"

void cw_write_expr(string_t *sb, const cir_expr_t *e) {
    if (!e) {
        cw_put(sb, "0");
        return;
    }
    switch (e->kind) {
    case CIR_EXPR_INT:
        cw_printf(sb, "%lld", (long long)e->int_value);
        break;
    case CIR_EXPR_FLOAT:
        cw_printf(sb, "(double)%.17g", e->float_value);
        break;
    case CIR_EXPR_BOOL:
        cw_put(sb, e->bool_value ? "true" : "false");
        break;
    case CIR_EXPR_IDENT:
        cw_put(sb, e->text ? e->text : "0");
        break;
    case CIR_EXPR_UNARY:
        cw_put(sb, "(");
        cw_put(sb, e->op ? e->op : "");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ")");
        break;
    case CIR_EXPR_BINARY:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " ");
        cw_put(sb, e->op ? e->op : "?");
        cw_put(sb, " ");
        cw_write_expr(sb, e->rhs);
        cw_put(sb, ")");
        break;
    case CIR_EXPR_CALL: {
        cw_put(sb, e->text ? e->text : "<null>");
        cw_put(sb, "(");
        bool first = true;
        for (const cir_expr_t *a = e->args; a; a = a->next) {
            if (!first) cw_put(sb, ", ");
            cw_write_expr(sb, a);
            first = false;
        }
        cw_put(sb, ")");
        break;
    }
    case CIR_EXPR_ASSIGN:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " ");
        cw_put(sb, e->op ? e->op : "=");
        cw_put(sb, " ");
        cw_write_expr(sb, e->rhs);
        cw_put(sb, ")");
        break;
    case CIR_EXPR_MEMBER:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").");
        cw_put(sb, e->text ? e->text : "clux_f_field");
        break;
    case CIR_EXPR_INDEX:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        if (e->array_data) cw_put(sb, ").data[");
        else cw_put(sb, ")[");
        cw_write_expr(sb, e->rhs);
        cw_put(sb, "]");
        break;
    case CIR_EXPR_CONSTRUCT:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){");
        if (e->type && e->type->kind == CIR_TYPE_ARRAY) cw_put(sb, ".data = {");
        {
            bool first = true;
            for (const cir_init_t *i = e->inits; i; i = i->next) {
                if (!first) cw_put(sb, ", ");
                if (i->name) {
                    cw_put(sb, ".");
                    cw_put(sb, i->name);
                    cw_put(sb, " = ");
                }
                cw_write_expr(sb, i->value);
                first = false;
            }
        }
        if (e->type && e->type->kind == CIR_TYPE_ARRAY) cw_put(sb, "}");
        cw_put(sb, "}");
        break;
    case CIR_EXPR_LEN:
        if (e->type && (e->type->kind == CIR_TYPE_SLICE ||
                        e->type->kind == CIR_TYPE_STR)) {
            cw_put(sb, "(");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").len");
        } else {
            cw_put(sb, "(sizeof((");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").data) / sizeof((");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").data[0]))");
        }
        break;
    case CIR_EXPR_SLICE: {
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){ .data = (");
        cw_put(sb, e->type && e->type->elem && e->type->elem->c_name
                   ? e->type->elem->c_name
                   : "int32_t");
        cw_put(sb, " *)");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ".data + (");
        if (e->rhs) cw_write_expr(sb, e->rhs);
        else cw_put(sb, "0");
        cw_put(sb, "), .len = (");
        if (e->high) cw_write_expr(sb, e->high);
        else if (e->src_type && (e->src_type->kind == CIR_TYPE_SLICE ||
                                 e->src_type->kind == CIR_TYPE_STR)) {
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").len");
        } else {
            cw_put(sb, "sizeof((");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").data) / sizeof((");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, ").data[0]))");
        }
        cw_put(sb, " - (");
        if (e->rhs) cw_write_expr(sb, e->rhs);
        else cw_put(sb, "0");
        cw_put(sb, ")) }");
        break;
    }
    case CIR_EXPR_MAKE: {
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){ .data = (");
        cw_put(sb, e->elem && e->elem->c_name ? e->elem->c_name : "int32_t");
        cw_put(sb, " *)clux_make(sizeof(");
        cw_put(sb, e->elem && e->elem->c_name ? e->elem->c_name : "int32_t");
        cw_put(sb, "), ");
        cw_write_expr(sb, e->lhs);
        if (e->inits) {
            cw_put(sb, ", (");
            cw_put(sb, e->elem && e->elem->c_name ? e->elem->c_name : "int32_t");
            cw_put(sb, "[]){");
            bool first = true;
            for (const cir_init_t *i = e->inits; i; i = i->next) {
                if (!first) cw_put(sb, ", ");
                cw_write_expr(sb, i->value);
                first = false;
            }
            cw_put(sb, "}, ");
            size_t cnt = 0;
            for (const cir_init_t *i = e->inits; i; i = i->next) cnt++;
            cw_printf(sb, "%zu", cnt);
        } else {
            cw_put(sb, ", NULL, 0");
        }
        cw_put(sb, "), .len = ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " }");
        break;
    }
    case CIR_EXPR_SLICE_DATA:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").data");
        break;
    case CIR_EXPR_STRING:
        cw_put(sb, "(struct clux_str){ .data = (const char *)\"");
        cw_write_c_bytes(sb, e->text, e->text_len);
        cw_put(sb, "\", .len = ");
        cw_printf(sb, "%zu", e->text_len);
        cw_put(sb, " }");
        break;
    case CIR_EXPR_OPTION:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        if (e->bool_value) {
            cw_put(sb, "){ .ok = true, .value = ");
            cw_write_expr(sb, e->lhs);
            cw_put(sb, " }");
        } else {
            cw_put(sb, "){ .ok = false }");
        }
        break;
    case CIR_EXPR_OPT_IS_SOME:
        if (!e->bool_value) cw_put(sb, "!(");
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").ok");
        if (!e->bool_value) cw_put(sb, ")");
        break;
    case CIR_EXPR_UNWRAP:
        cw_put(sb, "((");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").ok ? (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").value : (clux_panic_none(), (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").value))");
        break;
    case CIR_EXPR_UNION_CONSTRUCT:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_printf(sb, "){ .tag = %zu, .u.", e->tag);
        cw_put(sb, e->text ? e->text : "clux_m_member");
        cw_put(sb, " = ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " }");
        break;
    case CIR_EXPR_UNION_AT: {
        const char *ut =
            e->src_type ? e->src_type->c_name : "int32_t";
        const char *mt = e->type ? e->type->c_name : "int32_t";
        if (e->array_data) {
            /* 左值形态：直接取原对象成员的地址（tag 检查后返回 payload
               指针），`*(T*)` 是可写左值；接收者是左值，& 合法。 */
            cw_put(sb, "(*((");
            cw_put(sb, mt);
            cw_put(sb, " *)clux_union_at_lvalue(&(");
            cw_write_expr(sb, e->lhs);
            cw_printf(sb,
                      "), %zu, sizeof(((%s *)0)->tag), clux_offsetof(%s, u), "
                      "\"%s\")))",
                      e->tag, ut, ut, e->text ? e->text : "clux_m_member");
            break;
        }
        cw_put(sb, "(*(");
        cw_put(sb, mt);
        cw_put(sb, " *)clux_union_at(&((");
        cw_put(sb, ut);
        cw_put(sb, "[]){ ");
        cw_write_expr(sb, e->lhs);
        cw_printf(sb,
                  " })[0], %zu, sizeof(((%s *)0)->tag), clux_offsetof(%s, u), "
                  "\"%s\"))",
                  e->tag, ut, ut, e->text ? e->text : "clux_m_member");
        break;
    }
    case CIR_EXPR_SLICE_AT: {
        const char *st = e->type ? e->type->c_name : "int32_t";
        const char *et = (e->type && e->type->elem) ? e->type->elem->c_name
                                                     : "int32_t";
        cw_put(sb, "(*(");
        cw_put(sb, et);
        cw_put(sb, " *)clux_slice_at(&((");
        cw_put(sb, st);
        cw_put(sb, "[]){ ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " })[0], ");
        cw_write_expr(sb, e->rhs);
        cw_printf(sb, ", sizeof(%s), \"slice\"))", et);
        break;
    }
    case CIR_EXPR_UNION_MEMBER:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").u.");
        cw_put(sb, e->text ? e->text : "clux_m_member");
        break;
    case CIR_EXPR_UNION_TAG:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").tag");
        break;
    case CIR_EXPR_CUNION_CONSTRUCT:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){ .u.");
        cw_put(sb, e->text ? e->text : "clux_m_member");
        cw_put(sb, " = ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " }");
        break;
    case CIR_EXPR_FUNC_OBJ:
        cw_put(sb, e->text ? e->text : "<null>");
        break;
    case CIR_EXPR_FUNC_CALL:
        cw_put(sb, "(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").fn((");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env");
        for (const cir_expr_t *a = e->args; a; a = a->next) {
            cw_put(sb, ", ");
            cw_write_expr(sb, a);
        }
        cw_put(sb, ")");
        break;
    case CIR_EXPR_CLOSURE:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){ .fn = ");
        cw_put(sb, e->text ? e->text : "<null>");
        cw_put(sb, ", .env = ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ", .env_clone = ");
        cw_put(sb, e->text2 ? e->text2 : "NULL");
        cw_put(sb, ", .env_dispose = ");
        if (e->op) {
            cw_put(sb, "(void (*)(void *, void *))");
            cw_put(sb, e->op);
        } else {
            cw_put(sb, "NULL");
        }
        cw_put(sb, " }");
        break;
    case CIR_EXPR_FUNC_CLONE:
        cw_put(sb, "((");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env ? (");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "){ .fn = (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").fn, .env = (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env_clone((");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env), .env_clone = (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env_clone, .env_dispose = (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ").env_dispose } : (");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, "))");
        break;
    case CIR_EXPR_CLOSURE_RELEASE:
        cw_put(sb, "clux_func_release(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ")");
        break;
    case CIR_EXPR_NEW:
        cw_put(sb, "(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, " *)clux_new(sizeof(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "), ");
        if (e->inits) {
            cw_put(sb, "&(");
            cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
            cw_put(sb, "){");
            if (e->type && e->type->kind == CIR_TYPE_ARRAY) cw_put(sb, ".data = {");
            bool first = true;
            for (const cir_init_t *i = e->inits; i; i = i->next) {
                if (!first) cw_put(sb, ", ");
                if (i->name) {
                    cw_put(sb, ".");
                    cw_put(sb, i->name);
                    cw_put(sb, " = ");
                }
                cw_write_expr(sb, i->value);
                first = false;
            }
            if (e->type && e->type->kind == CIR_TYPE_ARRAY) cw_put(sb, "}");
            cw_put(sb, "}");
        } else {
            cw_put(sb, "NULL");
        }
        cw_put(sb, ")");
        break;
    case CIR_EXPR_BITFIELD_GET:
        cw_put(sb, "((");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "uint64_t");
        cw_put(sb, ")clux_bf_get((");
        cw_write_expr(sb, e->lhs);
        cw_printf(sb, ").data, %zu, %zu, %zu))", e->bit_word_bits,
                  e->bit_offset, e->bit_width);
        break;
    case CIR_EXPR_BITFIELD_SET:
        cw_put(sb, "clux_bf_set((");
        cw_write_expr(sb, e->lhs);
        cw_printf(sb, ").data, %zu, %zu, %zu, (uint64_t)(", e->bit_word_bits,
                  e->bit_offset, e->bit_width);
        cw_write_expr(sb, e->rhs);
        cw_put(sb, "))");
        break;
    case CIR_EXPR_BITFIELD_MAKE: {
        size_t n = 0;
        for (const cir_init_t *i = e->inits; i; i = i->next) n++;
        if (n == 0) {
            cw_put(sb, "((");
            cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
            cw_put(sb, "){0})");
            break;
        }
        cw_put(sb, "clux_bf_make_");
        cw_put(sb, cw_bf_helper_suffix(e->type));
        cw_put(sb, "((const struct clux_bf_item[]){");
        bool first = true;
        for (const cir_init_t *i = e->inits; i; i = i->next) {
            if (!first) cw_put(sb, ", ");
            cw_printf(sb, "{ %zu, %zu, (uint64_t)(", i->bit_offset,
                      i->bit_width);
            cw_write_expr(sb, i->value);
            cw_put(sb, ") }");
            first = false;
        }
        cw_printf(sb, "}, %zu)", n);
        break;
    }
    case CIR_EXPR_CAST:
        cw_put(sb, "((");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, ")(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, "))");
        break;
    case CIR_EXPR_TERNARY:
        cw_put(sb, "((");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, ") ? (");
        cw_write_expr(sb, e->rhs);
        cw_put(sb, ") : (");
        cw_write_expr(sb, e->high);
        cw_put(sb, "))");
        break;
    case CIR_EXPR_MEM_EQ:
        cw_put(sb, "(memcmp(&(");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, "), &(");
        cw_write_expr(sb, e->rhs);
        cw_put(sb, "), sizeof(");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, ")) == 0)");
        break;
    case CIR_EXPR_TEMP_LVALUE:
        /* C99 复合字面量数组：`((T[]){ v })[0]` 是左值，可安全取地址；
           每次求值都是一个新的临时对象（与 VM 对右值接收者的处理一致）。 */
        cw_put(sb, "(((");
        cw_put(sb, e->type && e->type->c_name ? e->type->c_name : "int32_t");
        cw_put(sb, "[]){ ");
        cw_write_expr(sb, e->lhs);
        cw_put(sb, " })[0])");
        break;
    default:
        cw_put(sb, "0");
        break;
    }
}
