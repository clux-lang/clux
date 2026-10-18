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
 * 当前包含的变换：
 *   - **defer 脱糖**：`defer [|捕获|] { ... }` →
 *       匿名局部函数定义（`__defer_N`）+ 各退出点（块尾 / return /
 *       break / continue）的逆序调用语句。改写后 AST 中不再有 AST_DEFER。
 *
 * 为什么单独成层：defer 的语义只需要"块的嵌套结构"，不需要类型/符号信息；
 * 放在 sema 里会让 Pass 3a 的符号注册与语法糖改写纠缠（且 sema 需要同时
 * 维护 scope->defers 列表）。独立成层后，sema 只看到"普通函数定义 + 普通
 * 调用语句"。
 *
 * 与 `comptime if` 的边界：`comptime if` 的裁剪需要**编译期常量求值**
 * （comptime var 的值），属于语义层，因此仍在 sema Pass 3a 做。为了不让
 * 两者互相纠缠（裁剪可能删除分支，而分支里的 defer 闭包名会被外层退出点
 * 引用），本层对"出现在 comptime if 分支内的 defer"报错——把它写在外面即可。
 */
typedef struct desugar_t {
  arena_t    *arena;   /* 合成节点分配 */
  diag_buf_t *diag;    /* 诊断缓冲（NULL = 静默） */
  vec_t      *tokens;  /* token pool（诊断定位） */
  uint32_t    defer_seq; /* `__defer_N` 命名序号 */
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
  ds.defer_seq = 0;
  return ds;
}

#ifdef __cplusplus
}
#endif
#endif
