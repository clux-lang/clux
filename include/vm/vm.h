#ifndef _H_CLUX_VM_VM_
#define _H_CLUX_VM_VM_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/function.h"
#include "vm/bcode.h"
#include "core/allocator.h"
#include "core/vec.h"

/**
 * vm_t: 虚拟机上下文
 *
 * 聚合全局状态：当前 scope、全局 scope、调用栈、内置类型注册表。
 * 所有 value 操作需要 vm 作为上下文参数。
 */
typedef struct vm_t {
    allocator_t *alloc;

    /* 作用域 */
    scope_t     *global_scope;  /* 全局根作用域 */
    scope_t     *root_scope;    /* 当前模块作用域（global 的子作用域） */
    scope_t     *current_scope;

    /* ---- 内置类型单例 ---- */
    type_t *type_i8;
    type_t *type_i16;
    type_t *type_i32;
    type_t *type_i64;
    type_t *type_u8;
    type_t *type_u16;
    type_t *type_u32;
    type_t *type_u64;
    type_t *type_f32;
    type_t *type_f64;
    type_t *type_bool;
    type_t *type_str;
    type_t *type_void;
    type_t *type_type;   /* 元类型：type 的 type */
    type_t *type_func;   /* 函数类型基类（无签名） */
    type_t *type_error;  /* 错误类型（引擎级硬错误） */
    type_t *type_interrupt; /* interrupt 类型（引擎级控制流哨兵） */

    /* ---- 函数签名类型池（按签名去重 intern，vm 拥有生命周期） ---- */
    vec_t *sig_types;    /* func_type_t*，元素为签名类型（sig 非空） */

    /* ---- const/volatile 修饰类型池（按 sub 去重 intern，vm 拥有生命周期） ---- */
    vec_t *const_types;    /* const_type_t*，元素为 const 修饰类型 */
    vec_t *volatile_types; /* volatile_type_t*，元素为 volatile 修饰类型 */

    /* ---- optional 修饰类型池（按 inner 去重 intern，vm 拥有生命周期） ---- */
    vec_t *option_types;   /* option_type_t*，元素为 optional 修饰类型 */

    /* ---- 数组类型池（按 elem_type + length 去重 intern，vm 拥有生命周期） ---- */
    vec_t *array_types;    /* array_type_t*，元素为数组类型 */

    /* ---- 类型 id 表（id → type_t*，LOAD_TYPE <id> 查表压栈） ---- */
    /* 内建类型固定 id 0..14（vm_init_builtins 登记）；程序类型 id 由编译器
       分配（>=16），运行期 DEFINE_TYPE <id> 声明登记（SEAL 密封后幂等
       更新/去重重绑）。同一 intern 实例重复登记幂等（多 id 别名同一 type_t）。 */
    vec_t *types_by_id;    /* type_t*，索引即类型 id */

    /* ---- 函数对象池（func_t / bcode_function_t*，vm 统一持有生命周期） ---- */
    /* 所有函数值共享同一 func_t*（clone 浅拷贝指针），因此 func_t 不能由
     * 某个 value dispose 释放（double free）；归本池统一释放。 */
    vec_t *functions;    /* func_t*，元素为函数对象（不 owns，vm_destroy 手动释放） */

    /* ---- 函数 id 表（id → func_t*，BIND_FUNC <id> 运行期登记） ---- */
    /* 内建函数 id 0..(FUNC_ID_BUILTIN_COUNT-1) 由 func_new 自动分配（从 0 起
       递增，next_builtin_func_id 维护）；程序函数 id 由编译器分配（>=64），
       BIND_FUNC <id> 填充 fn->id 并登记进本表。 */
    vec_t    *functions_by_id;    /* func_t*，索引即函数 id */
    uint32_t  next_builtin_func_id; /* func_new 内建 id 分配计数器 */

    /* ---- 执行器状态（复用 vm 上下文，不另建 exec_t） ---- */
    vec_t       *stack;   /* 操作数栈：value_t* 借用引用（不拥有，归 scope） */
    bytecode_t  *bc;      /* 当前执行中的字节码模块（嵌套调用时切换） */
    size_t       pc;      /* 当前指令指针（code 流字节偏移） */
    bool         halted;  /* error 出现即停止 */

    /* ---- 编译期状态 ---- */
    bool         comptime; /* true = 强制编译期求值（comptime var/func 上下文；
                              类型表达式槽位自动置位）。false = 只检查类型。 */
} vm_t;

/** 创建 VM（初始化内置类型、全局作用域、调用栈） */
vm_t *vm_new(allocator_t *alloc);

/** 销毁 VM（释放所有资源） */
void vm_destroy(vm_t **vm);

/** 进入新作用域（current_scope 变为新的子作用域） */
void vm_push_scope(vm_t *vm);

/** 退出当前作用域（销毁该作用域所有 value，current_scope 回退到 parent） */
void vm_pop_scope(vm_t *vm);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VM_ */
