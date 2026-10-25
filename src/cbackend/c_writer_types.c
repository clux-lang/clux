#include "cbackend/c_writer_internal.h"

/* 鎶婄被鍨嬫爣璁颁负"杩涘ご鏂囦欢"锛屽苟閫掑綊灞曞紑瀹冩寜鍊?鎸夋寚閽堜緷璧栫殑绫诲瀷锛? * struct/cunion/union 鐨勫瓧娈点€乤rray/option/slice 鐨勫厓绱犮€乸tr 鐨勫熀绫诲瀷銆? * func 绛惧悕鐨勫弬鏁?杩斿洖銆傞€掑綊鍦ㄦ寚閽堝鏀舵暃锛堥€掑綊 struct 闈犳寚閽堟墦鏂級銆?*/
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
            /* 闂寘绛惧悕鐨?env 缁撴瀯浣擄紙鍐呰仈鎹曡幏鍧楋級鎸夊€煎祵杩涚鍚?struct锛?               璺ㄦā鍧楀鍑?瀵煎叆鏃跺繀椤诲湪澶存枃浠堕噷涔熸湁瀹氫箟銆?*/
            cw_mark_header_type(t->sig->env_type);
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
               (f->type->kind == CIR_TYPE_STRUCT || f->type->kind == CIR_TYPE_STR ||
                 f->type->kind == CIR_TYPE_ARRAY ||
                 f->type->kind == CIR_TYPE_SLICE ||
                 f->type->kind == CIR_TYPE_OPTION ||
                 f->type->kind == CIR_TYPE_UNION ||
                 f->type->kind == CIR_TYPE_FUNC ||
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
        /* `struct clux_str` 鏄法妯″潡鍏变韩鐨勮〃绀猴細鐢?include guard 闃叉澶氫釜
           妯″潡澶存枃浠跺悓鏃跺畾涔夋椂鐨勯噸澶嶅畾涔夈€?*/
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
            if (f->type && (f->type->kind == CIR_TYPE_STRUCT || f->type->kind == CIR_TYPE_STR ||
                            f->type->kind == CIR_TYPE_ARRAY ||
                            f->type->kind == CIR_TYPE_SLICE ||
                            f->type->kind == CIR_TYPE_OPTION ||
                            f->type->kind == CIR_TYPE_UNION ||
                            f->type->kind == CIR_TYPE_FUNC ||
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
            if (f->type && (f->type->kind == CIR_TYPE_STRUCT || f->type->kind == CIR_TYPE_STR ||
                            f->type->kind == CIR_TYPE_ARRAY ||
                            f->type->kind == CIR_TYPE_SLICE ||
                            f->type->kind == CIR_TYPE_OPTION ||
                            f->type->kind == CIR_TYPE_UNION ||
                            f->type->kind == CIR_TYPE_FUNC ||
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
    case CIR_TYPE_FUNC:
        /* 鍑芥暟鍊肩被鍨嬫寜鍊煎嚭鐜板湪鑱氬悎瀛楁閲岋紙`struct Holder { cb: F; }`锛夋椂锛?           绛惧悕 struct 蹇呴』鍏堜簬璇ヨ仛鍚堝畾涔夈€傞€掑綊鍙戝皠鍙傛暟/杩斿洖/env锛屽啀杈撳嚭
           绛惧悕瀹氫箟锛涘凡鍦ㄥご鏂囦欢/鍓嶉潰鍙戝皠杩囩殑璺宠繃銆?*/
        if (t->emitted) return;
        t->emitted = true;
        if (t->sig && !t->sig->emitted) {
            cw_emit_type(sb, t->sig->ret);
            for (size_t i = 0; i < t->sig->param_count; i++)
                cw_emit_type(sb, t->sig->params[i]);
            cw_emit_type(sb, t->sig->env_type);
            t->sig->emitted = true;
            cw_write_signature_def(sb, t->sig);
        }
        break;
    default:
        t->emitted = true;
        break;
    }
}

void cw_emit_all_types(string_t *sb, cir_type_t *types) {
    for (cir_type_t *t = types; t; t = t->next) cw_emit_type(sb, t);
}

/* 寤惰繜鍙戝皠锛氬悕瀛椾互 prefix 寮€澶寸殑绫诲瀷锛堝闂寘 env 缁撴瀯浣擄紝瀛楁寮曠敤鍑芥暟绛惧悕
   struct锛屽繀椤诲湪绛惧悕瀹氫箟涔嬪悗锛夈€?*/
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

/* 澶存枃浠堕噷鐨勫嚱鏁扮鍚嶏紙鍑芥暟鍊肩被鍨?`struct <sig>` 鐨勫疄浣擄級锛? * 鍏堥€掑綊鍙傛暟/杩斿洖閲岀殑宓屽鍑芥暟绛惧悕锛屽啀杈撳嚭鑷韩锛堜緷璧栧厛锛夈€?*/
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

/* 姣忎釜琚瀯閫犺繃鐨?bitfield 绫诲瀷鐢熸垚涓€涓寜鍊艰繑鍥炵殑鏋勯€犲嚱鏁帮細
 *   clux_bf_make_<type>(items, n)
 * 鍊间互 `struct clux_bf_item { off, width, value }` 鏁扮粍浼犲叆锛屾寜浣嶅啓鍏ュ悗杩斿洖銆? * 鍙璺緞涓嶉渶瑕佸畠锛坈lux_bf_get 鏄€氱敤鐨勶級銆?*/
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
