/*
 * R3 借用来源（多源，2026-10）：**作用域标注即来源声明**。
 *
 * 编译期无法精确判定借用的来源（停机问题）——例如
 *     func pick(a: ref *i32, b: ref *i32): '<a,b> ref *i32 {
 *         if (a.* == 1) { return a; } else { return b; }
 *     }
 * 返回值可能来自 a 也可能来自 b。因此编译器**保守**处理：返回值同时继承
 * 返回标注里每个参数名对应**实参**的来源；调用点登记全部来源，任一被 move
 * 即拒绝（R3）。标注只写 `'<a>` 时只登记 a。
 *
 * 覆盖：
 *   - `var r: ref *T = take(b.p)`（经函数调用借 own 槽位）→ move(b) 拒绝；
 *   - 分支双来源 `'<a,b>`：m6 的 move 任一来源都拒绝；无关变量可 move；
 *   - 单来源标注 `'<a>`：未标注的参数来源不登记（保守度由标注决定）；
 *   - 链式借用 `var r2: ref *T = r` 继承 r 的全部来源；
 *   - 内建 `data(s)` 借 s 的缓冲区 → 登记 s 的来源；
 *   - 借用作用域结束 → move 恢复；重绑定到调用结果同样登记。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kPick =
    "func pick(a: ref *i32, b: ref *i32): '<a,b> ref *i32 {\n"
    "    if (a.* == 1) { return a; } else { return b; }\n"
    "}\n";

} // namespace

/* ==================== 经函数调用借用 ==================== */

TEST(BorrowSources, BorrowViaCallBlocksMove) {
  clux_expect_reject(
      "struct Box { p: own *i32; n: i32 }\n"
      "func take(r: ref *i32): '<r> ref *i32 { return r; }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  var r: '<b> ref *i32 = take(b.p);\n"
      "  _ = move(b);\n"          /* 来源经返回标注映射到实参 b.p → b */
      "  return r.*;\n"
      "}\n",
      "live borrow(s) still reference it", "bs_call");
}

TEST(BorrowSources, BorrowViaCallEndedAllowsMove) {
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func take(r: ref *i32): '<r> ref *i32 { return r; }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  {\n"
      "    var r: '<b> ref *i32 = take(b.p);\n"
      "    if (r.* != 7) { return 1; }\n"
      "  }\n"
      "  _ = move(b);\n"          /* 借用已随作用域消亡 */
      "  return 0;\n"
      "}\n",
      "bs_call_ended");
}

/* ==================== 多来源（分支返回） ==================== */

TEST(BorrowSources, BranchyReturnInheritsBothSources) {
  /* 无关变量可 move；两个来源都在集合里（此处先 move 无关的 p）。 */
  clux_expect_ok(std::string(kPick) +
                 "func main(): i32 {\n"
                 "  var x: i32 = 1;\n"
                 "  var y: i32 = 2;\n"
                 "  var r: ref *i32 = pick(x.&, y.&);\n"
                 "  var p: own *i32 = new i32{9};\n"
                 "  _ = move(p);\n"     /* 与借用无关 → 允许 */
                 "  if (r.* != 1) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "bs_branch_ok");
}

TEST(BorrowSources, SecondSourceBlocksMove) {
  clux_expect_reject(std::string(kPick) +
                     "func main(): i32 {\n"
                     "  var b: own *i32 = new i32{2};\n"
                     "  var x: i32 = 1;\n"
                     "  var r: ref *i32 = pick(x.&, b);\n"
                     "  _ = move(b);\n"   /* b 是来源之一 → 拒绝 */
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bs_second");
}

TEST(BorrowSources, SingleAnnotationOnlyRegistersThatSource) {
  clux_expect_ok(
      "func only_a(a: ref *i32, b: ref *i32): '<a> ref *i32 { return a; }\n"
      "func main(): i32 {\n"
      "  var b: own *i32 = new i32{2};\n"
      "  var x: i32 = 1;\n"
      "  var r: ref *i32 = only_a(x.&, b);\n"
      "  _ = move(b);\n"            /* 标注不含 b → 允许 */
      "  if (r.* != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "bs_single_annot");
}

/* ==================== 链式 / 内建 / 重绑定 ==================== */

TEST(BorrowSources, ChainedBorrowInheritsSources) {
  clux_expect_reject(std::string(kPick) +
                     "func main(): i32 {\n"
                     "  var b: own *i32 = new i32{2};\n"
                     "  var x: i32 = 1;\n"
                     "  var r: ref *i32 = pick(x.&, b);\n"
                     "  var r2: ref *i32 = r;\n"   /* 继承 {x, b} */
                     "  _ = move(b);\n"
                     "  return r2.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bs_chain");
}

TEST(BorrowSources, DataBuiltinBorrowsBuffer) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 2, 1, 2);\n"
      "  var p: ref *i32 = data(s);\n"   /* 借用 s 的缓冲区 */
      "  _ = move(s);\n"                 /* 应拒绝 */
      "  return p.*;\n"
      "}\n",
      "live borrow(s) still reference it", "bs_data");
}

TEST(BorrowSources, RebindToCallResultRegistersSources) {
  clux_expect_reject(std::string(kPick) +
                     "func main(): i32 {\n"
                     "  var b: own *i32 = new i32{2};\n"
                     "  var x: i32 = 1;\n"
                     "  var r: ref *i32 = x.&;\n"
                     "  r = pick(x.&, b);\n"       /* 重绑定到调用结果 */
                     "  _ = move(b);\n"
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bs_rebind");
}
