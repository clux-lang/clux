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
#include "vm/str_pool.h"
#include "vm/module.h"
#include "core/allocator.h"
#include "core/vec.h"
#include "core/strmap.h"

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
    type_t *type_opaque; /* opaque 类型（≈ C void*，id 17，M3 §8.4） */
    type_t *type_module; /* 模块类型（M5，命名空间值，不可实例化/赋值/传递） */
    type_t *type_usize;  /* usize：**独立**的无符号整数类型（id 18，宽度 = 指针） */

    /* ---- 函数签名类型池（按签名去重 intern，vm 拥有生命周期） ---- */
    vec_t *sig_types;    /* func_type_t*，元素为签名类型（sig 非空） */

    /* ---- const/volatile 修饰类型池（按 sub 去重 intern，vm 拥有生命周期） ---- */
    vec_t *const_types;    /* const_type_t*，元素为 const 修饰类型 */
    vec_t *volatile_types; /* volatile_type_t*，元素为 volatile 修饰类型 */
    vec_t *ownership_types; /* ownership_type_t*：own/ref/fatal 修饰类型
                              （三种 kind 同池，按 (kind, sub) 去重） */

    /* ---- optional 修饰类型池（按 inner 去重 intern，vm 拥有生命周期） ---- */
    vec_t *option_types;   /* option_type_t*，元素为 optional 修饰类型 */

    /* ---- 指针类型池（按 base_type + 所有权 kind 去重 intern，vm 拥有生命周期） ---- */
    vec_t *ptr_types;      /* ptr_type_t*，元素为 own/ref/fatal 指针类型 */

    /* ---- 数组类型池（按 elem_type + length 去重 intern，vm 拥有生命周期） ---- */
    vec_t *array_types;    /* array_type_t*，元素为数组类型 */

    /* ---- 枚举类型池（按 underlying + variant 表去重 intern，vm 拥有生命周期） ---- */
    vec_t *enum_types;     /* enum_type_t*，元素为枚举类型 */

    /* ---- 结构体类型池（按字段名+类型+顺序去重 intern，vm 拥有生命周期） ---- */
    vec_t *struct_types;   /* struct_type_t*，元素为结构体类型 */

    /* ---- 元组类型池（按元素类型+顺序去重 intern，vm 拥有生命周期） ---- */
    vec_t *tuple_types;    /* tuple_type_t*，元素为元组类型 */

    /* ---- tag union 类型池（按 member 表内容去重 intern，vm 拥有生命周期） ---- */
    vec_t *union_types;    /* union_type_t*，元素为 tag union 类型 */

    /* ---- C 语义 union 类型池（按 member 表内容去重 intern，vm 拥有生命周期） ---- */
    vec_t *cunion_types;   /* cunion_type_t*，元素为 C 语义 union 类型 */

    /* ---- 切片类型池（按 elem_type + 所有权 kind 去重 intern，vm 拥有生命周期） ---- */
    vec_t *slice_types;    /* slice_type_t*，元素为 own/ref/fatal 切片类型 */

    /* ---- 类型 id 表（id → type_t*，LOAD_TYPE <id> 查表压栈） ---- */
    /* 内建类型固定 id 0..14（vm_init_builtins 登记）；**程序类型 id 由本 VM
       全局分配**（type_id_alloc，从 TYPE_ID_PROGRAM_BASE 起递增），运行期
       DEFINE_TYPE <id> 声明登记（SEAL 密封后幂等更新/去重重绑）。
       同一 intern 实例重复登记幂等（多 id 别名同一 type_t）。

       ⚠️ id 必须**全 VM 唯一**，不能按模块各从 64 起：本表是 VM 单表（多模块
       共享同一 vm），且模块字节码里的 LOAD_TYPE <id> 直接索引本表。若两个模块
       各自从 64 开始分配，后编译模块的 DEFINE_TYPE 会覆盖先者的槽位，导致先者
       函数体里的 LOAD_TYPE 取到**别的模块的类型**（曾实测：被导入模块内的
       struct 构造运行期报 "construct: unsupported type"）。 */
    vec_t *types_by_id;    /* type_t*，索引即类型 id */
    uint32_t type_id_next; /* 程序类型 id 分配计数器（type_id_alloc 维护） */

    /* 注：原 `fatal_bases`（fatal 变体 → base 的派生关系登记表）已随所有权重构
       删除——fatal 不再是独立的类型变体，而是 value/symbol 层的 ownership 状态，
       所以"变体身份折回"这个问题整体不存在了（`type_fatal_base_of` 现在是恒等）。
       详见 docs/ownership-refactor.md。 */

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

    /* ---- 字符串池（str 生命周期托管，vm_str_intern 去重 intern） ---- */
    vec_t       *strs;     /* 池内字符串块（char*，含 NUL 终止符；不 owns，vm_destroy 手动释放） */

    /* ---- 模块表（M5：规范路径 → module_t*，IMPORT <path> 运行期查表）----
     * 每个模块独立走完整流水线（lex→parse→sema→compile），产出 module_t
     * （bc + global_scope + exports）。vm 拥有 module 生命周期。
     * IMPORT <path> 查此表获取 module_t*，构造模块值压栈。 */
    strmap_t    *modules;  /* 规范路径(C字符串) → module_t*（vm 拥有生命周期） */
} vm_t;

/* ---- `usize`：C 的 size_t 在 clux 里的名字（**独立类型**，宽度随架构）----
 *
 * `usize` **不是** `u64`/`u32` 的别名，而是自己的 `type_t`（id 18，NAME 即
 * "usize"，kind INT、无符号、size/align = 指针宽度）。它等价于 C 的 `size_t`
 * 这个**语义角色**，但在类型系统里是独立身份：
 *   - `typeof(1usize) != typeof(1u64)`、`usize` 与 `u64` 的 type value 不相等
 *     （`usize extends u64` 为假），错误信息里也显示 `usize`；
 *   - **与同宽无符号类型隐式互转**（64 位 usize ↔ u64，32 位 usize ↔ u32）：
 *     同宽同符号 = 表示完全相同，无信息损失，因此按 C 的 size_t 习惯仍可
 *     直接混用；变宽时按现有宽度规则（拓宽隐式、缩窄显式）；
 *   - FFI 里声明 `usize` 即 C 的 `size_t`，有自己的描述符码
 *     （`BCODE_FFI_DATA_USIZE` / `BCODE_FFI_RET_USIZE`）；
 *   - 大小/长度类内建（`len`/`sizeof`/`alignof`/`[N]` 下标/for_index 循环变量）
 *     一律用它，代码才真正可移植（u64 在 32 位平台上宽度是错的）。
 * 名字注册见 `vm_register_builtin_types`（vm.c），类型实体见 `vm_init_builtins`
 * （type.c）。 */
static inline const type_t *vm_type_usize(const vm_t *vm) {
    return vm->type_usize;
}

/** 创建 VM（初始化内置类型、全局作用域、调用栈） */
vm_t *vm_new(allocator_t *alloc);

/** 销毁 VM（释放所有资源） */
void vm_destroy(vm_t **vm);

/** 进入新作用域（current_scope 变为新的子作用域） */
void vm_push_scope(vm_t *vm);

/** 退出当前作用域（销毁该作用域所有 value，current_scope 回退到 parent） */
void vm_pop_scope(vm_t *vm);

/* ---- 模块表（M5：规范路径 → module_t*） ---- */

/**
 * 登记模块到 vm->modules 表。IMPORT <path> 运行期用。
 * 幂等——同一路径重复登记无害（后者替换前者，前者需调用方释放）。
 * vm 拥有 module_t 生命周期（vm_destroy 释放）。
 */
void vm_module_bind(vm_t *vm, const char *path, module_t *mod);

/**
 * 按规范路径查找模块。返回 module_t*，未找到返回 NULL。
 */
module_t *vm_module_lookup(vm_t *vm, const char *path);

/**
 * 按路径加载模块：查 vm->modules 表，命中构造模块值（type=type_module，
 * data=module_t*）压栈返回。未找到返回 NULL。
 */
value_t *vm_import(vm_t *vm, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VM_ */
