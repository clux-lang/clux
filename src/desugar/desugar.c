#include "desugar/desugar.h"

#include <string.h>

#include "parser/ast_block.h"
#include "parser/ast_func_def.h"
#include "parser/ast_walk.h"

/* ===========================================================================
 * 前脱糖（front-end desugaring）——parser 之后、sema 之前的**纯 AST 变形**
 *
 * 这一层**不认识**语义概念：不知道"闭包"、"函数"、"返回值"、"作用域"，
 * 也不查符号表、不做类型检查。它只按表面的语法形状改写 AST。
 *
 * 典型变换（示例，待落地）：
 *
 *     foreach (var a of b) { <body> }
 *   =>
 *     var __idx_0 = 0;
 *     while (__idx_0 < len(b)) {
 *         var a = b[__idx_0];
 *         <body>
 *         __idx_0 += 1;
 *     }
 *
 * 与之相对，**后脱糖**（defer -> 闭包 + 退出点调用）发生在 sema 里、且必须在
 * `comptime if` 裁剪**之后**（src/sema/stmt_build.c 的 desugar_defers）：
 * 它需要"函数 / 返回值 / 块作用域"这些语义结构，因此不属于本层。
 * =========================================================================== */

void desugar_program(desugar_t *ds, ast_program_t *prog) {
  if (!ds || !prog) return;
  /* 当前尚无前脱糖变换（foreach 等待落地，见文件头示例）。 */
  (void)ds;
  (void)prog;
}