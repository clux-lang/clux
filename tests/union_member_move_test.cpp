/*
 * union / cunion 成员 move 豁免 + `STORE_REINIT`（R2 恢复）。
 *
 * 规则：union / cunion 的成员与整体**共享同一块内存布局**（cunion 全 offset 0；
 * tag union 的 payload 共用 payload 区）——"局部 = 整体"。因此 `move(u.a)` 是合法
 * 的：整体进 TDZ（符号级 moved），作用域退出不再销毁整体（payload 已移出，由
 * 接管方销毁）。这也是 **cunion 指针成员的唯一释放途径**：`_ = move(c.p)`。
 *
 * 约束（只对**直接变量根** `u.member` 成立）：
 *   - union：member 槽位所有权必须是 own；cunion：成员类型含资源即可；
 *   - 经指针（`p.a` ≡ `p.*.a`）或嵌套结构（`s.u.a`）**不豁免**——整体 TDZ 会
 *     漏掉堆块/兄弟 own 槽位的销毁 → 泄漏；
 *   - 根变量有存活借用时拒绝（R3）。
 *
 * `STORE_REINIT`：move 之后的变量重新赋值（R2 恢复）时，旧内容所有权已转移
 * （内存里是陈旧字节），普通 STORE 的 `compound_assign_transfer` 会二次释放
 * → 双释放（实测 0xC0000005）。新指令只 implicit_cast + memcpy。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kU = "union U { a: own *i32; b: i32 }\n";
const char *kC = "cunion C { p: own *i32; n: i32 }\n";

} // namespace

/* ==================== union 成员 move（合法） ==================== */

TEST(UnionMemberMove, MoveOwnPayloadOut) {
  clux_expect_ok(std::string(kU) +
                 "func main(): i32 {\n"
                 "  var u: own U = .U{ .a = new i32{7} };\n"
                 "  var q: own *i32 = move(u.a);\n"
                 "  if (q.* != 7) { return 1; }\n"
                 "  return 0;\n"          /* q 释放；u 已 TDZ，不再销毁 */
                 "}\n",
                 "um_move");
}

TEST(UnionMemberMove, ExplicitDestroyPayload) {
  clux_expect_ok(std::string(kU) +
                 "func main(): i32 {\n"
                 "  var u: own U = .U{ .a = new i32{5} };\n"
                 "  _ = move(u.a);\n"     /* 显式销毁成员，整体 TDZ */
                 "  return 0;\n"
                 "}\n",
                 "um_destroy");
}

TEST(UnionMemberMove, CunionPointerFreedByMove) {
  /* 修复前：cunion 不参与自动销毁、成员 move 又被"字段不可 move"挡住 →
     指针成员**无法释放**。现在 `_ = move(c.p)` 是官方释放途径。 */
  clux_expect_ok(std::string(kC) +
                 "func main(): i32 {\n"
                 "  var c: C = .C{ .p = new i32{7} };\n"
                 "  if (c.p.* != 7) { return 1; }\n"
                 "  _ = move(c.p);\n"
                 "  return 0;\n"
                 "}\n",
                 "um_cunion_free");
}

TEST(UnionMemberMove, ReassignWholeAfterMemberMove) {
  /* 成员 move 后整体重新赋值（R2 恢复）：走 STORE_REINIT，不二次释放。 */
  clux_expect_ok(std::string(kU) +
                 "func main(): i32 {\n"
                 "  var u: own U = .U{ .a = new i32{1} };\n"
                 "  var q: own *i32 = move(u.a);\n"
                 "  if (q.* != 1) { return 1; }\n"
                 "  u = .U{ .a = new i32{2} };\n"
                 "  if (u.a.* != 2) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "um_reassign");
}

/* ==================== 整体 TDZ / 拒绝 ==================== */

TEST(UnionMemberMove, WholeValueTdzAfterMemberMove) {
  clux_expect_reject(std::string(kU) +
                     "func main(): i32 {\n"
                     "  var u: own U = .U{ .a = new i32{1} };\n"
                     "  var q: own *i32 = move(u.a);\n"
                     "  return u.b;\n"      /* 已 TDZ */
                     "}\n",
                     "used after move", "um_tdz");
}

TEST(UnionMemberMove, CunionWholeTdzAfterMemberMove) {
  /* 2026-10 放宽：cunion 是**不安全类型**（无 tag、不参与所有权运算），成员
     move 不再让整体进 TDZ —— 整体仍可读（槽位已被 TAKE_SLOT 置 0）；
     重复/错误释放由用户负责。tag union 仍严格（见 UnionMemberMove 其他用例）。 */
  clux_expect_ok(std::string(kC) +
                 "func main(): i32 {\n"
                 "  var c: C = .C{ .p = new i32{7} };\n"
                 "  _ = move(c.p);\n"
                 "  if (c.n != 0) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "um_cunion_no_tdz");
}

TEST(UnionMemberMove, DoubleMemberMoveRejected) {
  clux_expect_reject(std::string(kU) +
                     "func main(): i32 {\n"
                     "  var u: own U = .U{ .a = new i32{1} };\n"
                     "  var q: own *i32 = move(u.a);\n"
                     "  var w: own *i32 = move(u.a);\n"
                     "  return q.* + w.*;\n"
                     "}\n",
                     "used after move", "um_double");
}

TEST(UnionMemberMove, ScalarMemberMoveRejected) {
  /* 无资源成员不豁免（move 是拷贝，没有"整体移交"语义）。 */
  clux_expect_reject(std::string(kU) +
                     "func main(): i32 {\n"
                     "  var u: own U = .U{ .a = new i32{1} };\n"
                     "  var x: i32 = move(u.b);\n"
                     "  return x;\n"
                     "}\n",
                     "cannot move a struct/aggregate field or element", "um_scalar");
}

TEST(UnionMemberMove, MemberMoveThroughPointerRejected) {
  /* 经指针（p.a ≡ p.*.a）：整体 TDZ 会让 p 的堆块泄漏 → 不豁免。 */
  clux_expect_reject(std::string(kU) +
                     "func main(): i32 {\n"
                     "  var p: own *U = new U{ .a = new i32{1} };\n"
                     "  var q: own *i32 = move(p.a);\n"
                     "  return q.*;\n"
                     "}\n",
                     "cannot move a struct/aggregate field or element", "um_ptr");
}

TEST(UnionMemberMove, NestedUnionMemberMoveRejected) {
  /* 嵌套结构里的 union 成员：整体 TDZ 会漏掉兄弟 own 槽位 → 不豁免。 */
  clux_expect_reject(std::string(kU) +
                     "struct S { u: U; p: own *i32 }\n"
                     "func main(): i32 {\n"
                     "  var s: own S = .S{ .u = .U{ .a = new i32{1} },"
                     " .p = new i32{2} };\n"
                     "  var q: own *i32 = move(s.u.a);\n"
                     "  return q.*;\n"
                     "}\n",
                     "cannot move a struct/aggregate field or element", "um_nested");
}

TEST(UnionMemberMove, RefPayloadMoveRejected) {
  /* tag union 的 ref payload 是借用（槽位所有权非 own）→ 不能转移。 */
  clux_expect_reject("union U { a: ref *i32; b: i32 }\n"
                     "func main(): i32 {\n"
                     "  var x: i32 = 1;\n"
                     "  var u: U = .U{ .a = x.& };\n"
                     "  var q: ref *i32 = move(u.a);\n"
                     "  return q.*;\n"
                     "}\n",
                     "cannot move a struct/aggregate field or element", "um_ref");
}

TEST(UnionMemberMove, LiveBorrowBlocksMemberMove) {
  clux_expect_reject(std::string(kU) +
                     "func main(): i32 {\n"
                     "  var u: own U = .U{ .a = new i32{1} };\n"
                     "  var r: ref *i32 = u.a;\n"
                     "  var q: own *i32 = move(u.a);\n"
                     "  return r.* + q.*;\n"
                     "}\n",
                     "live borrow(s) still reference it", "um_borrow");
}

/* ==================== STORE_REINIT（R2 恢复） ==================== */

TEST(UnionMemberMove, MoveAggregateThenReassign) {
  /* 回归：`var c = move(b); b = .Box{…};` 曾经在重新赋值时按旧字节二次释放
     own 槽位（0xC0000005）。STORE_REINIT 只 memcpy。 */
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var c: own Box = move(b);\n"
      "  b = .Box{ .p = new i32{2}, .n = 2 };\n"
      "  if (c.p.* != 1) { return 1; }\n"
      "  if (b.p.* != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "um_reinit_struct");
}

TEST(UnionMemberMove, MovePointerThenReassign) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = move(p);\n"
      "  p = new i32{2};\n"
      "  if (q.* != 1) { return 1; }\n"
      "  if (p.* != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "um_reinit_ptr");
}
