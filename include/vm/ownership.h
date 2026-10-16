#ifndef _H_CLUX_VM_OWNERSHIP_
#define _H_CLUX_VM_OWNERSHIP_

/**
 * ownership_t: 值层所有权状态（value_t.ownership）
 *
 * **所有权是值的状态，不是类型或指针的属性**（2026-10 重构）。
 * 每个 value_t 携带一个 ownership_t，表示该值在当前上下文中的所有权角色：
 *
 *   OWN_NONE  — 无所有权（shadow 值、引擎内部对象的默认值）
 *   OWN_OWN   — 拥有 data（dispose 释放 data 块 + 递归释放内嵌 own 资源）
 *   OWN_REF   — 借用引用（data 指向父值内部，dispose 跳过释放）
 *   OWN_FATAL — 将亡值（sema 编译期状态：move/clone 产物，禁止命名，
 *               只能作为临时值流被接管；运行期永远不产生也不读取）
 *
 * **运行期约束**：exec/scope/value 等运行期代码只通过 value_is_borrowed()
 * 间接区分 OWN_REF（借用）vs 非借用，不直接读写 ownership 字段。OWN_FATAL
 * 仅供 sema 编译期使用。
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
