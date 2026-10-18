/*
 * R3 派生规则：**聚合的 own 槽位被 ref 借用时，禁止 move 该聚合**。
 *
 * 由"禁止 move 仍有存活 ref 的 own"派生而来：`var r: ref *i32 = b.p`（p 是
 * own 字段）借的是 **b 拥有的堆块**；`move(b)` 会把该块交给新 owner，新 owner
 * 可能更早销毁 → 活借用悬垂。因此定义点把 b 登记为 r 的借源
 * （`b->borrow_count++`），move(b) / `_ = move(b)` 被拒绝。
 *
 * 精确边界：
 *   - 借 own 字段 / own 元素 / own inner / own payload / own 指针的被指向内容
 *     （`b.p`、`a[i]`、`o.!`、`u.a`、`b.p`（b 是 own *Box））→ 禁止 move 根变量；
 *   - `x.&` / `b.&`：借存储，存储含 own 槽位 → 禁止 move；
 *   - 嵌套路径 `o.inner.p`、重绑定 `r = b.p` 同理；
 *   - 借 **ref 字段**（`b.q`，q: ref *T）→ **不**禁止（借的是 b 之外的块）；
 *   - 借标量字段（`b.n`）→ 不禁止（存储随 b 的作用域存活，借用不会比它活得久）；
 *   - 借用变量所在作用域结束 → 借源计数递减，move 恢复可用。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";

} // namespace

/* ============================ 禁止 move ============================ */

TEST(BorrowMove, OwnFieldBorrowBlocksMove) {
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var r: ref *i32 = b.p;\n"
                     "  _ = move(b);\n"      /* 活借用：拒绝 */
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_field");
}

TEST(BorrowMove, ExplicitDestroyBlockedByBorrow) {
  /* 显式销毁 `_ = move(b)` 与普通 move 走同一条 R3 检查。 */
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
                     "  var r: ref *i32 = b.p;\n"
                     "  _ = move(b);\n"
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_destroy");
}

TEST(BorrowMove, ArrayElementBorrowBlocksMove) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{1}, new i32{2} };\n"
      "  var r: ref *i32 = a[0];\n"
      "  _ = move(a);\n"
      "  return r.*;\n"
      "}\n",
      "live borrow(s) still reference it", "bm_array");
}

TEST(BorrowMove, OptionInnerBorrowBlocksMove) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var o: own ?own *i32 = .?own *i32{ new i32{3} };\n"
      "  var r: ref *i32 = o.!;\n"
      "  _ = move(o);\n"
      "  return r.*;\n"
      "}\n",
      "live borrow(s) still reference it", "bm_option");
}

TEST(BorrowMove, UnionPayloadBorrowBlocksMove) {
  clux_expect_reject("union U { a: own *i32; b: i32 }\n"
                     "func main(): i32 {\n"
                     "  var u: own U = .U{ .a = new i32{4} };\n"
                     "  var r: ref *i32 = u.a;\n"
                     "  _ = move(u);\n"
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_union");
}

TEST(BorrowMove, PointerPathBorrowBlocksMove) {
  /* b 是 own *Box：`b.p` ≡ `b.*.p`，借的是 b 拥有的堆块里的 own 字段。 */
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own *Box = new Box{ .p = new i32{2}, .n = 2 };\n"
                     "  var r: ref *i32 = b.p;\n"
                     "  _ = move(b);\n"
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_ptr_path");
}

TEST(BorrowMove, AddrBorrowBlocksMove) {
  /* `b.&` 借的是 b 的整个存储（含 own 槽位）。 */
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
                     "  var r: ref *Box = b.&;\n"
                     "  _ = move(b);\n"
                     "  return r.n;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_addr");
}

TEST(BorrowMove, NestedPathBorrowBlocksMove) {
  clux_expect_reject(
      "struct Inner { p: own *i32; n: i32 }\n"
      "struct Outer { inner: Inner; m: i32 }\n"
      "func main(): i32 {\n"
      "  var o: own Outer = .Outer{ .inner = .Inner{ .p = new i32{5}, .n = 5 },"
      " .m = 5 };\n"
      "  var r: ref *i32 = o.inner.p;\n"
      "  _ = move(o);\n"
      "  return r.*;\n"
      "}\n",
      "live borrow(s) still reference it", "bm_nested");
}

TEST(BorrowMove, RebindBorrowBlocksMove) {
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{6}, .n = 6 };\n"
                     "  var x: i32 = 1;\n"
                     "  var r: ref *i32 = x.&;\n"
                     "  r = b.p;\n"        /* 重绑定：旧源递减、新源（b）递增 */
                     "  _ = move(b);\n"
                     "  return r.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_rebind");
}

/* ============================ 允许 move ============================ */

TEST(BorrowMove, BorrowEndedAllowsMove) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                 "  {\n"
                 "    var r: ref *i32 = b.p;\n"
                 "    if (r.* != 7) { return 1; }\n"
                 "  }\n"
                 "  _ = move(b);\n"     /* 借用已随作用域消亡 → 允许 */
                 "  return 0;\n"
                 "}\n",
                 "bm_ended");
}

TEST(BorrowMove, RefFieldBorrowAllowsMove) {
  /* 借 ref 字段（q: ref *T）：借的是聚合之外的块，不把 b 记为借源。 */
  clux_expect_ok(
      "struct Box { p: own *i32; q: ref *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 5;\n"
      "  var b: own Box = .Box{ .p = new i32{1}, .q = x.&, .n = 1 };\n"
      "  var r: ref *i32 = b.q;\n"
      "  _ = move(b);\n"
      "  if (x != 5) { return 1; }\n"
      "  if (r.* != 5) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "bm_ref_field");
}

TEST(BorrowMove, ScalarFieldBorrowAllowsMove) {
  /* 借标量字段（b.n）：存储随 b 的作用域存活，借用不会比它活得久 → 允许。 */
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{1}, .n = 9 };\n"
                 "  var r: ref *i32 = b.n.&;\n"
                 "  if (r.* != 9) { return 1; }\n"
                 "  _ = move(b);\n"
                 "  return 0;\n"
                 "}\n",
                 "bm_scalar_field");
}

TEST(BorrowMove, MoveThenBorrowThenMoveBlocked) {
  /* 先把 b move 进 c（b 进 TDZ），再从 c 借 own 字段 → 再 move(c) 被拒绝：
     借源始终是**当前持有者**变量。 */
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
                     "  var c: own Box = move(b);\n"
                     "  var r: ref *i32 = c.p;\n"
                     "  if (r.* != 1) { return 1; }\n"
                     "  _ = move(c);\n"   /* r 仍活着 → 拒绝 */
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "bm_order");
}
