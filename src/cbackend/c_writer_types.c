#include "cbackend/c_writer_internal.h"

/* 把类型标记为"进头文件"，并递归展开它按值/按指针依赖的类型：
 * struct/cunion/union 的字段、array/option/slice 的元素、ptr 的基类型、
 * func 签名的参数/返回。递归在指针处收敛（递归 struct 靠指针打断）。 */
void cw_mark_header_type(cir_type_t *t) {
    if (!t) return;
    switch (t->kind) {
    case CIR_TYPE_STRUCT:
    case CIR_TYPE_UNION:
    case CIR_TYPE_CUNION:
        if (t->in_header) return;
        t->in_header = true;
        for (cir_field_t *f = t->fields; f; f = f->next)
            cw_mark_header_type(f->type);
        return;
    case CIR_TYPE_ARRAY:
    case CIR_TYPE_SLICE:
    case CIR_TYPE_OPTION:
        if (t->in_header) return;
        t->in_header = true;
        cw_mark_header_type(t->elem);
        return;
    case CIR_TYPE_PTR:
        cw_mark_header_type(t->elem);
        return;
    case CIR_TYPE_FUNC:
        if (t->in_header) return;
        t->in_header = true;
        if (t->sig) {
            t->sig->in_header = true;
            cw_mark_header_type(t->sig->ret);
            for (size_t i = 0; i < t->sig->param_count; i++)
                cw_mark_header_type(t->sig->params[i]);
        }
        return;
    case CIR_TYPE_STR:
        t->in_header = true;
        return;
    default:
        return;
    }
}

void cw_write_type_defs(string_t *sb, cir_type_t *types, bool header_only) {
    for (cir_type_t *t = types; t; t = t->next) {
        if (header_only != t->in_header) continue;
        if (t->kind == CIR_TYPE_STRUCT || t->kind == CIR_TYPE_ARRAY ||
            t->kind == CIR_TYPE_SLICE || t->kind == CIR_TYPE_STR ||
            t->kind == CIR_TYPE_OPTION || t->kind == CIR_TYPE_UNION ||
            t->kind == CIR_TYPE_CUNION || t->kind == CIR_TYPE_FUNC)
            cw_printf(sb, "struct %s;\n", t->c_name + 7);
    }
    cw_put(sb, "\n");
}

void cw_emit_type(string_t *sb, cir_type_t *t) {
    if (!t || t->emitted) return;
    switch (t->kind) {
    case CIR_TYPE_STRUCT:
        t->emitted = true;
        for (cir_field_t *f = t->fields; f; f = f->next) {
            if (f->type &&
               (f->type->kind == CIR_TYPE_STRUCT ||
                 f->type->kind == CIR_TYPE_ARRAY ||
                 f->type->kind == CIR_TYPE_SLICE ||
                 f->type->kind == CIR_TYPE_OPTION ||
                 f->type->kind == CIR_TYPE_UNION ||
                 f->type->kind == CIR_TYPE_CUNION))
                cw_emit_type(sb, f->type);
        }
        cw_printf(sb, "%s {\n", t->c_name);
        for (cir_field_t *f = t->fields; f; f = f->next) {
            cw_printf(sb, "    %s %s;\n",
               f->type && f->type->c_name ? f->type->c_name : "int32_t",
               f->name ? f->name : "field");
        }
        cw_put(sb, "};\n\n");
        break;
    case CIR_TYPE_ARRAY:
        if (t->elem) cw_emit_type(sb, t->elem);
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_array");
        cw_printf(sb, "    %s data[%zu];\n",
           t->elem && t->elem->c_name ? t->elem->c_name : "int32_t",
           t->array_len);
        cw_put(sb, "};\n\n");
        t->emitted = true;
        break;
    case CIR_TYPE_SLICE:
        if (t->elem) cw_emit_type(sb, t->elem);
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_slice");
        cw_printf(sb, "    %s *data;\n",
           t->elem && t->elem->c_name ? t->elem->c_name : "int32_t");
        cw_put(sb, "    size_t len;\n");
        cw_put(sb, "};\n\n");
        t->emitted = true;
        break;
    case CIR_TYPE_STR:
        if (t->emitted) return;
        /* `struct clux_str` 是跨模块共享的表示：用 include guard 防止多个
           模块头文件同时定义时的重复定义。 */
        cw_put(sb, "#ifndef CLUX_STR_STRUCT_DEFINED\n");
        cw_put(sb, "#define CLUX_STR_STRUCT_DEFINED\n");
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_str");
        cw_put(sb, "    const char *data;\n");
        cw_put(sb, "    size_t len;\n");
        cw_put(sb, "};\n");
        cw_put(sb, "#endif\n\n");
        t->emitted = true;
        break;
    case CIR_TYPE_OPTION:
        if (t->elem) cw_emit_type(sb, t->elem);
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_opt");
        cw_put(sb, "    bool ok;\n");
        cw_printf(sb, "    %s value;\n",
           t->elem && t->elem->c_name ? t->elem->c_name : "int32_t");
        cw_put(sb, "};\n\n");
        t->emitted = true;
        break;
    case CIR_TYPE_UNION:
        for (cir_field_t *f = t->fields; f; f = f->next)
            if (f->type && (f->type->kind == CIR_TYPE_STRUCT ||
                            f->type->kind == CIR_TYPE_ARRAY ||
                            f->type->kind == CIR_TYPE_SLICE ||
                            f->type->kind == CIR_TYPE_OPTION ||
                            f->type->kind == CIR_TYPE_UNION ||
                            f->type->kind == CIR_TYPE_CUNION))
                cw_emit_type(sb, f->type);
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_union");
        cw_put(sb, "    uint32_t tag;\n");
        cw_put(sb, "    union {\n");
        for (cir_field_t *f = t->fields; f; f = f->next)
            cw_printf(sb, "        %s %s;\n",
               f->type && f->type->c_name ? f->type->c_name : "int32_t",
               f->name ? f->name : "member");
        cw_put(sb, "    } u;\n");
        cw_put(sb, "};\n\n");
        t->emitted = true;
        break;
    case CIR_TYPE_CUNION:
        for (cir_field_t *f = t->fields; f; f = f->next)
            if (f->type && (f->type->kind == CIR_TYPE_STRUCT ||
                            f->type->kind == CIR_TYPE_ARRAY ||
                            f->type->kind == CIR_TYPE_SLICE ||
                            f->type->kind == CIR_TYPE_OPTION ||
                            f->type->kind == CIR_TYPE_UNION ||
                            f->type->kind == CIR_TYPE_CUNION))
                cw_emit_type(sb, f->type);
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_cunion");
        cw_put(sb, "    union {\n");
        for (cir_field_t *f = t->fields; f; f = f->next)
            cw_printf(sb, "        %s %s;\n",
               f->type && f->type->c_name ? f->type->c_name : "int32_t",
               f->name ? f->name : "member");
        cw_put(sb, "    } u;\n");
        cw_put(sb, "};\n\n");
        t->emitted = true;
        break;
    default:
        t->emitted = true;
        break;
    }
}

void cw_emit_all_types(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next) cw_emit_type(sb, t);
}

/* 延迟发射：名字以 prefix 开头的类型（如闭包 env 结构体，字段引用函数签名
   struct，必须在签名定义之后）。 */
void cw_emit_all_types_except(string_t *sb, cir_type_t *types,
                              const char *prefix) {
    for (cir_type_t *t = types; t; t = t->next)
        if (!(prefix && t->c_name && strncmp(t->c_name, prefix,
                                             strlen(prefix)) == 0))
            cw_emit_type(sb, t);
}

void cw_emit_types_with_prefix(string_t *sb, cir_type_t *types,
                               const char *prefix) {
    for (cir_type_t *t = types; t; t = t->next)
        if (prefix && t->c_name &&
            strncmp(t->c_name, prefix, strlen(prefix)) == 0)
            cw_emit_type(sb, t);
}

void cw_emit_header_types(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next)
        if (t->in_header) cw_emit_type(sb, t);
}

/* 头文件里的函数签名（函数值类型 `struct <sig>` 的实体）：
 * 先递归参数/返回里的嵌套函数签名，再输出自身（依赖先）。 */
static void emit_header_sig(string_t *sb, cir_signature_t *s) {
    if (!s || s->emitted) return;
    s->emitted = true;
    for (size_t i = 0; i < s->param_count; i++) {
        cir_type_t *p = s->params ? s->params[i] : NULL;
        if (p && p->kind == CIR_TYPE_FUNC) emit_header_sig(sb, p->sig);
    }
    if (s->ret && s->ret->kind == CIR_TYPE_FUNC) emit_header_sig(sb, s->ret->sig);
    cw_write_signature_def(sb, s);
}

void cw_emit_header_signatures(string_t *sb, cir_signature_t *sigs) {
    for (cir_signature_t *s = sigs; s; s = s->next)
        if (s->in_header) emit_header_sig(sb, s);
}

/* 每个被构造过的 bitfield 类型生成一个按值返回的构造函数：
 *   clux_bf_make_<type>(items, n)
 * 值以 `struct clux_bf_item { off, width, value }` 数组传入，按位写入后返回。
 * 只读路径不需要它（clux_bf_get 是通用的）。 */
void cw_emit_bf_helpers(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next) {
        if (t->kind != CIR_TYPE_STRUCT || !t->is_bitfield || !t->needs_bf_make)
            continue;
        if (!t->fields || !t->fields->name || !t->c_name) continue;
        cw_printf(sb, "static %s clux_bf_make_%s(const struct clux_bf_item *items,"
                      " size_t n) {\n",
                  t->c_name, cw_bf_helper_suffix(t));
        cw_printf(sb, "    %s out;\n", t->c_name);
        cw_put(sb, "    memset(&out, 0, sizeof out);\n");
        cw_put(sb, "    for (size_t i = 0; i < n; i++)\n");
        cw_printf(sb, "        clux_bf_set(out.%s.data, %zu, items[i].off, "
                      "items[i].width, items[i].value);\n",
                  t->fields->name, t->bit_word_bits);
        cw_put(sb, "    return out;\n");
        cw_put(sb, "}\n\n");
    }
}
