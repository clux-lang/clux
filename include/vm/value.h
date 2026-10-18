#ifndef _H_CLUX_VM_VALUE_
#define _H_CLUX_VM_VALUE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/type_interrupt.h"
#include "vm/scope_frame.h"
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
 * 每个 value 持有指向所属作用域 frame 的指针（借用 &scope->frame，见
 * scope_frame.h）：value_scope() 可还原所属 scope，供所有权分析判定
 * 作用域逃逸。untracked 构造（value_alloc_shell）无 vm 上下文，
 * frame 为 NULL（引擎内部对象不属用户作用域）。
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

/** 返回 value 所属作用域的 frame（NULL = 无归属，如 untracked 引擎对象） */
const scope_frame_t *value_frame(const value_t *v);

/** 从 value 还原所属 vm scope（container_of 反推；frame 为 NULL 时返回 NULL） */
scope_t *value_scope(const value_t *v);

/**
 * 获取 value 的所有权状态（值层编码）。
 *
 * **仅 sema 编译期使用**——运行期代码不应读取此字段。运行期的"采用 vs 拷贝"
 * 判据统一走 value_is_borrowed()（OWN_REF = 借用），不直接判 ownership。
 * OWN_FATAL 是 sema 的将亡值标记，运行期永远不产生也不读取。
 */
ownership_t value_ownership(const value_t *v);

/**
 * 设置 value 的所有权状态（**仅 sema 编译期使用**）。
 *
 * 运行期代码不应调用此函数。sema 在 shadow value 上设置所有权状态以驱动
 * 编译期的所有权检查（move/clone/return 拦截、fatal 消费点判定等）。
 */
void value_set_ownership(value_t *v, ownership_t own);

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
 * 是否是**真实的 type value**：值为元类型 `type` **且** data 非空（data 指向
 * `type_t`，可用 `value_as(v, const type_t *)` 取）。
 *
 * 与 `value_is_type(v, TYPE_KIND_TYPE)` 的区别：后者只说明"该表达式的**类型**
 * 是 `type`"，对 **shadow**（data == NULL，sema 的类型占位值）同样成立——此时
 * 解引用 data 就是空指针解引用。需要类型实体时一律用本函数判定。
 *
 * （`var t = i32;` 这类"类型绑进变量"已由 sema 拒绝，本判据用于防御其它
 * shadow 路径：它们只应被当作"类型是 type 的普通表达式"处理。）
 */
static inline bool value_is_type_value(const value_t *v) {
  return value_is_type(v, TYPE_KIND_TYPE) && value_data(v) != NULL;
}

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
value_t *value_alloc_shell(allocator_t *alloc, const type_t *type, void *data);

/** 构造 shadow value：只携带类型信息，data=NULL，用于语义分析阶段类型计算。
 * is_shadow=true 的 value 参与运算时只做类型计算不操作实际数据。
 * 自动 track 到 vm->current_scope。 */
value_t *value_make_shadow(vm_t *vm, const type_t *type);

/** 判断 value 是否为 shadow（只有类型，无实际数据） */
bool value_is_shadow(const value_t *v);

/** 构造借用引用：data 指向父值（数组/struct）内部元素槽位（value_t**），
 * ownership=OWN_REF。借用值只匿名存活于表达式链；绑定/返回时经 value_clone
 * materialize 成独立深拷贝（ownership=OWN_OWN）。自动 track 到 vm->current_scope。 */
value_t *value_make_borrowed(vm_t *vm, const type_t *type, void *slot);

/** 判断 value 是否为借用引用（非 shadow 且不拥有 data）。
 *
 * 这也是**唯一的"采用 vs 拷贝"判据**：变量读取（op_push）压入借用视图，
 * 因此 `scope_define` 见到非借用值就只可能是表达式刚创建的临时值 → 零拷贝
 * 采用；见到借用视图（或属于其他作用域的值）→ materialize 成独立深拷贝。
 * 不再需要"该值是否已绑定到名字"的附加标记（2026-10 所有权重构删除）。 */
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

/** 按**字节数**分配一块原始内存（清零；value_data 类，FREE 可直接释放）。
 *  用于 `NEW`：`new` 只负责"申请内存"（C 后端即 `malloc(size)`），构造由
 *  `CONSTRUCT <dest=该句柄>` 完成——两者分离，逐字段赋值与 C 一一对应。 */
void *value_alloc_raw(allocator_t *alloc, size_t size);

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

/**
 * fatal 变体 → 同布局的非 fatal 类型的**接管转换**（`fatal T → T`，含加限定的
 * `fatal T → const T`）：memcpy 顶层后**清空源**的 own/fatal 指针（`ptr_clear_
 * owned_recursive`），所有权随构造转移给新值。不适用（返回 NULL）时调用方走各自
 * 的常规路径：
 *   - 源不是 fatal、或目标也是 fatal；
 *   - 目标不是"同 kind + 同 size + 布局兼容"（array↔tuple 的布局兼容互转由各
 *     自的 vtable 处理，本函数只认同 kind 的 fatal 变体，`cand->kind != target->kind`
 *     即拒）。
 *
 * 为什么必需：fatal T 与 T 布局逐字节相同，而各复合类型原有的 implicit_cast 都是
 * **整块 memcpy** —— 副本会与源**共享内嵌 own 堆块**，两个值各自递归销毁同一堆块
 * = 双释放（实测 access violation）。`cfg->fatal_cast` 指向本类型的布局兼容判定
 * （struct：struct_type_compatible / tuple：tuple_type_compatible / array：同一
 * elem+len / union：union 成员表一致）。
 *
 * shadow 输入返回目标 shadow（semantics 阶段不产生副作用）。
 */
value_t *value_take_fatal_into(vm_t *vm, value_t *v, const type_t *target,
                               bool (*cfg)(vm_t *, const type_t *,
                                           const type_t *));

/** 深拷贝 value 并自动注册到 vm->current_scope（scope 管理生命周期） */
value_t *value_clone(vm_t *vm, value_t *v);

/**
 * 所有权**转移**（m3-design §7）：把 v 的资源无损交给调用方，返回接管方持有
 * 的新值（**fatal 变体**类型），并清空 v 内部的 own/fatal 指针使其不再持有
 * 任何资源（作用域退出时不会与接管方双释放）。
 *
 * 与 value_clone 的分工：
 *   - value_clone = 复制（源保持有效且独立），用于 `clone(x)` 与"读变量的值
 *     到新变量"这类需要保留源的场合；
 *   - value_move  = 转移（源被掏空），用于 `move(x)` 与**跨函数边界的所有权
 *     转移**（fatal 参数接收、fatal 返回值接收）——那里类型与目标完全一致，
 *     框架会走 clone 路径，必须显式改走转移，否则含 own 字段的复合值会被
 *     深拷贝一份（白分配），且调用方仍持有原件（语义上不是 move）。
 *
 * shadow → 同类型 shadow（不产生副作用）。
 */
value_t *value_move(vm_t *vm, value_t *v);

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
