#ifndef _H_CLUX_CTFE_CTFE_
#define _H_CLUX_CTFE_CTFE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "sema/sema.h"
#include "vm/vm.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * CTFE（compile-time evaluation）模块
 *
 * 直接解释 AST 产生真实 value（非 shadow），与 sema_expr（shadow 纯类型检查）
 * 平行，仅在编译期常量槽位按需调用（数组边界 N、type 别名计算、
 * sizeof/alignof/typeof、enum 值）。eval 命令亦复用本模块做端到端求值。
 *
 * 严格限制（设计定稿，docs/m2-design.md §8）：
 *   - 同步递归 AST 解释器，不允许暂停恢复
 *   - 必须编译期求值：遇运行期依赖（scope 查不到的标识符等）→ error
 *   - 禁止 FFI：只解释 AST_FUNC_DEF（clux 函数）；非 AST 实体不在可调范围
 *     （本骨架不实现 FFI 识别，规则仅记录）
 *
 * 生命周期契约：ctfe_eval / ctfe_eval_stmt 产生的中间值与结果都 track 到
 * vm->current_scope。调用方负责：push scope → 求值 → clone 结果到自己的
 * scope（若需跨 scope 存活）→ pop scope。
 */

/* ---- 控制流标志（语句解释器内部使用） ---- */

typedef enum {
    CTFE_CTRL_NONE = 0,
    CTFE_CTRL_RETURN,    /* return 触发：函数体解释立即停止 */
    CTFE_CTRL_BREAK,     /* break 触发 */
    CTFE_CTRL_CONTINUE,  /* continue 触发 */
} ctfe_ctrl_t;

/* ---- 求值上下文 ---- */

typedef struct ctfe_ctx {
    vm_t        *vm;        /* 求值上下文：value 构造/运算 + scope 生命周期 */
    sema_t      *sema;      /* 符号表（函数调用查 AST_FUNC_DEF）；可 NULL（eval 场景） */
    sema_scope_t *sema_scope; /* 当前 sema 词法作用域（局部 comptime 函数/变量
                                 符号所在块）；NULL 时函数调用回退 global_scope */
    size_t       budget;    /* 剩余求值步数（每节点 -1，耗尽报错，防死循环） */
    size_t       depth;     /* 当前递归深度（ctfe_eval 嵌套层数） */
    size_t       max_depth; /* 递归深度上限（函数嵌套/表达式嵌套） */
    ctfe_ctrl_t  ctrl;      /* 控制流标志（RETURN/BREAK/CONTINUE，语句解释器读写） */
    value_t     *ret_value; /* RETURN 时携带的返回值（借用，归 callee scope） */
} ctfe_ctx_t;

/* ---- 公共 API ---- */

/**
 * 表达式求值：返回真实 value（track 到 vm->current_scope）。
 * 求值失败返回 error value（value_is_error 判定）。
 * NULL 参数 / 未知节点种类 → error value。
 */
value_t *ctfe_eval(ctfe_ctx_t *ctx, ast_node_t *node);

/**
 * 语句解释：逐条执行块内语句。遇 AST_RETURN / AST_BREAK / AST_CONTINUE
 * 设置 ctx->ctrl 并立即返回（块解释循环据此停止）。
 * 表达式节点作为语句传入时求值并丢弃结果。
 * 失败返回 error value（value_is_error 判定）。
 */
value_t *ctfe_eval_stmt(ctfe_ctx_t *ctx, ast_node_t *stmt);

/* ===========================================================================
 * internal（ctfe.c / ctfe_expr.c / ctfe_stmt.c / ctfe_call.c 共享，不对外）
 * =========================================================================== */

/* ---- 内部工具（ctfe.c 实现） ---- */

/** 构造错误 value。 */
value_t *ctfe_err(ctfe_ctx_t *ctx, const char *msg);

/** 格式化错误 value（带上下文诊断，如函数名）。 */
value_t *ctfe_errf(ctfe_ctx_t *ctx, const char *fmt, ...);

/** 兄弟链计数（实参个数等）。 */
size_t ctfe_count_siblings(const ast_node_t *node);

/** strslice → NUL 结尾临时缓冲区（栈上，仅短名使用）。 */
const char *ctfe_slice_to_cstr(strslice_t s, char *buf, size_t cap);

/** 按类型宽度构造整数值（i8..u64）。 */
value_t *ctfe_make_int(ctfe_ctx_t *ctx, const type_t *t, uint64_t v);

/** AST_INT_LIT 类型后缀 → vm 整数类型；空后缀默认 i32。 */
const type_t *ctfe_int_lit_type(ctfe_ctx_t *ctx, strslice_t suffix);

/** 读取 bool 值（调用方保证类型为 bool）。 */
bool ctfe_read_bool(vm_t *vm, value_t *v);

/* ---- 作用域上移工具（ctfe.c 实现，stmt/call 在 pop 前调用） ---- */

/** RETURN 控制流下把 ret_value 上移到父作用域（防止 pop 销毁）。 */
void ctfe_hoist_return(ctfe_ctx_t *ctx);

/** error value 跨作用域传播：pop 前 clone 到父作用域。非 error / 无父作用域时原样返回。 */
value_t *ctfe_hoist_error(ctfe_ctx_t *ctx, value_t *e);

/* ---- 节点求值入口（各文件实现） ---- */

value_t *ctfe_eval_inner(ctfe_ctx_t *ctx, ast_node_t *node);                     /* ctfe_expr.c */
value_t *ctfe_call_ast_fn(ctfe_ctx_t *ctx, ast_func_def_t *fn, ast_node_t *args); /* ctfe_call.c */
value_t *ctfe_call_value(ctfe_ctx_t *ctx, value_t *callee, ast_node_t *args);     /* ctfe_call.c */

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_CTFE_CTFE_ */
