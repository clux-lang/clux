#include "vm/type.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/scope.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/vec.h"
#include "vm/type_int.h"
#include "vm/type_float.h"
#include "vm/type_bool.h"
#include "vm/type_str.h"
#include "vm/type_void.h"
#include "vm/type_type.h"
#include "vm/type_func.h"
#include "vm/type_error.h"
#include "vm/type_interrupt.h"

#include <string.h>
#include <stdalign.h>

/* ---- type_lookup：类型名按作用域链解析（类型即表达式） ---- */

/* 与变量同机制：内建类型值注册在 global scope，自定义类型（M2 type 定义）
   注册到定义点当前作用域。scope_lookup 沿 current→root→global 链查找；
   变量遮蔽类型时命中非 type value → NULL（遮蔽语义，见 m2-design 决策 6）。 */
const type_t *type_lookup(const vm_t *vm, strslice_t name) {
    if (!vm) return NULL;
    value_t *v = scope_lookup(vm->current_scope, name);
    if (!v || value_type(v) != vm->type_type) return NULL;
    return value_as(v, const type_t *);
}

/* ---- type_as_value ---- */

value_t *type_as_value(vm_t *vm, const type_t *t) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_type, &t);
    return value_make(vm, vm->type_type, data);
}

/* ---- 类型 id 表（id → type_t*，LOAD_TYPE <id> 查表压栈） ---- */

/* 内建类型 id 段 0..16（vm_register_builtin_types + error/interrupt）。
   程序类型 id 由 sema 分配，从 64 起（预留扩展空隙，见 vm.h 注释）。
   TYPE_ID_BUILTIN_COUNT / TYPE_ID_PROGRAM_BASE 定义于 vm/type.h（sema/编译器共用）。 */

/* BIND_TYPE 登记：扩容至 id+1 后写入。重复登记幂等（同一实例多 id 别名）。
   空洞槽位（内建段与程序段之间 17..63）以 NULL 填充——vec_push 拒绝
   NULL 值（no-op），故用 vec_resize 扩展长度填充。 */
void vm_type_bind(vm_t *vm, uint32_t id, const type_t *t) {
    if (!vm || !t) return;
    if (vec_len(vm->types_by_id) <= id) {
        vec_resize(vm->types_by_id, vm->alloc, (size_t)id + 1);
    }
    vec_set(vm->types_by_id, id, (void *)t);
}

/* LOAD_TYPE 查表：id 越界或未登记返回 NULL。 */
const type_t *vm_type_load(vm_t *vm, uint32_t id) {
    if (!vm || id >= vec_len(vm->types_by_id)) return NULL;
    return (const type_t *)vec_get(vm->types_by_id, id);
}

/* SET_TYPE_NAME：设置显示名。仅程序类型（id >= TYPE_ID_PROGRAM_BASE）可改名；
   内置类型 name 是静态字符串（STRSLICE_LIT），不可释放也不可替换。 */
bool type_set_name(vm_t *vm, const type_t *t, strslice_t name) {
    if (!vm || !t || !name.ptr) return false;
    if (t->id < TYPE_ID_PROGRAM_BASE) return false;  /* 内置类型不可改名 */

    /* 旧 name 若为堆分配的规范名（seal 时生成），先释放 */
    type_t *mut = (type_t *)t;
    if (mut->name.ptr) {
        char *np = (char *)mut->name.ptr;
        allocator_free(vm->alloc, (void **)&np);
        mut->name = (strslice_t){ NULL, 0 };
    }

    /* 拷贝新 name（NUL 结尾，vm 拥有生命周期） */
    char *buf = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char),
                                         NULL, NULL, NULL, name.len + 1);
    if (!buf) return false;
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    mut->name = (strslice_t){ buf, name.len };
    return true;
}

/* ---- type_equal / type_extends（鸭子类型判断） ---- */

/*
 * 分派到 a 自身 vtable 的类型运算槽位。NULL 槽位 = 默认：
 *   - type_equal：指针比较（基础类型单例，const/volatile 独立 intern）
 *   - type_extends：同 type_equal（默认严格兼容，M2 复合类型按成员扩展）
 */
bool type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (!a || !b) return a == b;
    if (a == b) return true;
    if (a->vtable && a->vtable->type_equal)
        return a->vtable->type_equal(vm, a, b);
    return false;
}

bool type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (!sub || !sup) return false;
    if (sub == sup) return true;
    if (sub->vtable && sub->vtable->type_extends)
        return sub->vtable->type_extends(vm, sub, sup);
    /* 默认：兼容 = 鸭子相等（无复合结构时即类型相同） */
    return type_equal(vm, sub, sup);
}

/* ---- const/volatile 辅助 ---- */

const type_t *type_qualifier_sub(const type_t *t) {
    if (!t) return NULL;
    if (t->kind == TYPE_KIND_CONST)
        return ((const const_type_t *)t)->sub;
    if (t->kind == TYPE_KIND_VOLATILE)
        return ((const volatile_type_t *)t)->sub;
    return NULL;
}

bool type_has_const(const type_t *t) {
    for (const type_t *p = t; p; p = type_qualifier_sub(p))
        if (p->kind == TYPE_KIND_CONST) return true;
    return false;
}

/* ---- type_promote ---- */

typedef enum {
    CAT_NONE    = 0,  /* 非数值（str/void/type/func/error） */
    CAT_BOOL    = 1,
    CAT_SINT    = 2,  /* 有符号整数 */
    CAT_UINT    = 3,  /* 无符号整数 */
    CAT_FLOAT   = 4,
} type_cat_t;

static type_cat_t type_category(const vm_t *vm, const type_t *t) {
    if (t == vm->type_bool) return CAT_BOOL;
    if (t == vm->type_i8 || t == vm->type_i16 ||
        t == vm->type_i32 || t == vm->type_i64) return CAT_SINT;
    if (t == vm->type_u8 || t == vm->type_u16 ||
        t == vm->type_u32 || t == vm->type_u64) return CAT_UINT;
    if (t == vm->type_f32 || t == vm->type_f64) return CAT_FLOAT;
    return CAT_NONE;
}

static int type_rank_val(const vm_t *vm, const type_t *t) {
    if (t == vm->type_bool) return 0;
    if (t == vm->type_i8)   return 1;
    if (t == vm->type_u8)   return 2;
    if (t == vm->type_i16)  return 3;
    if (t == vm->type_u16)  return 4;
    if (t == vm->type_i32)  return 5;
    if (t == vm->type_u32)  return 6;
    if (t == vm->type_i64)  return 7;
    if (t == vm->type_u64)  return 8;
    if (t == vm->type_f32)  return 9;
    if (t == vm->type_f64)  return 10;
    return -1;  /* 非数值类型 */
}

const type_t *type_promote(const vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return a;

    type_cat_t ca = type_category(vm, a);
    type_cat_t cb = type_category(vm, b);

    if (ca == CAT_NONE || cb == CAT_NONE) return NULL;  /* 非数值类型 */
    if (ca != cb) return NULL;  /* 不同类别（bool/int/float）不协商 */

    int ra = type_rank_val(vm, a);
    int rb = type_rank_val(vm, b);
    return (ra >= rb) ? a : b;
}

/* ================================================================ */
/* 内置类型实例（全局静态，由 vm_init_builtins 初始化）                   */
/* ================================================================ */

static type_t g_type_i8   = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_i16  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_i32  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_i64  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_u8   = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_u16  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_u32  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_u64  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_f32  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_f64  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_bool = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_str  = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_void = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_type = { NULL, {NULL,0}, 0, 0, 0, false };
/* func 基类声明为 func_type_t 布局：无签名（sig 全零），向下转型安全 */
static func_type_t g_type_func = { { NULL, {NULL,0}, 0, 0, 0, false }, { NULL, 0, NULL, false } };
static type_t g_type_error = { NULL, {NULL,0}, 0, 0, 0, false };
static type_t g_type_interrupt = { NULL, {NULL,0}, 0, 0, 0, false };

void vm_init_builtins(vm_t *vm) {
    static const char S_I8[]  = "i8",   S_I16[] = "i16", S_I32[] = "i32", S_I64[] = "i64";
    static const char S_U8[]  = "u8",   S_U16[] = "u16", S_U32[] = "u32", S_U64[] = "u64";
    static const char S_F32[] = "f32",  S_F64[] = "f64";
    static const char S_BOOL[] = "bool", S_STR[] = "str";
    static const char S_VOID[] = "void", S_TYPE[] = "type", S_FUNC[] = "func";
    static const char S_ERROR[] = "error";
    static const char S_INTERRUPT[] = "interrupt";

    /* 函数签名类型池（type_func_sig intern 用）；元素由 vm_destroy 手动释放，
       vec 只持有指针数组（与 scope owned 同一模式） */
    vm->sig_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 数组类型池（type_array_intern intern 用）；元素由 vm_destroy 手动释放 */
    vm->array_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 内置基础类型创建即 sealed（无开放构造阶段，永不重新 intern）。
       id 按 0..14 固定（vm_register_builtin_types 与 LOAD_TYPE 内建段一致）。 */
    g_type_i8   = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I8),  sizeof(int8_t),   alignof(int8_t),   TYPE_KIND_INT,  true,  0 };
    g_type_i16  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I16), sizeof(int16_t),  alignof(int16_t),  TYPE_KIND_INT,  true,  1 };
    g_type_i32  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I32), sizeof(int32_t),  alignof(int32_t),  TYPE_KIND_INT,  true,  2 };
    g_type_i64  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I64), sizeof(int64_t),  alignof(int64_t),  TYPE_KIND_INT,  true,  3 };
    g_type_u8   = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U8),  sizeof(uint8_t),   alignof(uint8_t),  TYPE_KIND_INT,  true,  4 };
    g_type_u16  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U16), sizeof(uint16_t),  alignof(uint16_t), TYPE_KIND_INT,  true,  5 };
    g_type_u32  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U32), sizeof(uint32_t),  alignof(uint32_t), TYPE_KIND_INT,  true,  6 };
    g_type_u64  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U64), sizeof(uint64_t),  alignof(uint64_t), TYPE_KIND_INT,  true,  7 };
    g_type_f32  = (type_t){ &VTABLE_FLOAT, STRSLICE_LIT(S_F32), sizeof(float),    alignof(float),    TYPE_KIND_FLOAT, true,  8 };
    g_type_f64  = (type_t){ &VTABLE_FLOAT, STRSLICE_LIT(S_F64), sizeof(double),   alignof(double),   TYPE_KIND_FLOAT, true,  9 };
    g_type_bool = (type_t){ &VTABLE_BOOL, STRSLICE_LIT(S_BOOL), sizeof(bool),     alignof(bool),     TYPE_KIND_BOOL, true, 10 };
    g_type_str  = (type_t){ &VTABLE_STR,  STRSLICE_LIT(S_STR),  sizeof(string_t*), alignof(string_t*), TYPE_KIND_STR,  true, 11 };
    g_type_void = (type_t){ &VTABLE_VOID, STRSLICE_LIT(S_VOID), 0, 1, TYPE_KIND_VOID, true, 12 };
    g_type_type = (type_t){ &VTABLE_TYPE, STRSLICE_LIT(S_TYPE), sizeof(const type_t*), alignof(const type_t*), TYPE_KIND_TYPE, true, 13 };
    g_type_func.base = (type_t){ &VTABLE_FUNC, STRSLICE_LIT(S_FUNC), sizeof(func_t*), alignof(func_t*), TYPE_KIND_FUNC, true, 14 };

    /* error_data_t 内联在 value data 块中 */
    g_type_error = (type_t){ &VTABLE_ERROR, STRSLICE_LIT(S_ERROR),
                             sizeof(error_data_t), alignof(error_data_t),
                             TYPE_KIND_ERROR, true, 15 };

    /* interrupt_data_t 内联在 value data 块中（引擎级控制流哨兵） */
    g_type_interrupt = (type_t){ &VTABLE_INTERRUPT, STRSLICE_LIT(S_INTERRUPT),
                                 sizeof(interrupt_data_t), alignof(interrupt_data_t),
                                 TYPE_KIND_INTERRUPT, true, 16 };

    vm->type_i8   = &g_type_i8;
    vm->type_i16  = &g_type_i16;
    vm->type_i32  = &g_type_i32;
    vm->type_i64  = &g_type_i64;
    vm->type_u8   = &g_type_u8;
    vm->type_u16  = &g_type_u16;
    vm->type_u32  = &g_type_u32;
    vm->type_u64  = &g_type_u64;
    vm->type_f32  = &g_type_f32;
    vm->type_f64  = &g_type_f64;
    vm->type_bool = &g_type_bool;
    vm->type_str  = &g_type_str;
    vm->type_void = &g_type_void;
    vm->type_type = &g_type_type;
    vm->type_func = &g_type_func.base;
    vm->type_error = &g_type_error;
    vm->type_interrupt = &g_type_interrupt;
}
