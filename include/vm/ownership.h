#ifndef _H_CLUX_VM_OWNERSHIP_
#define _H_CLUX_VM_OWNERSHIP_

/**
 * ownership_t: 所有权修饰（type_t.ownership）
 *
 * 正交于 type_kind_t 的所有权属性。指针（PTR）和切片（SLICE）支持三种
 * 修饰；其他类型（struct/tuple/array/...）恒为 OWN_NONE，唯一例外是
 * OWN_FATAL——move/clone 产物的将亡值（fatal T）。
 *
 * 单独成文件的原因：`vtable_t` 的 `type_set_ownership` 槽位需要这个类型，而
 * vtable.h 被 type.h 先包含，两者的包含顺序不允许把枚举定义在 type.h 里；
 * 在 vtable.h 里写不完全枚举前置声明（`typedef enum ownership_t ownership_t;`）
 * 能编译但是 MSVC 扩展（-Wmicrosoft-enum-forward-reference）。因此把定义抽到
 * 这个无依赖的头文件，两边都 include 它。
 *
 *   OWN_NONE  — 无所有权修饰（标量/struct/tuple/... 的默认值）
 *   OWN_OWN   — 独占所有权（own *T / own []T，作用域退出自动销毁）
 *   OWN_REF   — 借用引用（ref *T / ref []T，无所有权，可 copy）
 *   OWN_FATAL — 将亡值（fatal *T / fatal []T / fatal T，move/clone 产物；
 *               禁止命名，只能作为临时值流被接管）
 */
typedef enum ownership_t {
    OWN_NONE  = 0,
    OWN_OWN,
    OWN_REF,
    OWN_FATAL,
} ownership_t;

#endif /* _H_CLUX_VM_OWNERSHIP_ */
