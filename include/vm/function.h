#ifndef _H_CLUX_VM_FUNCTION_
#define _H_CLUX_VM_FUNCTION_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/value.h"
#include "core/strslice.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct vm_t    vm_t;
typedef struct scope_t scope_t;
typedef struct func_t  func_t;

/* ---- C 函数签名 ---- */

/**
 * 引擎侧函数原型：所有 clux 函数最终以 C 函数形式执行。
 * - vm: 虚拟机上下文
 * - self: 指向 func_t 自身
 * - argc: 实参数量
 * - args: 实参数组（已 clone 到当前作用域）
 * 返回值: 结果 value（调用方负责 clone 到自己的作用域）
 */
typedef value_t *(*cfunc_t)(vm_t *vm, func_t *self, size_t argc, value_t **args);

/* ---- func_t: 函数对象 ---- */

/**
 * func_t: 函数对象（VM 引擎侧）
 *
 * - cfunc: C 函数指针，引擎通过它执行逻辑
 * - closure_scope: 定义时的词法环境（闭包捕获）
 * - root_scope: 定义时的模块作用域（全局根的子作用域）
 * - id: 函数全局唯一 id（内建函数由 vm 自动分配 < FUNC_ID_PROGRAM_BASE；
 *   程序函数由编译器分配 >= FUNC_ID_PROGRAM_BASE，PUSH_FUNCTION 立即数
 *   写入，BIND_FUNC 登记进 vm->functions_by_id）
 * - name: 函数名（调试/显示用）。内建函数为静态字面量（owns_name=false）；
 *   程序函数经 SET_FUNC_NAME 拷贝到 vm 堆（owns_name=true，func_destroy 释放）
 *
 * 签名类型存于 func_t->type（func_new 传入，归 vm 类型池所有，不释放）；
 * func value 的 type 亦为签名类型，调用点经 value_type() 取回
 * （见 func_new）。LOAD_FUNCTION <id> 从 func_t 直接取签名包装 value。
 * sema 侧不构造 func value：符号表只记录函数定义 AST 节点（sym->ast），
 * 签名类型存于 sym->type。
 *
 * 引擎视角的参数没有名字，cfunc 通过 args[i] 按位置访问。
 * 参数名绑定是语言层（AST interpreter）的职责。
 */
struct func_t {
    cfunc_t         cfunc;
    scope_t        *closure_scope;

    /* 闭包的**捕获聚合类型**（struct；NULL = 无捕获/非闭包）。由
       `SET_FUNC_CAPTURES <type_id>` 在 hoist 注册函数对象时写入，实例化时
       复制。VM 层据此把闭包当聚合值处理：clone 深拷贝捕获、free 按槽位
       所有权释放、字段读写按槽位类型。 */
    const struct struct_type_t *capture_type;

    /* 捕获 env 块（裸数据块，布局 = capture_type）：闭包 = { 函数实现, env }，
       env 就是"捕获结构体"的实例。由 func_instantiate_raw 按 capture_type
       分配（zeroed），SET_CLOSURE 按字段偏移写入；字段读取/写入/克隆/释放
       全部按 capture_type 的槽位规则进行（与结构体完全同构）。无捕获 = NULL。 */
    void           *env;

    /* true = env 指向**函数对象分配块内部**的内联捕获块（`func_instantiate_raw`
       把 `sizeof(bcode_function_t) + env_size` 一次分配，env = 块尾）：随函数
       对象一起释放，FREE/销毁路径不得再 `allocator_free(env)`。
       false = env 是独立堆块（hoist 基底对象的兜底分配）或 NULL。 */
    bool            env_inline;

    /* 闭包 env 的**生成函数** id（"闭包 = 捕获聚合结构体 + vtable"）：
       - env_dispose_fid：`__destroy_<env>(dv: ref *Env)` —— 释放 env 的 own 捕获；
       - env_clone_fid  ：`__clone_<env>(dv: ref *Env): fatal Env` —— 深拷贝 env。
       0 = 无（无捕获 / env 无资源）。clone/free 时**间接调用**它们（而不是按
       capture_type 运行期内省），与 C 后端 `struct { callee, env, dispose, clone }`
       一一对应。 */
    uint32_t        env_dispose_fid;
    uint32_t        env_clone_fid;
    scope_t        *root_scope;
    const type_t   *type;        /* 签名类型（func_type_t，归 vm 类型池所有，不释放） */
    uint32_t        id;
    strslice_t      name;
    bool            owns_closure_scope; /* true：closure_scope 由函数对象创建（bcode_function），
                                           随 vm->functions 释放；false：调用方传入（不拥有） */
    bool            owns_name;          /* true：name 为 SET_FUNC_NAME 堆拷贝，func_destroy 释放 */

    /* ---- M6 FFI Phase 5：该函数作为 **C 回调** 的闭包（惰性创建）----
     * `cfunc_ptr(f)`（内建）首次要求 C 函数指针时创建 libffi 闭包：
     *   ffi_closure      —— libffi 闭包句柄（clux_ffi_closure_destroy 释放）
     *   ffi_closure_ctx  —— handler 上下文（本 allocator 分配）
     *   ffi_closure_code —— trampoline 地址（可当 C 函数指针用的那个值）
     * 三者归**函数对象**所有，随 vm->functions 一起销毁——与 closure_scope 同一
     * 生命周期模型（函数对象是 VM 生命周期对象，不归任何作用域）。因此重复调用
     * `cfunc_ptr(f)` 复用同一 trampoline，不重复泄漏可执行内存。
     * 全 NULL = 尚未请求过 C 函数指针（普通函数恒为 NULL）。 */
    void           *ffi_closure;
    void           *ffi_closure_ctx;
    void           *ffi_closure_code;

    /* ---- 编译期：该函数字面量对应的 AST 节点（`ast_func_def_t*`）----
     * CTFE 求值字面量时填入，供**常量闭包折叠**取回参数/函数体（此时
     * sema_func_by_id 可能尚未登记该字面量：pass_globals 早于 Pass 3b）。
     * 运行期对象恒 NULL；不参与任何运行期语义。 */
    const void     *ct_literal;
};

/**
 * 闭包捕获写入：按 `capture_type` 槽位先销毁旧值（own 槽位释放），
 * 再 implicit_cast + memcpy 写入 `env` 块。
 *
 * 两个调用方：运行期 `SET_CLOSURE`（op_set_closure）与**编译期常量闭包的
 * 物化**（CTFE 让 comptime 函数返回带捕获的闭包时，捕获值以常量形式写进
 * 实例 env，随后由 sema 编码成 MAKE_FUNCTION + SET_CLOSURE 序列）。
 *
 * 返回 NULL 成功；否则返回**静态**错误文本（调用方包成 error value / 诊断）。
 */
const char *func_closure_store(vm_t *vm, func_t *fn, strslice_t name,
                               value_t *val);

/* ---- 函数 id 分段（function.c / compile_func.c 共用） ---- */
/* 内建函数固定 id 0..(FUNC_ID_BUILTIN_COUNT-1)（vm 自动分配：func_new 内
   部从 0 起递增，printf=0）；程序函数 id 由编译器分配，从 FUNC_ID_PROGRAM_BASE
   起（预留扩展空隙，见 vm.h 注释）。 */
#define FUNC_ID_BUILTIN_COUNT 2u
#define FUNC_ID_PROGRAM_BASE  64u

/* ---- 函数 id 表（id → func_t*，索引即 id） ---- */

/**
 * 登记函数到 id 表（vm->functions_by_id，索引即 id）。BIND_FUNC <id> 运行期
 * 用；幂等——同一函数重复登记（多 id 别名）无害。
 * Panics on out-of-memory.
 */
void vm_func_bind(vm_t *vm, uint32_t id, func_t *fn);

/**
 * 按 id 查函数：id 越界或未登记返回 NULL。调用方无需手动释放（func_t 归
 * vm->functions 池）。
 */
func_t *vm_func_load(vm_t *vm, uint32_t id);

/** 把一个 func_t 包成**借用**的 func 值（data 是 func_t*，与 LOAD_FUNCTION
 *  一致；不 clone、不实例化）。用于 VM 内部按 fid 间接调用生成的 env vtable
 *  函数（`__destroy_<env>` / `__clone_<env>`）。 */
value_t *func_value_ref(vm_t *vm, func_t *fn);

/* ---- 生命周期 ---- */

/**
 * 创建函数对象并包装为 func value。
 *
 * - vm: 虚拟机上下文（alloc 取自 vm->alloc；func_t 注册进 vm->functions，
 *   由 vm_destroy 统一释放生命周期）
 * - sig_type: 签名类型（func_type_t，由 type_func_sig 注册 intern）。
 *   成为 func value 的 type：调用点经 value_type() 取回签名，
 *   func_shadow_call 据此做参数数量/隐式转换校验。不可为 NULL。
 * - 返回 value_t*：data 存指向 func_t 的指针，type 即 sig_type。
 *   untracked（调用方管理生命周期）：value_dispose(vm, v) 释放 data 块
 *   与 value_t 结构体；func_t 本体归 vm->functions，由 vm_destroy 释放。
 */
value_t *func_new(vm_t *vm,
                  cfunc_t cfunc,
                  scope_t *closure_scope,
                  scope_t *root_scope,
                  const type_t *sig_type,
                  strslice_t name);

/**
 * 销毁函数对象（释放 func_t 自身；签名归 vm 类型池所有）。
 * 仅供 vm_destroy 遍历 vm->functions 时调用，普通代码不应直接使用
 * （函数对象生命周期由 vm 统一管理）。
 */
void func_destroy(allocator_t *alloc, func_t **fn);

/**
 * 创建程序函数"引用对象"（CTFE 函数引用专用）：轻量 func_t，仅携带
 * 签名 + 名字 + id（cfunc=NULL，无 body 入口，不可调用），注册进
 * vm->functions 统一释放，不登记 functions_by_id。
 *
 * 用途：comptime 函数体内引用/求值普通函数（`comptime func get() {
 * return add; }` 或 `return func(x:i32):i32{...};`）时，CTFE 求值需要一个
 * 函数值载体——折叠终点是 AST_FUNC_REF（sema_ct_lit 按 id 产出），
 * 运行期由 LOAD_FUNCTION <fid> 加载真实函数，与本对象无关。
 *
 * - sig_type: 签名类型（func_type_t，归 vm 类型池）；name: 函数名（借用，
 *   owns_name=false，sema arena / vm 静态字面量生命周期）。
 * - id: 函数 id（sema 分配的 fid：函数字面量/局部函数经 sema_func_id_alloc
 *   幂等分配，源码引用取符号表 fid；内建函数 = 内建 id）。func_t->id 供
 *   sema_ct_encode 编码折叠（LOAD_FUNCTION <id> 的依据），与是否登记
 *   functions_by_id 无关。
 * - 返回 func_t*（非 value）：调用方自行 value_alloc_data_copy 包装。
 *   Panics on out-of-memory.
 */
func_t *func_new_program_ref(vm_t *vm, const type_t *sig_type,
                             strslice_t name, uint32_t id);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_FUNCTION_ */
