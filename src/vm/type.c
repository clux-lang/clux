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
#include "vm/type_opaque.h"
#include "vm/type_module.h"
#include "vm/type_array.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_option.h"
#include "vm/type_union.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"

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
   程序类型 id 由 type_id_alloc 全 VM 全局分配，从 64 起（预留扩展空隙）。
   TYPE_ID_BUILTIN_COUNT / TYPE_ID_PROGRAM_BASE 定义于 vm/type.h（sema/编译器共用）。 */

/* 分配全 VM 唯一的程序类型 id。id 溢出（>u32 上限）时返回 0 —— 调用方
   （sema_type_register / 编译器）按"分配失败"处理并报错，不静默复用。 */
uint32_t type_id_alloc(vm_t *vm) {
    if (!vm) return 0;
    if (vm->type_id_next == 0) vm->type_id_next = TYPE_ID_PROGRAM_BASE;
    if (vm->type_id_next == 0xFFFFFFFFu) return 0;
    return vm->type_id_next++;
}

/* DEFINE_TYPE <id> 声明登记（SEAL 密封后按自身 id 幂等重绑）：扩容至
   id+1 后写入。重复登记幂等（同一实例多 id 别名）。
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

/* 派生类型布局延迟定型（见 include/vm/type.h 的说明）。
 * 遍历 const/volatile/数组池，把"直接以 root 为布局来源"的派生类型同步到
 * root 的最终布局，并以修好的类型为新 root 继续（链式派生）。 */
void type_sync_derived_layout(vm_t *vm, const type_t *sub) {
    if (!vm || !sub || !type_is_sealed(sub)) return;
    const type_t *roots[64];
    size_t nroots = 0;
    roots[nroots++] = sub;
    for (size_t pass = 0; pass < 8; pass++) {
        size_t fixed = 0;
        for (size_t r = 0; r < nroots; r++) {
            const type_t *root = roots[r];
            /* const / volatile：布局 = sub 的布局 */
            vec_t *pools[2] = {vm->const_types, vm->volatile_types};
            for (size_t p = 0; p < 2; p++) {
                vec_t *pool = pools[p];
                if (!pool) continue;
                size_t n = vec_len(pool);
                for (size_t i = 0; i < n; i++) {
                    const type_t *t = (const type_t *)vec_get(pool, i);
                    if (!t || t == root || !type_is_sealed(t)) continue;
                    if (type_qualifier_sub(t) != root) continue;
                    if (t->size == root->size && t->align == root->align) continue;
                    ((type_t *)t)->size  = root->size;
                    ((type_t *)t)->align = root->align;
                    if (nroots < 64) roots[nroots++] = t;
                    fixed++;
                }
            }
            /* 数组：[N]T 布局 = N * sizeof(T)（对齐 = T 的对齐） */
            if (vm->array_types) {
                size_t n = vec_len(vm->array_types);
                for (size_t i = 0; i < n; i++) {
                    const type_t *t = (const type_t *)vec_get(vm->array_types, i);
                    if (!t || t == root || !type_is_sealed(t)) continue;
                    if (array_type_elem(t) != root) continue;
                    size_t eal = root->align ? root->align : 1;
                    size_t nsz = array_type_len(t) * root->size;
                    if (t->size == nsz && t->align == eal) continue;
                    ((type_t *)t)->size  = nsz;
                    ((type_t *)t)->align = eal;
                    if (nroots < 64) roots[nroots++] = t;
                    fixed++;
                }
            }
        }
        if (fixed == 0) break;
    }
}

bool type_has_const(const type_t *t) {
    for (const type_t *p = t; p; p = type_qualifier_sub(p))
        if (p->kind == TYPE_KIND_CONST) return true;
    return false;
}

/* SET_TYPE 运行期用：把 sub 设进开放限定类型（const/volatile 两遍构造的
   "设字段"一步）。密封后静默忽略（与 array/func 的 set 系列一致）。 */
void type_qual_set_sub(vm_t *vm, const type_t *t, const type_t *sub) {
    (void)vm;
    if (!t || !sub) return;
    if (type_is_sealed(t)) return;
    switch (t->kind) {
        case TYPE_KIND_CONST:
            ((const_type_t *)t)->sub = sub;
            break;
        case TYPE_KIND_VOLATILE:
            ((volatile_type_t *)t)->sub = sub;
            break;
        default:
            break; /* 非限定类型静默忽略 */
    }
}

/* ---- 所有权设置（fatal T 的将亡值变体） ---- */

bool type_set_ownership(vm_t *vm, const type_t *t, ownership_t ownership) {
    if (!vm || !t) return false;
    /* 不变量：类型属性只能在 seal 前编辑（与 add_field / set_embeds /
       mark_unique 同一道闸门）。 */
    if (type_is_sealed(t)) return false;
    /* 复合类型的 ownership 恒为 OWN_NONE，fatal 是唯一例外——只此一条转换 */
    if (ownership != OWN_FATAL) return false;
    if (t->ownership != OWN_NONE) return false;
    if (!t->vtable || !t->vtable->type_set_ownership) return false; /* 标量/指针/切片… */
    return t->vtable->type_set_ownership(vm, t, ownership);
}

/* 五个复合类型 vtable 共用的槽位实现（见 type.h 说明） */
bool type_set_ownership_default(vm_t *vm, const type_t *self,
                                ownership_t ownership) {
    (void)vm;
    if (!self) return false;
    ((type_t *)self)->ownership = ownership;
    return true;
}

/* "fatal " + base 名（见 type.h）。前缀只此一份实现，五种复合类型共用。
 * 长度取自 strslice_t（不用 strlen：名可能含内嵌字节）。 */
char *type_name_with_ownership(allocator_t *alloc, const type_t *base,
                               ownership_t ownership, const char *fallback) {
    if (!alloc) return NULL;
    const char *prefix = (ownership == OWN_FATAL) ? "fatal " : "";
    size_t plen = (ownership == OWN_FATAL) ? 6u : 0u;
    const char *bn = (base && base->name.ptr) ? base->name.ptr : fallback;
    size_t blen = 0;
    if (base && base->name.ptr) {
        blen = base->name.len;
    } else if (bn) {
        while (bn[blen]) blen++;
    }
    char *buf = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                        NULL, plen + blen + 1);
    if (!buf) return NULL;
    if (plen) memcpy(buf, prefix, plen);
    if (blen) memcpy(buf + plen, bn, blen);
    buf[plen + blen] = '\0';
    return buf;
}

/* ---- 递归 own 资源判定（见 type.h） ---- */

bool type_owns_resources(const type_t *t) {
    if (!t) return false;
    if (type_directly_owns(t)) return true; /* own/share/weak 指针、own 切片 */
    switch (t->kind) {
        case TYPE_KIND_STRUCT: {
            const struct_type_t *st = (const struct_type_t *)t;
            for (size_t i = 0; i < st->field_count; i++)
                if (type_owns_resources(st->fields[i].type)) return true;
            return false;
        }
        case TYPE_KIND_TUPLE: {
            const tuple_type_t *tt = (const tuple_type_t *)t;
            for (size_t i = 0; i < tt->elem_count; i++)
                if (type_owns_resources(tt->elems[i].type)) return true;
            return false;
        }
        case TYPE_KIND_UNION: {
            const union_type_t *ut = (const union_type_t *)t;
            for (size_t i = 0; i < ut->member_count; i++)
                if (type_owns_resources(ut->members[i].payload_type)) return true;
            return false;
        }
        case TYPE_KIND_OPTION:
            return type_owns_resources(((const option_type_t *)t)->inner);
        case TYPE_KIND_ARRAY:
            return type_owns_resources(((const array_type_t *)t)->elem_type);
        default:
            return false; /* 标量/str/ptr(已判)/slice(已判)/cunion/enum/func/opaque */
    }
}

/* ---- fatal 将亡值是否携带可转移的堆资源（见 type.h） ---- */

bool type_fatal_carries_resource(const type_t *t) {
    if (!t || !type_is_fatal(t)) return false;
    if (type_is_ptr(t) || type_is_slice(t)) return true; /* 裸指针 / 切片堆块 */
    return type_owns_resources(t);                       /* 复合：内嵌 own 堆块 */
}

/* ---- 递归 ref 判定（见 type.h） ---- */

bool type_contains_ref(const type_t *t) {
    if (!t) return false;
    if (type_is_ref(t)) return true; /* 顶层 ref *T / ref []T */
    switch (t->kind) {
        case TYPE_KIND_STRUCT: {
            const struct_type_t *st = (const struct_type_t *)t;
            for (size_t i = 0; i < st->field_count; i++)
                if (type_contains_ref(st->fields[i].type)) return true;
            return false;
        }
        case TYPE_KIND_TUPLE: {
            const tuple_type_t *tt = (const tuple_type_t *)t;
            for (size_t i = 0; i < tt->elem_count; i++)
                if (type_contains_ref(tt->elems[i].type)) return true;
            return false;
        }
        case TYPE_KIND_UNION: {
            const union_type_t *ut = (const union_type_t *)t;
            for (size_t i = 0; i < ut->member_count; i++)
                if (type_contains_ref(ut->members[i].payload_type)) return true;
            return false;
        }
        case TYPE_KIND_CUNION: {
            const cunion_type_t *ct = (const cunion_type_t *)t;
            for (size_t i = 0; i < ct->member_count; i++)
                if (type_contains_ref(ct->members[i].type)) return true;
            return false;
        }
        case TYPE_KIND_OPTION:
            return type_contains_ref(((const option_type_t *)t)->inner);
        case TYPE_KIND_CONST:
        case TYPE_KIND_VOLATILE:
            return type_contains_ref(type_qualifier_sub(t));
        case TYPE_KIND_ARRAY:
            return type_contains_ref(((const array_type_t *)t)->elem_type);
        default:
            return false; /* 标量/str/own/fatal 指针/type/error/opaque/enum/func */
    }
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

/* 内置类型单例：零初始化（静态存储期），全部字段（vtable/name/size/align/
   kind/sealed/id）由 vm_init_builtins 逐一填充。
   此前用位置式初始化逐个列字段，新增字段（如 `id`）后必然漏初始化并触发
   -Wmissing-field-initializers——这里改为依赖零初始化，避免同类漂移。 */
static type_t g_type_i8;
static type_t g_type_i16;
static type_t g_type_i32;
static type_t g_type_i64;
static type_t g_type_u8;
static type_t g_type_u16;
static type_t g_type_u32;
static type_t g_type_u64;
static type_t g_type_f32;
static type_t g_type_f64;
static type_t g_type_bool;
static type_t g_type_str;
static type_t g_type_void;
static type_t g_type_type;
/* func 基类声明为 func_type_t 布局：无签名（sig 全零），向下转型安全 */
static func_type_t g_type_func;
static type_t g_type_error;
static type_t g_type_interrupt;
static type_t g_type_opaque;
static type_t g_type_module;

void vm_init_builtins(vm_t *vm) {
    static const char S_I8[]  = "i8",   S_I16[] = "i16", S_I32[] = "i32", S_I64[] = "i64";
    static const char S_U8[]  = "u8",   S_U16[] = "u16", S_U32[] = "u32", S_U64[] = "u64";
    static const char S_F32[] = "f32",  S_F64[] = "f64";
    static const char S_BOOL[] = "bool", S_STR[] = "str";
    static const char S_VOID[] = "void", S_TYPE[] = "type", S_FUNC[] = "func";
    static const char S_ERROR[] = "error";
    static const char S_INTERRUPT[] = "interrupt";
    static const char S_OPAQUE[] = "opaque";
    static const char S_MODULE[] = "module";

    /* 函数签名类型池（type_func_sig intern 用）；元素由 vm_destroy 手动释放，
       vec 只持有指针数组（与 scope owned 同一模式） */
    vm->sig_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 数组类型池（type_array_intern intern 用）；元素由 vm_destroy 手动释放 */
    vm->array_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 内置基础类型创建即 sealed（无开放构造阶段，永不重新 intern）。
       id 按 0..14 固定（vm_register_builtin_types 与 LOAD_TYPE 内建段一致）。
       ownership = OWN_NONE（内建标量无所有权修饰）。 */
    g_type_i8   = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I8),  sizeof(int8_t),   alignof(int8_t),   TYPE_KIND_INT,  OWN_NONE, true,  0 };
    g_type_i16  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I16), sizeof(int16_t),  alignof(int16_t),  TYPE_KIND_INT,  OWN_NONE, true,  1 };
    g_type_i32  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I32), sizeof(int32_t),  alignof(int32_t),  TYPE_KIND_INT,  OWN_NONE, true,  2 };
    g_type_i64  = (type_t){ &VTABLE_INT_SIGNED,  STRSLICE_LIT(S_I64), sizeof(int64_t),  alignof(int64_t),  TYPE_KIND_INT,  OWN_NONE, true,  3 };
    g_type_u8   = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U8),  sizeof(uint8_t),   alignof(uint8_t),  TYPE_KIND_INT,  OWN_NONE, true,  4 };
    g_type_u16  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U16), sizeof(uint16_t),  alignof(uint16_t), TYPE_KIND_INT,  OWN_NONE, true,  5 };
    g_type_u32  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U32), sizeof(uint32_t),  alignof(uint32_t), TYPE_KIND_INT,  OWN_NONE, true,  6 };
    g_type_u64  = (type_t){ &VTABLE_INT_UNSIGNED, STRSLICE_LIT(S_U64), sizeof(uint64_t),  alignof(uint64_t), TYPE_KIND_INT,  OWN_NONE, true,  7 };
    g_type_f32  = (type_t){ &VTABLE_FLOAT, STRSLICE_LIT(S_F32), sizeof(float),    alignof(float),    TYPE_KIND_FLOAT, OWN_NONE, true,  8 };
    g_type_f64  = (type_t){ &VTABLE_FLOAT, STRSLICE_LIT(S_F64), sizeof(double),   alignof(double),   TYPE_KIND_FLOAT, OWN_NONE, true,  9 };
    g_type_bool = (type_t){ &VTABLE_BOOL, STRSLICE_LIT(S_BOOL), sizeof(bool),     alignof(bool),     TYPE_KIND_BOOL, OWN_NONE, true, 10 };
    g_type_str  = (type_t){ &VTABLE_STR,  STRSLICE_LIT(S_STR),  sizeof(const char *), alignof(const char *), TYPE_KIND_STR,  OWN_NONE, true, 11 };
    g_type_void = (type_t){ &VTABLE_VOID, STRSLICE_LIT(S_VOID), 0, 1, TYPE_KIND_VOID, OWN_NONE, true, 12 };
    g_type_type = (type_t){ &VTABLE_TYPE, STRSLICE_LIT(S_TYPE), sizeof(const type_t*), alignof(const type_t*), TYPE_KIND_TYPE, OWN_NONE, true, 13 };
    g_type_func.base = (type_t){ &VTABLE_FUNC, STRSLICE_LIT(S_FUNC), sizeof(func_t*), alignof(func_t*), TYPE_KIND_FUNC, OWN_NONE, true, 14 };

    /* error_data_t 内联在 value data 块中 */
    g_type_error = (type_t){ &VTABLE_ERROR, STRSLICE_LIT(S_ERROR),
                             sizeof(error_data_t), alignof(error_data_t),
                             TYPE_KIND_ERROR, OWN_NONE, true, 15 };

    /* interrupt_data_t 内联在 value data 块中（引擎级控制流哨兵） */
    g_type_interrupt = (type_t){ &VTABLE_INTERRUPT, STRSLICE_LIT(S_INTERRUPT),
                                 sizeof(interrupt_data_t), alignof(interrupt_data_t),
                                 TYPE_KIND_INTERRUPT, OWN_NONE, true, 16 };

    /* opaque（≈ C void*，m3-design §8.4）：指针值 = 裸指针。内建单例，
       无开放构造阶段（与 error/interrupt 同族，type_seal 槽位 NULL）。 */
    g_type_opaque = (type_t){ &VTABLE_OPAQUE, STRSLICE_LIT(S_OPAQUE),
                              sizeof(void *), alignof(void *),
                              TYPE_KIND_OPAQUE, OWN_NONE, true, 17 };

    /* module（M5）：模块命名空间值。data = module_t*（借用，不拥有）。
     * 不可实例化/赋值/传递——仅由 IMPORT 指令内部构造。内建单例，
     * 无开放构造阶段。不登记进 global scope（模块类型不是用户可引用的类型名）。 */
    g_type_module = (type_t){ &VTABLE_MODULE, STRSLICE_LIT(S_MODULE),
                              sizeof(void *), alignof(void *),
                              TYPE_KIND_MODULE, OWN_NONE, true, 18 };

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
    vm->type_opaque = &g_type_opaque;
    vm->type_module = &g_type_module;
}

/* 取 fatal 变体：**唯一权威定义**（sema 与运行期共用，见 type.h 的说明）。
   各形态的变体必须由**同一套**参数拼出，否则会 intern 出同布局不同身份的实例。 */
const type_t *type_fatal_variant_of(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (t->ownership == OWN_FATAL) return t;        /* 幂等 */

    /* 指针 / 切片：顶层所有权升级。**必须在下面那句"其它所有权原样"之前**判——
       `own *T` / `own []T` 本身就是 OWN_OWN，先被那句挡掉就永远升不上 fatal，
       于是 FFI 的 `fatal *T` 形参收 `own *T` 实参会被误拒。
       `ref` 指针/切片**不持有资源**，按借用语义不参与升级（原样返回）。 */
    if (type_is_slice(t) || type_is_ptr(t)) {
        if (t->ownership == OWN_OWN) {
            return type_is_slice(t)
                       ? type_slice_intern(vm, OWN_FATAL, slice_type_elem(t))
                       : type_ptr_intern(vm, OWN_FATAL, ptr_type_base(t));
        }
        return t;
    }
    if (t->ownership != OWN_NONE) return t;         /* 其它所有权：原样 */
    if (!type_owns_resources(t)) return t;          /* 无资源：fatal 无意义 */

    switch (t->kind) {
    case TYPE_KIND_STRUCT:
        return type_struct_intern_with_ownership(vm, t, OWN_FATAL);
    case TYPE_KIND_TUPLE: {
        size_t n = tuple_type_elem_count(t);
        tuple_elem_t elems[n > 0 ? n : 1];
        for (size_t i = 0; i < n; i++) {
            const tuple_elem_t *e = tuple_type_elem(t, i);
            elems[i].type = e ? e->type : NULL;
            elems[i].offset = 0; /* seal 重算 */
        }
        return type_tuple_intern_with_ownership(vm, n > 0 ? elems : NULL, n,
                                                OWN_FATAL);
    }
    case TYPE_KIND_ARRAY:
        return type_array_intern_with_ownership(vm, array_type_elem(t),
                                                array_type_len(t), OWN_FATAL);
    case TYPE_KIND_OPTION:
        return type_option_intern_with_ownership(vm, type_option_inner(t),
                                                 OWN_FATAL);
    case TYPE_KIND_UNION: {
        size_t n = union_type_member_count(t);
        union_member_t members[n > 0 ? n : 1];
        for (size_t i = 0; i < n; i++) {
            const union_member_t *m = union_type_member(t, i);
            members[i].name = m ? m->name : STRSLICE_EMPTY;
            members[i].tag = m ? m->tag : (uint32_t)i;
            members[i].payload_type = m ? m->payload_type : NULL;
        }
        return type_union_intern_with_ownership(vm, n > 0 ? members : NULL, n,
                                                OWN_FATAL);
    }
    default:
        return t; /* 无该变体的形态 */
    }
}
