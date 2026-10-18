/*
 * **显式销毁**：`_ = move(x)`（以及丢弃将亡值 `_ = new T{…}` / `_ = clone(x)`）。
 *
 * 语义（m3-design §3.3 / ownership-refactor §7.1.2）：
 *   - `_ = move(x)`：在**当前位置**释放 x 的全部 own 槽位（与作用域退出同一套
 *     扁平槽位展开：ptr/slice → FREE，聚合 → 逐槽位），并把 x 标 moved ——
 *     作用域退出不再二次释放；
 *   - `_ = <将亡值>`（new / 构造 / clone / 返回 fatal 的调用）：生成临时变量 +
 *     赋值 + 销毁序列（同样是扁平展开，不依赖运行期 DISPOSE）；
 *   - `_ = <借用/无资源>`（ref 变量、`x.&`、字段读…）：只求值 + POP——
 *     **不得 DISPOSE**（借用不拥有堆块，释放会破坏借用源，实测 0xC0000005）；
 *   - `_ = x`（own/fatal 变量**不 move**）：拒绝——own 不可 copy；
 *   - `move(ref)`：拒绝（借用不拥有堆块）。
 *
 * 每个正例都断言 exit 0 + stderr 无 memory leak（`clux_expect_ok`）。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";

} // namespace

/* ============================ 提前销毁变量 ============================ */

TEST(ExplicitDestroy, OwnPointer) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  _ = move(p);\n"          /* 提前销毁（作用域退出不再释放） */
      "  return 0;\n"
      "}\n",
      "ed_ptr");
}

TEST(ExplicitDestroy, OwnSlice) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  _ = move(s);\n"
      "  return 0;\n"
      "}\n",
      "ed_slice");
}

TEST(ExplicitDestroy, StructWithOwnField) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
                 "  _ = move(b);\n"
                 "  return 0;\n"
                 "}\n",
                 "ed_struct");
}

TEST(ExplicitDestroy, TupleArrayOptionUnion) {
  clux_expect_ok(
      "union U { a: own *i32; b: i32 }\n"
      "func main(): i32 {\n"
      "  var t: own <own *i32, i32> = .<own *i32, i32>{ new i32{2}, 2 };\n"
      "  _ = move(t);\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{3}, new i32{4} };\n"
      "  _ = move(a);\n"
      "  var o: own ?own *i32 = .?own *i32{ new i32{5} };\n"
      "  _ = move(o);\n"
      "  var u: own U = .U{ .a = new i32{6} };\n"
      "  _ = move(u);\n"
      "  return 0;\n"
      "}\n",
      "ed_kinds");
}

TEST(ExplicitDestroy, NestedSliceOfStructs) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var s: own []Box = make(Box, 2,\n"
                 "      .Box{ .p = new i32{7}, .n = 7 },\n"
                 "      .Box{ .p = new i32{8}, .n = 8 });\n"
                 "  _ = move(s);\n"
                 "  return 0;\n"
                 "}\n",
                 "ed_slice_structs");
}

/* ============================ 丢弃将亡值 ============================ */

TEST(ExplicitDestroy, DiscardNewTemporary) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  _ = new i32{1};\n"
      "  return 0;\n"
      "}\n",
      "ed_new_temp");
}

TEST(ExplicitDestroy, DiscardConstructWithOwnSlots) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  _ = .Box{ .p = new i32{2}, .n = 2 };\n"
                 "  return 0;\n"
                 "}\n",
                 "ed_ctor_temp");
}

TEST(ExplicitDestroy, DiscardCloneKeepsSource) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var src: own Box = .Box{ .p = new i32{3}, .n = 3 };\n"
                 "  _ = clone(src);\n"       /* 丢弃 clone 的副本 */
                 "  if (src.p.* != 3) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "ed_clone_temp");
}

/* ===================== 借用 / 无资源：只求值丢弃 ===================== */

TEST(ExplicitDestroy, DiscardBorrowIsNoOp) {
  /* `_ = r` / `_ = x.&` 只是求值丢弃：借用不拥有堆块，绝不能释放借用源。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var r: ref *i32 = x.&;\n"
      "  _ = r;\n"
      "  _ = x.&;\n"
      "  if (x != 7) { return 1; }\n"
      "  if (r.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "ed_borrow");
}

TEST(ExplicitDestroy, DiscardScalarIsNoOp) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  _ = 5;\n"
      "  return 0;\n"
      "}\n",
      "ed_scalar");
}

/* ============================ 控制流 / 重用 ============================ */

TEST(ExplicitDestroy, InsideLoopAndBranch) {
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var n: i32 = 0;\n"
      "  while (n < 2) {\n"
      "    var p: own *i32 = new i32{n};\n"
      "    _ = move(p);\n"
      "    n = n + 1;\n"
      "  }\n"
      "  var b: own Box = .Box{ .p = new i32{9}, .n = 9 };\n"
      "  if (b.n == 9) { _ = move(b); }\n"
      "  return n;\n"
      "}\n",
      "ed_flow");
}

TEST(ExplicitDestroy, ReassignAfterDestroy) {
  /* 销毁后重新赋值：TDZ 恢复（R2），作用域退出释放新块。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  _ = move(p);\n"
      "  p = new i32{2};\n"
      "  if (p.* != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ed_reassign");
}

TEST(ExplicitDestroy, RefFieldNotFreed) {
  /* 含 ref 字段的聚合：销毁只释放 own 槽位，ref 借用源不受影响。 */
  clux_expect_ok(
      "struct Box { p: own *i32; q: ref *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 5;\n"
      "  var b: own Box = .Box{ .p = new i32{1}, .q = x.&, .n = 1 };\n"
      "  _ = move(b);\n"
      "  if (x != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ed_ref_field");
}

/* ============================ 负例 ============================ */

TEST(ExplicitDestroy, DiscardOwnWithoutMoveRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  _ = p;\n"                 /* own 不可 copy：必须 `_ = move(p)` */
      "  return 0;\n"
      "}\n",
      "cannot discard 'p' by copy", "ed_reject_copy");
}

TEST(ExplicitDestroy, MoveBorrowRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var r: ref *i32 = x.&;\n"
      "  _ = move(r);\n"           /* 借用不拥有堆块，不能 move */
      "  return 0;\n"
      "}\n",
      "cannot move a borrowed value", "ed_reject_move_ref");
}

TEST(ExplicitDestroy, DoubleDestroyRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  _ = move(p);\n"
      "  _ = move(p);\n"           /* TDZ：已转移，不能再次销毁 */
      "  return 0;\n"
      "}\n",
      "used after move", "ed_reject_double");
}
