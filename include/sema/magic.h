#ifndef _H_CLUX_SEMA_MAGIC_
#define _H_CLUX_SEMA_MAGIC_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "sema/sema.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 魔法方法（magic methods）：结构体上的保留方法名，编译器把"该类型上的
 * 原生语法位置"改写为普通实例方法调用。设计见
 * docs/whitepaper/03-declarations.md。
 *
 * 命名空间：`__` 前缀在源码层保留（parser 的 reserved-ident 规则），仅
 * struct 成员方法位置放行；其余位置（变量/参数/类型名/全局函数）照旧拒绝。
 */

/* `__deref__` 展开深度默认值（可经 CLI `--magic-deref-depth=N` 或环境变量
   `CLUX_MAGIC_DEREF_DEPTH` 覆盖；0 = 禁用）。 */
#define SEMA_MAGIC_DEREF_MAX_DEPTH_DEFAULT 8u

/** 进程级覆盖（CLI 设置）：depth >= 0 生效，-1 = 清除（回落到 env/默认）。
 *  sema_create 时解析进 `sema_t.magic_deref_max_depth`。 */
void sema_magic_set_deref_depth_override(int depth);

/** 解析最终生效的默认深度：CLI 覆盖 > 环境变量 > 常量。 */
uint32_t sema_magic_deref_depth_resolve(void);

typedef enum {
    SEMA_MAGIC_NONE = 0,
    SEMA_MAGIC_ADD,   /* a + b  */
    SEMA_MAGIC_SUB,   /* a - b  */
    SEMA_MAGIC_MUL,   /* a * b  */
    SEMA_MAGIC_DIV,   /* a / b  */
    SEMA_MAGIC_MOD,   /* a % b  */
    SEMA_MAGIC_AND,   /* a & b  */
    SEMA_MAGIC_OR,    /* a | b  */
    SEMA_MAGIC_XOR,   /* a ^ b  */
    SEMA_MAGIC_SHL,   /* a << b */
    SEMA_MAGIC_SHR,   /* a >> b */
    SEMA_MAGIC_NEG,   /* -a     */
    SEMA_MAGIC_NOT,   /* !a     */
    SEMA_MAGIC_BNOT,  /* ~a     */
    SEMA_MAGIC_EQ,    /* a == b */
    SEMA_MAGIC_LT,    /* a < b  */
    SEMA_MAGIC_LE,    /* a <= b */
    SEMA_MAGIC_GT,    /* a > b  */
    SEMA_MAGIC_GE,    /* a >= b */
    SEMA_MAGIC_INDEX, /* a[i]   */
    SEMA_MAGIC_SLICE, /* a[lo:hi] */
    SEMA_MAGIC_LEN,   /* len(a) */
    SEMA_MAGIC_DEREF, /* a.x / a.* 的成员回退 */
    SEMA_MAGIC_DISPOSE, /* `using a = ...;` 的作用域退出调用（不是析构） */
    SEMA_MAGIC_ASSIGN,  /* a = b（赋值；初始化不派发） */
    SEMA_MAGIC_CALL,     /* x(args) ⇒ x.__call__(args) */
    SEMA_MAGIC_INIT_COPY, /* 同类型初始化：var a: T = b;（形参 *T） */
    SEMA_MAGIC_INIT,    /* .T{args} 构造（零初始化 + __init__） */
} sema_magic_t;

typedef struct {
    const char  *name;     /* "__add__" */
    sema_magic_t id;
    uint8_t      nparams;  /* 含 self 的形参总数 */
    bool         ret_bool; /* 返回类型必须是 bool */
    bool         ret_usize;/* 返回类型必须是 usize */
    bool         ret_deref;/* 返回类型必须可解引用（指针 / 带 __deref__） */
    bool         rhs_ptr;  /* 第二形参（右操作数）必须是指针（零拷贝） */
    bool         ret_void; /* 返回类型必须是 void（__dispose__） */
    bool         any_arity;/* 形参个数由用户决定（__init__） */
} sema_magic_entry_t;

/** 按名字查魔法表；不是魔法名返回 NULL。 */
const sema_magic_entry_t *sema_magic_by_name(strslice_t name);

/** 表项 → 名字（诊断用）。 */
const char *sema_magic_name(sema_magic_t id);

/** 名字是否以 "__" 开头（保留标识符空间）。 */
bool sema_magic_is_reserved_name(strslice_t name);

/**
 * 校验一个 struct 方法声明：
 *  - 名字以 "__" 开头时必须在魔法表内（未知魔法名 → 诊断）；
 *  - **所有** struct 方法的接收者必须是指针（`*Owner` / `*const Owner`）；
 *  - 魔法方法的参数个数 / 返回类型规则；
 *  - 魔法方法不允许自带类型参数（P0）。
 *
 * `params` / `ret` 由调用方解析后传入（可为 NULL = 未解析，跳过对应检查）。
 * 返回 true = 通过（或已诊断）。
 */
bool sema_magic_check_method(sema_t *sema, const struct type_t *owner,
                             ast_node_t *def, const struct type_t *const *params,
                             size_t nparams, const struct type_t *ret);

/** 遍历 sema->funcs（已解析签名的非泛型方法）与 prog（泛型方法声明）
 *  校验所有 struct 方法（Pass 2 之后调用一次）。 */
void sema_validate_methods(sema_t *sema, ast_program_t *prog);

/** 魔法方法接收者（`*T` / `*const T`）指向的类型；不是指针返回 NULL。 */
const struct type_t *sema_magic_receiver_target(const struct type_t *self_type);

/** 类型是否可解引用（指针，或自带 `__deref__` 的 struct）。 */
bool sema_magic_is_derefable(sema_t *sema, const struct type_t *t);

/** 在 `t` 的成员里找魔法方法；命中返回成员符号（未命中 NULL）。 */
sema_symbol_t *sema_magic_member(sema_t *sema, const struct type_t *t,
                                 const char *name);

/** 构造 `<recv>.<name>(args)` 的 AST（成员调用节点，未做类型检查）。
 *  args 可为 NULL；调用方随后用 `sema_expr` 重新按普通方法调用检查。
 *  节点以 `next = NULL` 返回，调用方负责接回兄弟链。 */
ast_node_t *sema_magic_make_call(sema_t *sema, ast_node_t *recv,
                                 const char *name, ast_node_t *args,
                                 uint32_t tok_begin, uint32_t tok_end);

/** 二元/一元运算符 → 魔法方法名（NULL = 该运算符不参与魔法派发）。
 *  `!=` 映射到 `__eq__`（调用方负责取反）。 */
const char *sema_magic_binop_name(const token_t *op);
const char *sema_magic_unop_name(const token_t *op);

/** `__deref__` 展开（`.x` / `.m()` / `.x = v` 的成员回退）：
 *  沿 `__deref__` 链找到 **member** 所在的接收者。
 *
 *  - `recv_t` 是接收者类型（值或指针），`recv_expr` 是接收者表达式；
 *  - `member` 是待访问的成员名；
 *  - 返回 1：`*out_expr` = 展开后的接收者（`(recv.__deref__())...`），
 *    调用方把它接回原成员访问/调用/赋值节点即可；
 *  - 返回 0：链不适用（没有 `__deref__` / 非 struct），调用方走原诊断；
 *  - 返回 -1：已诊断（环 / 超深 / 禁用）。
 *
 *  环/深度检测只在本链内部（局部 visited），不会与同一语句里的其它
 *  `__deref__` 访问串味。 */
int sema_magic_deref_rewrite(sema_t *sema, ast_node_t **out_expr,
                             const struct type_t *recv_t, ast_node_t *recv_expr,
                             const char *member, uint32_t tok_begin,
                             uint32_t tok_end);

/** 类型级判等的语法糖展开：`a == b`（lhs 是 struct/union，且该 struct 没有
 *  `__eq__`）⇒ 生成并调用 `__clux_eq_<id>(a.&, b.&)`。
 *  返回 NULL = 该类型不适用（调用方保留内建比较）。 */
ast_node_t *sema_magic_eq_call(sema_t *sema, const struct type_t *t,
                               ast_node_t *lhs, ast_node_t *rhs,
                               uint32_t tok_begin, uint32_t tok_end);

/** 该类型是否走"生成判等函数"路径（非位域 struct / tag union）。 */
bool sema_magic_eq_desugarable(sema_t *sema, const struct type_t *t);

/** 类型是否有 `__init__`（构造语义改写用）。 */
bool sema_magic_has_init(sema_t *sema, const struct type_t *t);


/** 单参 `__init__` 的隐式构造（`var a: T = x` / 实参 / 返回值 / 字段初始化）：
 *  目标 T 恰好一个初始化参数、src 不是 T 且可赋给该参数时，把 *slot 改写成
 *  `__clux_init_T(<原表达式>)`。返回新 shadow（*converted = true）。 */
value_t *sema_magic_init_convert(sema_t *sema, ast_node_t **slot,
                                 const struct type_t *src_t,
                                 const struct type_t *target_t,
                                 sema_scope_t *scope, bool *converted);

/** `.*` 的 `__deref__` 展开：把接收者沿 `__deref__` 链展开到**指针**。
 *  返回 1 = `*out_expr` 是指针表达式（调用方接 `.*`）；
 *  返回 0 = 不适用（没有 `__deref__`）；-1 = 已诊断（环/超深/禁用）。 */
int sema_magic_deref_star_rewrite(sema_t *sema, ast_node_t **out_expr,
                                  const struct type_t *recv_t,
                                  ast_node_t *recv_expr, uint32_t tok_begin,
                                  uint32_t tok_end);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_SEMA_MAGIC_ */
