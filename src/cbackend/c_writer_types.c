#include "cbackend/c_writer_internal.h"

void cw_write_type_defs(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next)
        if (t->kind == CIR_TYPE_STRUCT || t->kind == CIR_TYPE_ARRAY ||
            t->kind == CIR_TYPE_SLICE || t->kind == CIR_TYPE_STR ||
            t->kind == CIR_TYPE_OPTION || t->kind == CIR_TYPE_UNION ||
            t->kind == CIR_TYPE_CUNION)
            cw_printf(sb, "struct %s;\n", t->c_name + 7);
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
        cw_printf(sb, "%s {\n", t->c_name ? t->c_name : "struct clux_str");
        cw_put(sb, "    const char *data;\n");
        cw_put(sb, "    size_t len;\n");
        cw_put(sb, "};\n\n");
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

void cw_write_env_union(string_t *sb, cir_type_t *types) {
    cw_put(sb, "union clux_env_any {\n");
    cw_put(sb, "    char clux_dummy;\n");
    for (cir_type_t *t = types; t; t = t->next)
        if (t->is_closure_env && t->c_name && t->env_field)
            cw_printf(sb, "    %s %s;\n", t->c_name, t->env_field);
    cw_put(sb, "};\n\n");
}
