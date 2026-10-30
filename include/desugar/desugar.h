#ifndef _H_CLUX_DESUGAR_DESUGAR_
#define _H_CLUX_DESUGAR_DESUGAR_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/arena.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"

/**
 * 前脱糖（front-end desugaring）——**parser 之后、sema 之前**的一层纯 AST
 * 变换（2026-10）。
 *
 * 约束：这一层**只做语法结构改写**，不做任何语义上下文绑定——
 *   - 不查符号表、不做类型检查、不建 sema 作用域、不分配函数 id；
 *   - 只依赖 AST 的结构事实（块/循环/函数的嵌套关系）与 arena；
 *   - 因此可以独立测试（`tests/desugar_test.cpp` 直接对 AST 断言）。
 *
 * 这层**不认识语义概念**：不知道"闭包 / 函数 / 返回值 / 作用域"。
 *
 * 已落地的变换：`foreach`（for-each 语法糖）
 *
 *     foreach ([var] a [: T] of b) { <body> }
 *
 * 迭代器 `b` **不是裸标识符**时（调用 / 切片 / 成员访问…先求值一次，落临时变量）：
 *
 *     {
 *         var __arr_N = b;
 *         for (var __idx_N: usize = 0usize;
 *              __idx_N < len(__arr_N);
 *              __idx_N = __idx_N + 1usize) {
 *             var a [: T] = __arr_N[__idx_N];    // `var` 形态：每轮新声明
 *             // 或者（无 `var` 形态）：a = __arr_N[__idx_N];
 *             <body>
 *         }
 *     }
 *
 * 迭代器 `b` 是**裸标识符**时，条件与下标直接引用 `b`（不复制、也不多包一层块）：
 *
 *     for (var __idx_N: usize = 0usize;
 *          __idx_N < len(b);
 *          __idx_N = __idx_N + 1usize) {
 *         var a [: T] = b[__idx_N];
 *         <body>
 *     }
 *
 * **为什么步进放在 `for` 的 update 段，而不是 `while` 体尾**：`continue` 跳向
 * update 段（for）而不是循环条件（while 体尾的 `__idx_N = __idx_N + 1` 会被
 * 跳过 → 死循环）。展开因此落在 AST_FOR 上，而不是 AST_WHILE。
 *
 * **为什么裸标识符迭代器不落临时变量**：`var __arr_N = s` 会多一次复制（并让
 * 迭代器的别名关系改变），而 `len(s)` / `s[i]` 只是读。代价是"迭代器只求值
 * 一次"只对非标识符成立——裸标识符按名字逐轮读取，所以循环体内修改该容器会
 * 影响后续迭代（想遍历快照就先 `var copy = v;` 再遍历 copy）。
 *
 * **为什么非标识符形态外面再包一个块**：`__arr_N` 是展开内部的临时名，不该泄漏
 * 到用户作用域——否则同一块里两个 foreach 的临时变量都活到块尾。块作用域把
 * 它的生命周期精确圈在循环内（`__idx_N` 本来就在 for 作用域里）。
 *
 * **合成节点一律继承源 `foreach` 的 token 区间**（诊断指向用户的代码），
 * 运算符（`<` / `+` / `=`）用 `arena_token_symbol` 在 arena 里合成——与 parser
 * 合成 `<<`/`>>` 的机制同一套，不需要往 token pool 里塞 token，也不存在
 * "合成 token 没有源码位置"的问题。
 *
 * 合成节点一律**继承源 `foreach` 的 token 区间**（诊断指向用户的代码），
 * 运算符（`<` / `+` / `=`）用 `arena_token_symbol` 在 arena 里合成——与 parser
 * 合成 `<<`/`>>` 的机制同一套，不需要往 token pool 里塞 token，也不存在
 * "合成 token 没有源码位置"的问题。
 *
 * **不属于本层**的脱糖：
 *   - `defer`（**后脱糖**）：需要"函数 / 返回值 / 块作用域"这些语义结构，
 *     因此留在 sema 内，并且必须在 `comptime if` **裁剪之后**执行
 *     （见 src/sema/stmt_build.c 的 `desugar_defers`）；
 *   - `comptime if` 裁剪：需要编译期常量求值，在 sema Pass 3a。
 */
typedef struct desugar_t {
  arena_t    *arena;  /* 合成节点 / 合成 token 分配 */
  diag_buf_t *diag;   /* 诊断缓冲（NULL = 静默） */
  vec_t      *tokens; /* token pool（只读：合成节点的源码定位） */
  uint32_t    seq;    /* 合成名字序号（`__arr_N` / `__idx_N`） */
} desugar_t;

/** 对整个程序做前脱糖（遍历 prog->funcs 的全部函数定义）。 */
void desugar_program(desugar_t *ds, ast_program_t *prog);

/**
 * 对**单个函数定义**做前脱糖（M7 泛型实例：从 token 区间重新解析出的
 * 函数副本不经过 desugar_program，需要单独展开 body 内的 foreach）。
 * 只做函数体/参数/返回类型里的结构改写，不处理装饰器（调用方负责拒绝
 * 泛型定义上的装饰器）。
 */
void desugar_func(desugar_t *ds, ast_func_def_t *fn);

/** 便捷构造（不接管所有权）。 */
static inline desugar_t desugar_make(arena_t *arena, diag_buf_t *diag,
                                     vec_t *tokens) {
  desugar_t ds;
  ds.arena = arena;
  ds.diag = diag;
  ds.tokens = tokens;
  ds.seq = 0;
  return ds;
}

#ifdef __cplusplus
}
#endif
#endif
