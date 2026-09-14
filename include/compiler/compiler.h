#ifndef _H_CLUX_COMPILER_COMPILER_
#define _H_CLUX_COMPILER_COMPILER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "vm/bcode.h"
#include "vm/vm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===========================================================================
 * 字节码编译器（AST → bcode）
 *
 * 输入：AST（parser 产物，arena 持有）+ sema 作用域树（仅用于查找
 * 函数签名类型 / 变量符号元数据）。
 * 输出：bytecode_t 模块（strtable + code 流），与执行器约定完全对齐
 * （见 docs/architecture-m1.md 2.7 与 tests/exec_test.cpp 函数测试）。
 *
 * 编译错误策略：diag 收集 + fail-fast。任一节点编译出错即置
 * compiler->failed，停止继续编译；driver 出口统一 diag_print_all。
 *
 * 平衡规则（编译期静态追踪）：
 *   - scope_depth：跳转跨出 N 层块作用域时，先发 N 个 POP_SCOPE 再跳
 *   - stack_depth：条件分支汇合点（if 的 end / for 的 update 入口）要求
 *     两侧栈深一致（表达式语句压栈的值由调用方 POP 平衡）
 * =========================================================================== */

/* ---- 标签（前向占位 + 回填） ---- */

typedef struct compile_patch_t {
    size_t pos;                 /* JMP/JZ/JNZ 的操作数字段偏移（opcode+4） */
    struct compile_patch_t *next;
} compile_patch_t;

typedef struct compile_label_t {
    bool           defined;     /* 已定义：pos 为实际目标 pc */
    size_t         pos;
    compile_patch_t *patches;   /* 未定义时的前向跳转 patch 列表 */
} compile_label_t;

/* ---- 循环上下文（break/continue 目标标签 + 跳出深度） ---- */

typedef struct compile_loop_t {
    compile_label_t *break_label;    /* 循环出口（break 目标） */
    compile_label_t *continue_label; /* 循环体底部（continue 目标） */
    size_t           scope_depth;    /* 循环进入时的 scope 深度（break/continue 跳出基准） */
    struct compile_loop_t *next;
} compile_loop_t;

/* ---- 编译器上下文 ---- */

typedef struct compiler_t {
    allocator_t    *alloc;
    vm_t           *vm;
    diag_buf_t     *diag;
    bytecode_t     *bc;
    vec_t          *tokens;        /* token pool（借 driver，诊断用位置） */

    /* sema 作用域树（借用，不拥有；编译完成后由 driver 释放） */
    sema_scope_t   *global_scope;
    sema_scope_t   *current_scope; /* 编译期当前词法作用域（符号元数据查找） */

    /* sema 类型登记表（借用，不拥有）：sema->types（sema_type_t* 数组）。
       hoist 类型提升区遍历它生成 BIND_TYPE 构造；AST_TYPE_REF 槽位经
       sema_type_find_name 查表拿 id 发 LOAD_TYPE。 */
    vec_t          *sema_types;

    /* 函数 id 分配计数器：编译注册段时按声明顺序从 FUNC_ID_PROGRAM_BASE
       起递增（PUSH_FUNCTION <id> 立即数），与类型 id 机制对称——但分配在
       compiler 侧（不写回 AST），运行时 BIND_FUNC 登记进 vm->functions_by_id。 */
    uint32_t        func_id_next;

    /* 静态平衡追踪 */
    size_t          scope_depth;    /* 当前已 PUSH_SCOPE 未 POP 的层数 */
    int             stack_depth;    /* 静态操作数栈深度（压栈 +1 / 弹栈 -1） */

    compile_loop_t *loop_stack;     /* 循环上下文栈（break/continue） */

    bool            failed;         /* 已发生编译错误（fail-fast） */
} compiler_t;

/* ---- 生命周期 ---- */

/**
 * 创建编译器上下文。
 * - vm: 运行 VM（类型查询 / LOAD 校验用；不执行）
 * - diag: 诊断收集器（driver 出口打印）
 * - tokens: token pool（借 driver，节点位置 → location 转换用）
 * - global_scope: sema 作用域树根（借用，符号元数据查找）
 * - sema_types: sema 类型登记表（借用，hoist 提升 + LOAD_TYPE 槽位查找）
 * 返回 NULL 表示参数无效或 OOM。
 */
compiler_t *compiler_new(allocator_t *alloc, vm_t *vm, diag_buf_t *diag,
                         vec_t *tokens, sema_scope_t *global_scope,
                         vec_t *sema_types);

/** 销毁编译器上下文（不释放 vm/diag/tokens/scope 树，均为借用） */
void compiler_destroy(compiler_t **pc);

/**
 * 编译 AST 程序为字节码模块（产物归调用方，bcode_destroy 释放）。
 * 编译错误时返回 NULL（诊断已记入 diag，driver 出口打印）。
 * 成功后 bc 即编译产物（含函数注册段 + 各函数体），调用方
 * exec_run 后 scope_lookup("main") + value_call 触发执行。
 */
bytecode_t *compiler_compile(compiler_t *c, ast_node_t *program);

/* ===========================================================================
 * internal（compile.c / compile_type.c / compile_expr.c / compile_stmt.c /
 *          compile_func.c 共享，不对外）
 * =========================================================================== */

/* ---- 内部工具（compile.c 实现） ---- */

/** AST 节点 → 源码位置（经 token pool）。 */
location_t c_loc(compiler_t *c, const ast_node_t *node);

/** 编译错误：记入 diag + 置 failed（fail-fast）。 */
void c_error(compiler_t *c, const ast_node_t *node, const char *fmt, ...);

/* ---- 标签工具（前向占位 + 回填） ---- */

void label_init(compile_label_t *l);
void label_here(compiler_t *c, compile_label_t *l);
void emit_jump(compiler_t *c, compile_label_t *l);

/* ---- 静态平衡工具（scope/操作数栈深度静态追踪） ---- */

void balance_push(compiler_t *c);
void balance_pop(compiler_t *c);
void balance_scopes_out(compiler_t *c, size_t n);
void st_push(compiler_t *c, int delta);

/* ---- 节点编译入口（各文件实现） ---- */

void   compile_type_expr(compiler_t *c, ast_node_t *type_expr); /* compile_type.c */
void   compile_expr(compiler_t *c, ast_node_t *node);          /* compile_expr.c */
void   compile_stmt(compiler_t *c, ast_node_t *node);          /* compile_stmt.c */
size_t compile_func_body(compiler_t *c, ast_func_def_t *fn);   /* compile_func.c */
size_t compile_func_reg(compiler_t *c, ast_func_def_t *fn);    /* 返回 PUSH_FUNCTION body 操作数字段位置 */

/* ---- hoist 类型提升区（compile_hoist.c） ---- */

/**
 * 编译类型提升区（产物最前、注册段之前）：
 * 遍历 sema->types，按 type_t 结构单遍递归构造（依赖后序）每个程序类型并
 * BIND_TYPE <id> 绑定（内建类型已由 vm_register_builtin_types 绑内建 id，
 * 此处仅 BIND 别名 id）；数组/const/volatile 构造后 BIND 密封实例，槽位
 * LOAD_TYPE <id> 运行时直接查表。净栈深 0。
 */
void compile_hoist(compiler_t *c);

/** 类型登记表查找（AST_TYPE_REF 名字 / type_t 指针 → sema_type_t）。 */
const sema_type_t *c_sema_type_find_name(vec_t *types, strslice_t name);
const sema_type_t *c_sema_type_find_ptr(vec_t *types, const type_t *t);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_COMPILER_COMPILER_ */
