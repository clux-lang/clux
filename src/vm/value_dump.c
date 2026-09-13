#include "vm/value_dump.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_array.h"
#include "core/string.h"
#include "core/strslice.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * value_dump：以 `.type { value }` 形式（复杂类型递归包裹）渲染任意 value。
 *
 * 仅依赖 value 层 API（value_type / value_kind / value_data / value_is_shadow）
 * 与数组只读访问器（value_array_count / value_array_at），不触碰各类型私有布局。
 * =========================================================================== */

/* ---- 本地：按 value 实际类型宽度读取值 ---- */

static int64_t dump_read_signed(const value_t *v) {
    switch (value_type(v)->size) {
    case 1: return (int64_t)*(const int8_t  *)value_data(v);
    case 2: return (int64_t)*(const int16_t *)value_data(v);
    case 4: return (int64_t)*(const int32_t *)value_data(v);
    default: return *(const int64_t *)value_data(v);
    }
}

static uint64_t dump_read_unsigned(const value_t *v) {
    switch (value_type(v)->size) {
    case 1: return (uint64_t)*(const uint8_t  *)value_data(v);
    case 2: return (uint64_t)*(const uint16_t *)value_data(v);
    case 4: return (uint64_t)*(const uint32_t *)value_data(v);
    default: return *(const uint64_t *)value_data(v);
    }
}

static double dump_read_double(const value_t *v) {
    if (value_type(v)->size == sizeof(float))
        return (double)*(const float *)value_data(v);
    return *(const double *)value_data(v);
}

/* ---- 类型名渲染：语言原生风格 [N]T / const T / volatile T ---- */

static void type_dump_name(const type_t *t, string_t *out) {
    if (!t) { string_append_cstr(out, "?"); return; }
    switch (t->kind) {
    case TYPE_KIND_ARRAY: {
        const type_t *elem = array_type_elem(t);
        size_t len = array_type_len(t);
        string_append_char(out, '[');
        if (len != SIZE_MAX) {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "%zu", len);
            string_append_bytes(out, buf, (size_t)n);
        }
        string_append_char(out, ']');
        type_dump_name(elem, out);   /* 嵌套：[2][3]i32 */
        return;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const char *kw = (t->kind == TYPE_KIND_CONST) ? "const " : "volatile ";
        string_append_cstr(out, kw);
        type_dump_name(type_qualifier_sub(t), out);
        return;
    }
    default:
        if (t->name.ptr) string_append_bytes(out, t->name.ptr, t->name.len);
        else string_append_cstr(out, "?");
        return;
    }
}

/* ---- 递归 dumper ---- */

static void value_dump_impl(const vm_t *vm, const value_t *v, string_t *out) {
    const type_t *t = v ? value_type(v) : NULL;
    string_append_char(out, '.');
    type_dump_name(t, out);

    /* const / volatile 不改变数据布局，值体按底层 kind 决定，但名字保留限定名 */
    type_kind_t k = t ? t->kind : TYPE_KIND_VOID;
    const type_t *bt = t;
    if (k == TYPE_KIND_CONST || k == TYPE_KIND_VOLATILE) {
        const type_t *sub = type_qualifier_sub(t);
        if (sub) { bt = sub; k = sub->kind; }
    }
    /* 聚合类型（数组/结构体）的 { } 前后加空格便于阅读；基础类型保持紧凑
       `.i32{42}`（与语言构造字面量 .i32{...} 一致），即 `. [3]i32 { .i32{1} }`。 */
    bool agg = (k == TYPE_KIND_ARRAY || k == TYPE_KIND_STRUCT);

    if (!v) { string_append_cstr(out, "{<null>}"); return; }
    if (value_is_shadow(v)) {
        string_append_cstr(out, "{<shadow>}");
        return;
    }

    string_append_cstr(out, agg ? " { " : "{");

    switch (k) {
    case TYPE_KIND_BOOL:
        string_append_cstr(out,
            (*(const uint8_t *)value_data(v)) ? "true" : "false");
        break;
    case TYPE_KIND_INT: {
        bool unsign = (bt->name.ptr && bt->name.len > 0 && bt->name.ptr[0] == 'u');
        char buf[32];
        int n = unsign
            ? snprintf(buf, sizeof buf, "%llu",
                       (unsigned long long)dump_read_unsigned(v))
            : snprintf(buf, sizeof buf, "%lld",
                       (long long)dump_read_signed(v));
        string_append_bytes(out, buf, (size_t)n);
        break;
    }
    case TYPE_KIND_FLOAT: {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "%g", dump_read_double(v));
        string_append_bytes(out, buf, (size_t)n);
        break;
    }
    case TYPE_KIND_STR: {
        string_t *s = *(string_t **)value_data(v);
        string_append_char(out, '"');
        if (s) string_append_cstr(out, string_cstr(s));
        string_append_char(out, '"');
        break;
    }
    case TYPE_KIND_ARRAY: {
        size_t n = value_array_count(v);
        for (size_t i = 0; i < n; i++) {
            if (i) string_append_cstr(out, ", ");
            value_dump_impl(vm, value_array_at(v, i), out);
        }
        break;
    }
    case TYPE_KIND_STRUCT:
        /* VM 尚未实现 struct：best-effort 空体（待字段遍历后扩展本分支） */
        break;
    default:
        string_append_cstr(out, "<");
        if (t && t->name.ptr) string_append_bytes(out, t->name.ptr, t->name.len);
        else string_append_cstr(out, "?");
        string_append_char(out, '>');
        break;
    }

    string_append_cstr(out, agg ? " }" : "}");
}

void value_dump_string(const vm_t *vm, const value_t *v, string_t *out) {
    (void)vm;
    if (!out) return;
    value_dump_impl(vm, v, out);
}
