#ifndef _H_CLUX_VM_OWNERSHIP_
#define _H_CLUX_VM_OWNERSHIP_

/**
 * ownership_t: **类型修饰**词汇（own / ref / fatal）。
 *
 * 所有权是类型的一部分（docs/ownership-as-type.md）：`own T` / `ref T` /
 * `fatal T` 是 `TYPE_KIND_OWN/REF/FATAL` 的独立类型，参与相等、intern、反射
 * 与签名。值上**不再有**所有权状态（P3a，2026-10）；运行期类型已剥离修饰，
 * 只保留一个"借用视图"位（value_t.borrowed，回答 dispose 是否释放 data 块，
 * 不是所有权）。
 *
 * 枚举本身仍被使用，是因为所有权修饰在 sema / AST / 签名构建 / FFI 规则中
 * 作为**修饰词**传递（例如 `type_ownership_apply(vm, OWN_OWN, t)`）。
 *
 *   OWN_NONE  — 无修饰（裸类型 T）
 *   OWN_OWN   — 拥有资源：move-only，持有者销毁
 *   OWN_REF   — 借用：持有者不销毁
 *   OWN_FATAL — 将亡值：必须被消费一次（move 出去 / 绑定 / 显式销毁）
 *
 * 单独成文件的原因：value_t 的访问器与 vtable_t 都需要这个类型，而各自的
 * 头文件包含顺序不允许把枚举定义在 type.h 里；在 vtable.h 里写不完全枚举
 * 前置声明（`typedef enum ownership_t ownership_t;`）能编译但是 MSVC 扩展
 * （-Wmicrosoft-enum-forward-reference）。因此把定义抽到这个无依赖的头文件，
 * 各方都 include 它。
 */
typedef enum ownership_t {
    OWN_NONE  = 0,
    OWN_OWN,
    OWN_REF,
    OWN_FATAL,
} ownership_t;

#endif /* _H_CLUX_VM_OWNERSHIP_ */
