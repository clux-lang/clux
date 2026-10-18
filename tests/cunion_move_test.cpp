/*
 * cunion 成员 move 放宽（2026-10）：**不受路径限制、不进 TDZ**。
 *
 * 理由：cunion 是**不安全类型**——无 tag、成员共享 offset 0、不参与所有权运算，
 * 编译器不保证重复释放/错误释放能被发现。因此对它的成员 move 采用"用户自负"
 * 语义：
 *   - 任意访问路径都允许：`_ = move(c.p)` / `_ = move(s.c.p)` / `_ = move(p.c.p)`；
 *   - **不进入 TDZ**：整体仍可读、可再次 move；
 *   - 运行期是**真转移**（TAKE_SLOT：读取槽位值后把槽位 memset 0）——重复 move
 *     变成对 NULL 的 no-op，不留悬垂指针。
 * tag union 仍是严格语义（直接变量根 → 整体 TDZ），不受影响。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kC = "cunion C { p: own *i32; n: i32 }\n";

} // namespace

/* ==================== 任意路径都能释放 ==================== */

TEST(CunionMove, DirectMember) {
  clux_expect_ok(std::string(kC) +
                 "func main(): i32 {\n"
                 "  var c: C = .C{ .p = new i32{7} };\n"
                 "  _ = move(c.p);\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_direct");
}

TEST(CunionMove, NestedInStruct) {
  clux_expect_ok(std::string(kC) +
                 "struct S { c: C; m: i32 }\n"
                 "func main(): i32 {\n"
                 "  var s: S = .S{ .c = .C{ .p = new i32{7} }, .m = 1 };\n"
                 "  if (s.c.p.* != 7) { return 1; }\n"
                 "  _ = move(s.c.p);\n"
                 "  if (s.m != 1) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_nested");
}

TEST(CunionMove, ThroughPointer) {
  clux_expect_ok(std::string(kC) +
                 "struct S { c: C; m: i32 }\n"
                 "func main(): i32 {\n"
                 "  var p: own *S = new S{ .c = .C{ .p = new i32{9} },"
                 " .m = 2 };\n"
                 "  _ = move(p.c.p);\n"
                 "  if (p.m != 2) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_ptr");
}

TEST(CunionMove, DeeperPath) {
  clux_expect_ok(std::string(kC) +
                 "struct Inner { c: C }\n"
                 "struct Outer { in: Inner; k: i32 }\n"
                 "func main(): i32 {\n"
                 "  var o: own *Outer = new Outer{\n"
                 "      .in = .Inner{ .c = .C{ .p = new i32{5} } }, .k = 3 };\n"
                 "  _ = move(o.in.c.p);\n"
                 "  if (o.k != 3) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_deep");
}

/* ==================== 不进 TDZ：整体仍可读 / 可再 move ==================== */

TEST(CunionMove, RootStillUsableAfterMove) {
  clux_expect_ok(std::string(kC) +
                 "func main(): i32 {\n"
                 "  var c: C = .C{ .p = new i32{7} };\n"
                 "  _ = move(c.p);\n"
                 "  if (c.n != 0) { return 1; }\n"   /* 槽位已置 0 */
                 "  return 0;\n"
                 "}\n",
                 "cm_no_tdz");
}

TEST(CunionMove, RepeatedMoveIsNoop) {
  /* 真转移（TAKE_SLOT 置零）：重复 move 读到 NULL → no-op，不二次释放。 */
  clux_expect_ok(std::string(kC) +
                 "func main(): i32 {\n"
                 "  var c: C = .C{ .p = new i32{7} };\n"
                 "  _ = move(c.p);\n"
                 "  _ = move(c.p);\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_repeat");
}

TEST(CunionMove, MoveIntoOwnThenDiscardSlot) {
  /* 先转移到 own 变量（槽位置零），再 move 同一成员 → no-op，不双释放。 */
  clux_expect_ok(std::string(kC) +
                 "struct S { c: C; m: i32 }\n"
                 "func main(): i32 {\n"
                 "  var s: S = .S{ .c = .C{ .p = new i32{7} }, .m = 1 };\n"
                 "  var q: own *i32 = move(s.c.p);\n"
                 "  if (q.* != 7) { return 1; }\n"
                 "  _ = move(s.c.p);\n"
                 "  return 0;\n"
                 "}\n",
                 "cm_into_own");
}

/* ==================== tag union 语义不受影响 ==================== */

TEST(CunionMove, TagUnionStillTdz) {
  clux_expect_reject(
      "union U { a: own *i32; b: i32 }\n"
      "func main(): i32 {\n"
      "  var u: own U = .U{ .a = new i32{7} };\n"
      "  var q: own *i32 = move(u.a);\n"
      "  return u.b;\n"     /* tag union：整体 TDZ，仍拒绝 */
      "}\n",
      "used after move", "cm_union_tdz");
}

TEST(CunionMove, TagUnionThroughPointerStillRejected) {
  clux_expect_reject(
      "union U { a: own *i32; b: i32 }\n"
      "func main(): i32 {\n"
      "  var p: own *U = new U{ .a = new i32{7} };\n"
      "  var q: own *i32 = move(p.a);\n"
      "  return q.*;\n"
      "}\n",
      "cannot move a struct/aggregate field or element", "cm_union_ptr");
}
