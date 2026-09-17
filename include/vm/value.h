#ifndef _H_CLUX_VM_VALUE_
#define _H_CLUX_VM_VALUE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/type_interrupt.h"
#include "core/allocator.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * value_t: 万物皆值（不透明类型）
 *
 * value_t 始终在堆上分配，由 scope 持有生命周期。
 * 所有 value_make / value_clone / value_implicit_cast / value_explicit_cast
 * 创建的 value 会自动 track 到 vm->current_scope。
 *
 * 外部代码通过 value_type() / value_data() 访问内部字段，
 * 不直接访问 struct value_t 成员。
 */
typedef struct value_t value_t;

/* ---- 访问器 ---- */

/** 获取 value 的类型 */
const type_t *value_type(const value_t *v);

/** 获取 value 的 data 指针 */
void *value_data(const value_t *v);

/** 便捷数据访问宏 */
#define value_as(v, T) (*((T *)(value_data(v))))

/** 判断 value 是否为 void（type==NULL 表示无类型） */
bool value_is_void(const value_t *v);

/** 判断 value 是否为 error（引擎级硬错误） */
bool value_is_error(vm_t *vm, const value_t *v);

/** 判断 value 是否为 interrupt（引擎级控制流哨兵） */
bool value_is_interrupt(vm_t *vm, const value_t *v);

/* ---- 类型查询（type is value：kind 分类 + vtable 分派） ---- */

/** 返回 value 类型的粗粒度 kind（type 为 NULL 时返回 TYPE_KIND_VOID） */
type_kind_t value_kind(const value_t *v);

/** value 的类型是否为指定 kind（NULL type → false，除非 kind 是 VOID） */
bool value_is_type(const value_t *v, type_kind_t kind);

/**
 * value 的类型是否含 const 修饰（沿 sub 链递归）：const i32 → true；
 * volatile(const(i32)) → true；volatile(i32) → false。
 * sema 仅在延迟初始化（TDZ 首次赋值豁免）时使用；赋值拦截下沉 vtable。
 */
bool value_has_const(const value_t *v);

/* ---- 构造器 ---- */

/** 从已有 data 指针构造 value（堆分配 + auto-track 到 current_scope）。
 * 接管 data 所有权。 */
value_t *value_make(vm_t *vm, const type_t *type, void *data);

/** 从已有 data 指针构造 value（堆分配，不 auto-track）。
 * 仅供需要手动管理生命周期时使用。 */
value_t *value_make_untracked(allocator_t *alloc, const type_t *type, void *data);

/** 构造 shadow value：只携带类型信息，data=NULL，用于语义分析阶段类型计算。
 * is_shadow=true 的 value 参与运算时只做类型计算不操作实际数据。
 * 自动 track 到 vm->current_scope。 */
value_t *value_make_shadow(vm_t *vm, const type_t *type);

/** 判断 value 是否为 shadow（只有类型，无实际数据） */
bool value_is_shadow(const value_t *v);

/** 构造借用引用：data 指向父值（数组/struct）内部元素槽位（value_t**），
 * is_own=false。借用值只匿名存活于表达式链；绑定/返回时经 value_clone
 * materialize 成独立深拷贝（is_own=true）。自动 track 到 vm->current_scope。 */
value_t *value_make_borrowed(vm_t *vm, const type_t *type, void *slot);

/** 判断 value 是否为借用引用（非 shadow 且不拥有 data） */
bool value_is_borrowed(const value_t *v);

/* ---- error 构造 ---- */

/** 创建 error value（不带位置信息），message 为 C 字符串 */
value_t *value_make_error(vm_t *vm, const char *message);

/** 创建 error value（带位置信息） */
value_t *value_make_error_loc(vm_t *vm, const char *message, const char *location);

/* ---- undefined 构造 ---- */

/** 创建 undefined value（void 类型，data=NULL，标记"类型待推导"） */
value_t *value_make_undefined(vm_t *vm);

/** 判断 value 是否为 undefined（void 类型 value） */
bool value_is_undefined(vm_t *vm, const value_t *v);

/* ---- nil 构造 ---- */

/** 创建 nil value（type_nil 类型，data 为 func_t* 宽度的零块 = NULL 指针）。
 *  nil 表示函数 0 初始化（未来空指针）；nil 是字面量值，不是类型名。 */
value_t *value_make_nil(vm_t *vm);

/** 判断 value 是否为 nil（type_nil 类型 value） */
bool value_is_nil(vm_t *vm, const value_t *v);

/* ---- interrupt 构造 ---- */

/** 创建 interrupt value（引擎级控制流哨兵，data 内联 interrupt_data_t） */
value_t *value_make_interrupt(vm_t *vm, interrupt_kind_t kind);

/** 读取 interrupt value 的 kind（非 interrupt value 时返回 INTERRUPT_RETURN） */
interrupt_kind_t value_interrupt_kind(vm_t *vm, const value_t *v);

/* ---- 内存分配辅助 ---- */

/** 通过 allocator 分配一个 value_t（零初始化），用于 scope 存储 */
value_t *value_alloc(allocator_t *alloc);

/** 按 type->size/align 分配数据块，返回 data 指针 */
void *value_alloc_data(allocator_t *alloc, const type_t *type);

/** 分配数据块并用源数据初始化（memcpy） */
void *value_alloc_data_copy(allocator_t *alloc, const type_t *type, const void *src);

/* ---- 运算分派: a + b => a.type->vtable->add(vm, a, b) ---- */

value_t *value_add(vm_t *vm, value_t *a, value_t *b);
value_t *value_sub(vm_t *vm, value_t *a, value_t *b);
value_t *value_mul(vm_t *vm, value_t *a, value_t *b);
value_t *value_div(vm_t *vm, value_t *a, value_t *b);
value_t *value_mod(vm_t *vm, value_t *a, value_t *b);
value_t *value_neg(vm_t *vm, value_t *a);

value_t *value_eq(vm_t *vm, value_t *a, value_t *b);
value_t *value_ne(vm_t *vm, value_t *a, value_t *b);
value_t *value_lt(vm_t *vm, value_t *a, value_t *b);
value_t *value_le(vm_t *vm, value_t *a, value_t *b);
value_t *value_gt(vm_t *vm, value_t *a, value_t *b);
value_t *value_ge(vm_t *vm, value_t *a, value_t *b);

value_t *value_band(vm_t *vm, value_t *a, value_t *b);
value_t *value_bor(vm_t *vm, value_t *a, value_t *b);
value_t *value_bxor(vm_t *vm, value_t *a, value_t *b);
value_t *value_bnot(vm_t *vm, value_t *a);
value_t *value_shl(vm_t *vm, value_t *a, value_t *b);
value_t *value_shr(vm_t *vm, value_t *a, value_t *b);

value_t *value_lnot(vm_t *vm, value_t *a);

value_t *value_call(vm_t *vm, value_t *callee, value_t **args, size_t argc);

/* ---- 索引 / 容器运算（分派到 vtable 的 get_index / set_index / length） ---- */

/** self[index]：index 为运行时 value（整数），返回元素副本或 error */
value_t *value_get_index(vm_t *vm, value_t *self, value_t *index);

/** self[index] = val：下标写入，返回 self 或 error */
value_t *value_set_index(vm_t *vm, value_t *self, value_t *index, value_t *val);

/** 长度查询：返回表示元素个数的整数 value（i64） */
value_t *value_length(vm_t *vm, value_t *self);

/* ---- 生命周期 ---- */

/** 销毁 value 的堆载荷（dispose data，不释放 value_t 结构体） */
void value_dispose(vm_t *vm, value_t *v);

/** 深拷贝 value 并自动注册到 vm->current_scope（scope 管理生命周期） */
value_t *value_clone(vm_t *vm, value_t *v);

/** 原地赋值：将 src 的数据写入 dst（类型必须匹配），返回 dst 或 error */
value_t *value_assign(vm_t *vm, value_t *dst, value_t *src);

/* ---- 类型运算（鸭子类型，type value 的 == / extends） ---- */

/**
 * 密封一个 type value：代理到 value->data 指向的 type_t 的 vtable->type_seal。
 * 仅对 type value（value_type == vm->type_type）有意义；非 type value 原样返回。
 * 若 type_seal 因去重复用已有缓存类型，则把当前 value 的 data（指向被回收的旧
 * type）重定向到缓存 type，并扫描操作数栈把所有引用旧 type 的 type value 一并
 * 重定向，避免悬空指针。返回密封后的 type value（即入参 src，data 可能已更新）。
 */
const value_t *value_seal(vm_t *vm, value_t *src);

/** type value 的 == 代理：比较两个类型值是否鸭子类型相等（type_equal） */
value_t *value_type_eq(vm_t *vm, value_t *a, value_t *b);

/** type value 的 extends：sub 类型值是否兼容 sup 类型值（type_extends） */
value_t *value_type_extends(vm_t *vm, value_t *a, value_t *b);

/** extends 运算符入口：分派 a->type->vtable->extends（type value 专用） */
value_t *value_extends(vm_t *vm, value_t *a, value_t *b);

/* ---- const/volatile 解包原语（type_const.c/type_volatile.c 代理用） ---- */

/**
 * 临时替换 value 的 type 指针（限定类型解包代理用）。返回旧 type。
 * 调用方须在 sub vtable 槽位返回后恢复（value_restore_type）。
 * 仅限 vm 内部类型实现使用。
 */
const type_t *value_swap_type(value_t *v, const type_t *new_type);

/** 恢复 value 的 type 指针（与 value_swap_type 配对） */
void value_restore_type(value_t *v, const type_t *old_type);

/* ---- 类型转换 ---- */

/**
 * 加限定符（qualify）身份转换：源无限定符、目标带 const/volatile 且限定符
 * sub 链剥到源类型（如 i32 → volatile i32、i32 → const volatile i32）。
 * 限定符只影响存取语义/赋值规则，不改变底层表示——身份拷贝（与
 * type_const.c / type_volatile.c 中"脱限定符 const T → T / volatile T → T
 * 身份拷贝"对称）。仅在标量/值类型 vtable 的 implicit_cast 中调用（指针的
 * const 语义不同，指针 vtable 不调用此 helper）。返回新 value 或 error。
 */
value_t *value_implicit_qualify(vm_t *vm, value_t *v, const type_t *target);

value_t *value_implicit_cast(vm_t *vm, value_t *v, const type_t *target);
value_t *value_explicit_cast(vm_t *vm, value_t *v, const type_t *target);

/* ---- vtable 实现辅助宏 ---- */

/*
 * VTABLE_BINARY: vtable 二元运算的标准前置流程
 *
 * 1. error 传播（短路）
 * 2. 同类型 → fall through，调用方做运算
 * 3. 不同类型：
 *    a. 两者均为数值类型 → promote → implicit_cast 两边 → re-dispatch
 *    b. 非数值类型 → 尝试右值 implicit_cast 到左值类型（右值兼容左值）
 *
 * 用法:
 *   static value_t *int_add(vm_t *vm, value_t *a, value_t *b) {
 *       VTABLE_BINARY(vm, a, b, add, "+");
 *       // 此时 value_type(a) == value_type(b)，做运算
 *       ...
 *   }
 */
#define VTABLE_BINARY(vm, a, b, slot, op_sym)                                \
    do {                                                                      \
        if (value_is_error((vm), (a))) return (a);                           \
        if (value_is_error((vm), (b))) return (b);                           \
        if (value_type((a)) != value_type((b))) {                            \
            const type_t *_rt = type_promote((vm), value_type((a)), value_type((b))); \
            if (_rt) {                                                        \
                value_t *_a2 = value_implicit_cast((vm), (a), _rt);          \
                if (value_is_error((vm), _a2)) return _a2;                   \
                value_t *_b2 = value_implicit_cast((vm), (b), _rt);          \
                if (value_is_error((vm), _b2)) return _b2;                   \
                return _rt->vtable->slot((vm), _a2, _b2);                    \
            }                                                                 \
            value_t *_b2 = value_implicit_cast((vm), (b), value_type((a)));  \
            if (value_is_error((vm), _b2)) return _b2;                       \
            return value_type((a))->vtable->slot((vm), (a), _b2);            \
        }                                                                     \
    } while (0)

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VALUE_ */
