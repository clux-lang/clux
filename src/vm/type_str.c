#include "vm/type_str.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/str_pool.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * str 类型（平凡化后）
 *
 * str value 的 data 块直接存池内字符串的 const char * 指针（含 '\0'）——
 * sizeof(str)==指针大小，与 C 语言 const char * 对齐。字符串本体由 vm 统一
 * intern / 统一释放，value 侧零生命周期代码：
 *   - dispose：no-op（data 块由 value_dispose 统一释放，池内存归池）
 *   - clone：alloc + memcpy（data 全平凡，memcpy 即可）
 *   - assign：alloc + memcpy 覆盖（旧 data 块由 value_dispose 统一释放）
 *   - eq/ne：按池指针内容比较（ptr 相等即同内容，因池去重；兜底 memcmp）
 * =========================================================================== */

static value_t *bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

/* 读 str data 的池内指针 */
static const char *str_view(const value_t *v) {
    return *(const char *const *)value_data(v);
}

static bool str_content_eq(const char *a, const char *b) {
    if (a == b) return true;   /* 池去重：同指针即同内容 */
    if (!a || !b) return false;
    return strcmp(a, b) == 0;  /* 兜底：不同池块内容比较 */
}

static value_t *str_eq(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_type(a) != vm->type_str || value_type(b) != vm->type_str)
        return value_make_error(vm, "==: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, str_content_eq(str_view(a), str_view(b)));
}

static value_t *str_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_type(a) != vm->type_str || value_type(b) != vm->type_str)
        return value_make_error(vm, "!=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, !str_content_eq(str_view(a), str_view(b)));
}

static void str_dispose(vm_t *vm, value_t *v) {
    /* data 全平凡：字符串本体归池，此处无需释放（池 vm_destroy 统一释放） */
    (void)vm; (void)v;
}

static value_t *str_clone(vm_t *vm, value_t *v) {
    /* shadow：返回新 shadow（不分配 data） */
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    /* 平凡拷贝：data 块是池内指针，memcpy 即深拷贝（池内存共享） */
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

static value_t *str_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* shadow：只检查类型兼容性，不拷贝 data */
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    /* 覆盖 data 块（池内指针拷贝）；旧块由 value_dispose 统一释放 */
    memcpy(value_data(dst), value_data(src), value_type(dst)->size);
    return dst;
}

const vtable_t VTABLE_STR = {
    .eq = str_eq, .ne = str_ne,
    .dispose = str_dispose, .clone = str_clone, .assign = str_assign,
};
