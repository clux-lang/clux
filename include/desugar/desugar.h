#ifndef _H_CLUX_DESUGAR_DESUGAR_
#define _H_CLUX_DESUGAR_DESUGAR_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/arena.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "parser/lexer.h"

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
 * 典型变换（本层该做的事，示例待落地）：
 *
 *     foreach (var a of b) { <body> }
 *   ⇒
 *     var __idx_0 = 0;
 *     while (__idx_0 < len(b)) {
 *         var a = b[__idx_0];
 *         <body>
 *         __idx_0 += 1;
 *     }
 *
 * **不属于本层**的脱糖：
 *   - `defer`（**后脱糖**）：需要"函数 / 返回值 / 块作用域"这些语义结构，
 *     因此留在 sema 内，并且必须在 `comptime if` **裁剪之后**执行
 *     （见 src/sema/stmt_build.c 的 `desugar_defers`）；
 *   - `comptime if` 裁剪：需要编译期常量求值，在 sema Pass 3a。
 */
typedef struct desugar_t {
  arena_t    *arena;   /* 合成节点分配 */
  diag_buf_t *diag;    /* 诊断缓冲（NULL = 静默） */
  vec_t      *tokens;  /* token pool（诊断定位） */
  uint32_t    seq;     /* 合成名字序号（`__idx_N` 等） */
} desugar_t;

/** 对整个程序做前脱糖（遍历 prog->funcs 的全部函数定义）。 */
void desugar_program(desugar_t *ds, ast_program_t *prog);

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
