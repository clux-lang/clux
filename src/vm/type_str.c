#include "vm/type_str.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/string.h"
#include "core/panic.h"

static value_t *bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

static value_t *str_eq(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, eq, "==");
    /* nil 侧：比较字符串指针是否为 NULL（str 的 0 初始化语义） */
    if (value_type(a) == vm->type_nil || value_type(b) == vm->type_nil) {
        if (value_type(a) == vm->type_str && value_type(b) != vm->type_str)
            return value_make_error(vm, "==: type mismatch");
        if (value_type(b) == vm->type_str && value_type(a) != vm->type_str)
            return value_make_error(vm, "==: type mismatch");
        if (value_is_shadow(a) || value_is_shadow(b))
            return value_make_shadow(vm, vm->type_bool);
        const string_t *s = *(const string_t **)value_data(
            value_type(a) == vm->type_str ? a : b);
        return bool_store(vm, s == NULL);
    }
    if (value_type(a) != vm->type_str || value_type(b) != vm->type_str)
        return value_make_error(vm, "==: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    string_t *sa = *(string_t **)value_data(a);
    string_t *sb = *(string_t **)value_data(b);
    return bool_store(vm, string_equals(sa, sb));
}

static value_t *str_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_type(a) == vm->type_nil || value_type(b) == vm->type_nil) {
        if (value_type(a) == vm->type_str && value_type(b) != vm->type_str)
            return value_make_error(vm, "!=: type mismatch");
        if (value_type(b) == vm->type_str && value_type(a) != vm->type_str)
            return value_make_error(vm, "!=: type mismatch");
        if (value_is_shadow(a) || value_is_shadow(b))
            return value_make_shadow(vm, vm->type_bool);
        const string_t *s = *(const string_t **)value_data(
            value_type(a) == vm->type_str ? a : b);
        return bool_store(vm, s != NULL);
    }
    if (value_type(a) != vm->type_str || value_type(b) != vm->type_str)
        return value_make_error(vm, "!=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    string_t *sa = *(string_t **)value_data(a);
    string_t *sb = *(string_t **)value_data(b);
    return bool_store(vm, !string_equals(sa, sb));
}

static void str_dispose(vm_t *vm, value_t *v) {
    (void)vm;
    string_t **sp = (string_t **)value_data(v);
    if (sp && *sp) {
        string_free(sp);
    }
}

static value_t *str_clone(vm_t *vm, value_t *v) {
    /* shadow：返回新 shadow（不分配 data） */
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    /* 零值 str（NULL 指针，数组自动 0 填充产物）克隆保持 NULL，与
       array_blit_raw 的 STR 分支一致 */
    string_t *src = *(string_t **)value_data(v);
    string_t *copy = src ? string_from_string(vm->alloc, src) : NULL;
    if (src && !copy) panic("vm: out of memory cloning string");
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), &copy);
    return value_make(vm, value_type(v), data);
}

static value_t *str_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* shadow：只检查类型兼容性，不拷贝 data */
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    /* nil 源：释放旧 string，置 NULL（str 的 0 初始化语义） */
    if (value_type(src) == vm->type_nil) {
        string_t **dp = (string_t **)value_data(dst);
        if (dp && *dp) string_free(dp);
        if (dp) *dp = NULL;
        return dst;
    }
    /* dispose 旧 string，clone 新 string */
    string_t **dp = (string_t **)value_data(dst);
    if (dp && *dp) string_free(dp);
    string_t *s = *(string_t **)value_data(src);
    string_t *copy = string_from_string(vm->alloc, s);
    if (!copy) panic("vm: out of memory assigning string");
    *dp = copy;
    return dst;
}

const vtable_t VTABLE_STR = {
    .eq = str_eq, .ne = str_ne,
    .dispose = str_dispose, .clone = str_clone, .assign = str_assign,
};
