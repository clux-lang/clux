#ifndef _H_CLUX_CTFE_CTFE_
#define _H_CLUX_CTFE_CTFE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "vm/vm.h"

/* 复合值支持（ctfe_composite.c）需要具体 AST 节点的定义。 */
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_call.h"
#include "parser/ast_make.h"
#include "parser/ast_member.h"
#include "parser/ast_slice.h"
#include "parser/ast_try.h"
#include "parser/ast_unwrap.h"

/* sema 类型只以**指针**形式出现（sema 符号表查函数 AST + 当前词法作用域），
   因此这里用前向声明而非 #include "sema/sema.h"——否则 sema 与 ctfe 互相
   include（sema 的 comptime.c/expr.c 需要 ctfe_eval，ctfe.h 需要 sema_t），
   形成头文件循环依赖。前向声明使依赖方向单向：ctfe → sema。 */
struct sema_t;         /* sema 上下文（sema/sema.h 定义） */
struct _sema_scope_t;  /* sema 词法作用域（sema/symbol.h 定义） */
typedef struct sema_t sema_t;
typedef struct _sema_scope_t sema_scope_t;
#include <stdbool.h>
#include <stddef.h>

/**
 * CTFE（compile-time evaluation）模块
 *
 * 直接解释 AST 产生真实 value（非 shadow），与 sema_expr（shadow 纯类型检查）
 * 平行，仅在编译期常量槽位按需调用（数组边界 N、type 别名计算、
 * sizeof/alignof/typeof、enum 值）。eval 命令亦复用本模块做端到端求值。
 *
 * 严格限制（设计定稿）：
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
    const type_t *ret_type; /* 当前函数声明的返回类型（return 值的类型上下文；
                               用于 `return nil` / T→?T 等带类型上下文的求值） */
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

/** 定义局部变量：**总是**把值 materialize 成目标作用域自己的副本。
 *
 * 为什么不能直接调 scope_define：CTFE 的 AST_IDENT 返回的是作用域里**存储的
 * value 本体**（不像运行期压入借用视图），于是 `var b = a` 的 b 会命中
 * scope_define 的"本作用域非借用值直接采用（零拷贝）"快路径 → b 与 a 别名，
 * 写 b 会改写 a。这里先 clone 到目标作用域再 define，语义与运行期一致。 */
value_t *ctfe_define_value(ctfe_ctx_t *ctx, scope_t *scope, const char *name,
                           value_t *v);

/* ---- 作用域上移工具（ctfe.c 实现，stmt/call 在 pop 前调用） ---- */

/** RETURN 控制流下把 ret_value 上移到父作用域（防止 pop 销毁）。 */
void ctfe_hoist_return(ctfe_ctx_t *ctx);

/** error value 跨作用域传播：pop 前 clone 到父作用域。非 error / 无父作用域时原样返回。 */
value_t *ctfe_hoist_error(ctfe_ctx_t *ctx, value_t *e);

/* ---- 节点求值入口（各文件实现） ---- */

value_t *ctfe_eval_inner(ctfe_ctx_t *ctx, ast_node_t *node);                     /* ctfe_expr.c */
value_t *ctfe_call_ast_fn(ctfe_ctx_t *ctx, ast_func_def_t *fn, ast_node_t *args); /* ctfe_call.c */
value_t *ctfe_call_value(ctfe_ctx_t *ctx, value_t *callee, ast_node_t *args);     /* ctfe_call.c */
/** 用**已求值**的实参调用 AST 函数（方法调用：接收者已求值，不能重求 AST）。 */
value_t *ctfe_call_ast_fn_vals(ctfe_ctx_t *ctx, ast_func_def_t *fn,
                               value_t *const *args, size_t argc);               /* ctfe_call.c */

/* ---- 复合值（ctfe_composite.c）----
 *
 * CTFE 的值是真实 data 块（与运行期同一套 type_t 布局），所以复合值访问就是
 * "按类型布局读写 data"。这一层提供与运行期字节码同语义的访问：
 *   - struct 字段 / tag union 成员（tag 校验）/ cunion 成员（offset 0）
 *   - 指针自动解引用（`p.x` / `p.*` / `p.&`）
 *   - optional 解包（`.!`）与 `nil`（比较 / 初始化 / 构造 / 赋值）
 *   - 切片表达式 `a[lo:hi]` 与 `make(ptr, len)`
 *   - 实例方法调用 `recv.m(args)`（含 `*T` 接收者取地址）
 */

/** 带类型上下文求值：nil → none、T → ?T 提升、隐式转换；want == NULL 时等价
 *  ctfe_eval。用于 var 初始化 / 构造字段 / 赋值右值 / 实参 / return。 */
value_t *ctfe_eval_as(ctfe_ctx_t *ctx, ast_node_t *e, const type_t *want);

/** 构造 `?T` 的 none 值（零 tag + 归零载荷）。 */
value_t *ctfe_none_value(ctfe_ctx_t *ctx, const type_t *opt_type);

/** `x == nil` / `x != nil`（lhs_is_nil 指出哪一侧是 nil 字面量）。 */
value_t *ctfe_nil_compare(ctfe_ctx_t *ctx, ast_binary_t *b, bool lhs_is_nil);

/** `x is Member`：tag union 的 member 判定（编译期折叠为 bool 常量）。 */
value_t *ctfe_is_member(ctfe_ctx_t *ctx, ast_binary_t *b);

/** `obj.field` 读（struct / union / cunion / 指针自动解引用）。 */
value_t *ctfe_member_get(ctfe_ctx_t *ctx, ast_member_t *m);

/** 成员 / 解引用 / 解包目标的赋值（含复合赋值）。返回 void 或 error。 */
value_t *ctfe_assign_lvalue(ctfe_ctx_t *ctx, ast_assign_t *as);

/** 下标左值赋值 `a[i] = v` / `a[i] op= v`（数组 / 元组 / 切片 / 指针）。 */
value_t *ctfe_index_assign(ctfe_ctx_t *ctx, ast_assign_t *as);

/** `x.!`：optional 断言解包（none → 编译期错误）。 */
value_t *ctfe_unwrap_opt(ctfe_ctx_t *ctx, ast_unwrap_t *u);

/** `p.*`：指针解引用读（null → 编译期错误）。 */
value_t *ctfe_deref(ctfe_ctx_t *ctx, ast_node_t *operand);

/** `x.&`：取地址（操作数必须是可寻址的编译期存储）。 */
value_t *ctfe_addr_of(ctfe_ctx_t *ctx, ast_node_t *addr_node);

/** `a[lo:hi]`：数组 / 切片 / str 的切片视图。 */
value_t *ctfe_slice_expr(ctfe_ctx_t *ctx, ast_slice_t *sl);

/** `make(ptr, len)`：非分配切片视图（与运行期一致）。 */
value_t *ctfe_make_slice(ctfe_ctx_t *ctx, ast_make_t *mk);

/** 实例方法调用 `recv.m(args)`。返回 NULL = 不是方法调用（调用方回退）。 */
value_t *ctfe_method_call(ctfe_ctx_t *ctx, ast_call_t *call);

/** 静态方法调用 `Type::method(args)`。返回 NULL = 不是静态方法
 *  （例如枚举 variant 引用 `Color::Red`），调用方回退。 */
value_t *ctfe_static_call(ctfe_ctx_t *ctx, ast_call_t *call);

/** 内建 `data(x)`：数组 / 切片 / str 的首元素指针（`*T`）。
 *  只指向编译期存储，不能编码成运行期常量。 */
value_t *ctfe_builtin_data(ctfe_ctx_t *ctx, ast_node_t *arg);

/** 内建 `take(opt)`：取走 optional 载荷并把源置 nil（none → 编译期错误，
 *  与运行期 panic 对应）。返回载荷值（类型 T）。 */
value_t *ctfe_builtin_take(ctfe_ctx_t *ctx, ast_node_t *arg);

/** 内建 `panic(msg)`：编译期 panic → 求值消息后返回错误值（不会真的 abort）。 */
value_t *ctfe_builtin_panic(ctfe_ctx_t *ctx, ast_node_t *arg);

/** `x.?` / `x.!`（sema 生成的 AST_TRY）：先跑 setup，再按 `ok()` 分派
 *  `value()` 或失败路径（`.?` → 执行 tail 的 return；`.!` → 编译期 panic）。 */
value_t *ctfe_try_expr(ctfe_ctx_t *ctx, ast_try_t *tr);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_CTFE_CTFE_ */
