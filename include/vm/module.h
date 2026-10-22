#ifndef _H_CLUX_VM_MODULE_
#define _H_CLUX_VM_MODULE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/strmap.h"
#include <stdbool.h>

typedef struct vm_t       vm_t;
typedef struct scope_t    scope_t;
typedef struct bytecode_t bytecode_t;
typedef struct _lexer_t   lexer_t;
typedef struct ast_node   ast_node_t;
typedef struct _sema_scope_t sema_scope_t;

/**
 * module_t: 编译期模块对象（M5 模块系统）
 *
 * 每个模块独立走完整流水线（lex → parse → sema → compile），产出三部分：
 *
 * - bc: 该模块的字节码（注册段在 PC=0，函数体在后）。自包含。
 * - global_scope: 模块全局作用域。运行期 DEFINE 把函数/var 绑定到这里。
 *   func_t.root_scope 指向所属 module 的 global_scope，跨模块调用时切换。
 * - exports: 导出表（name → value_t* 借用映射，指向 global_scope 中的符号）。
 *   GET_MEMBER 从这里取成员。非导出符号不在此表中，模块私有。
 * - canonical: 规范路径，作为 vm->modules 的 key（owned，vm_destroy 释放）。
 *
 * module 对象归 vm 拥有，vm_destroy 统一释放。sema 用完即销毁，产物转移给 module。
 */
typedef struct module_t {
    bytecode_t *bc;             /* 模块字节码（owned） */
    scope_t    *global_scope;   /* 模块全局作用域（owned） */
    strmap_t   *exports;        /* name → value_t* 借用映射（owned map，借用 value） */
    char       *canonical;      /* 规范路径（owned，NUL 终止） */

    /* ---- M5 字节码容器所需：**源码路径**与导入者（owned 副本，可 NULL） ----
     * 容器与 `.cxs` 文本里存的是**源码中写的路径**（"./math"、"./lib/api"），
     * 不是规范化后的宿主路径——产物因此与宿主目录布局解耦（换个目录照样解析）。
     * 加载时由加载方以"导入者的规范路径"为基准重新规范化，得到 vm->modules 的
     * key（与运行期 op_import 对 IMPORT 操作数做的规范化完全一致）。
     *
     * - import_path: 导入它的那条 `import` 语句里写的路径（NULL = 入口模块）
     * - importer:    导入者的规范路径（NULL = 入口模块）
     *
     * 菱形依赖（两个模块以不同写法导入同一模块）只需记录任意一条边：两条边
     * 规范化后必然得到同一个 key（否则编译期就不会解析到同一模块）。 */
    char       *import_path;
    char       *importer;

    /* ---- M6 FFI：extern lib 库表（别名 → 动态库句柄；owned，destroy 关闭全部）----
     * 别名即源码里 `extern lib <名> from "...";` 的 <名>，`extern cfunc <名>::<fn>`
     * 依此查表取句柄再 dlsym。NULL = 本模块没有 extern lib（懒创建）。 */
    strmap_t   *ffi_libs;

    /* ---- M7 跨模块泛型：保留源码侧句柄 ----
     * 导入方实例化 `mod::Vec[i32]` / `mod::f[i32](...)` 时需要重新解析
     * 定义点 token 区间。AST 节点在共享 arena 中（借用），但 token 文本
     * 指向 lexer 的源码缓冲区——因此模块必须保留 lexer 与 token pool
     * （owned；module_destroy 释放）。src_ast 借用共享 arena。 */
    lexer_t    *src_lexer;
    void       *src_tokens;  /* vec_t*（token pool，元素由 pool 拥有） */
    ast_node_t *src_ast;     /* 顶层 AST_PROGRAM（借用共享 arena） */

    /* M7 跨模块泛型归属：本模块的 **sema 全局作用域**（保留到 module 销毁）。
       导入方实例化本模块泛型时，实例体必须按定义模块的词法环境解析名字；
       该作用域持有本模块全局 var/函数/类型/import 别名的 sema 符号。 */
    sema_scope_t *sema_global;

    /* M7 两阶段模块流水线（进行中）：保留 sema 编译期实例（funcs/prog/
       types/instances）到 module 销毁。跨模块泛型实例直接加入 owner 模块的
       prog 后，需要 owner sema 仍可继续实例化/编译。当前先保留，后续 finalize
       阶段使用。 */
    struct sema_t *sema;
    /* 两阶段流水线状态：prepared=sema 已完成；finalized=bytecode/注册/exports
       已完成。sema_exports 是阶段 1 的导出符号表（name → sema_symbol_t* 借用），
       供尚未 finalize 的依赖模块被跨模块 sema 解析。 */
    bool prepared;
    bool finalized;
    strmap_t *sema_exports;
} module_t;

/**
 * 创建 module 对象。canonical 路径字符串被拷贝（module 拥有副本）。
 * bc 和 global_scope 的所有权转移给 module。
 * exports 由调用方填充（sema 构建导出表后传入）。
 */
module_t *module_new(allocator_t *alloc, bytecode_t *bc,
                     scope_t *global_scope, strmap_t *exports,
                     const char *canonical);

/**
 * 销毁 module 对象：释放 bc、global_scope、exports map、canonical 字符串。
 */
void module_destroy(vm_t *vm, module_t **mod);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_MODULE_ */
