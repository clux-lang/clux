/*
 * tag union 携带所有权指针（own *T / own []T / 含 own 槽位的聚合）——
 * 销毁序列、所有权流动闸门的回归测试。
 *
 * 覆盖点（docs/ownership-refactor.md §0 / §7）：
 *   - 作用域退出点按 **tag 条件** 只释放当前 member 的 own payload
 *     （`if (v is m) { <payload 销毁序列> }`，sema 生成、compiler 展开）；
 *   - 非活跃 tag 的 own member 不得被释放（否则 free 到垃圾地址）；
 *   - ref member 不得进销毁序列（借用目标不是自己的）；
 *   - 嵌套（union 里 struct、struct 里 union、union 里 union）逐层展开；
 *   - move / own 形参 / fatal 返回 / 循环 / break / continue 各退出路径；
 *   - 堆上的 union（`new U{...}` → `own *U`）先释放 payload 再释放块；
 *   - tag 宽度跨过 128 边界（IS_TAG / tag 哨兵的宽度一致）；
 *   - 重新赋值：联合体变量 / 结构体字段覆盖旧值时释放旧值；
 *   - 所有权闸门：FATAL 不得进非 own 位置、own 不可 copy、
 *     ref / own 变量不得塞进 own 槽位（构造与字段赋值两条路径）。
 *
 * 每个正例都断言"退出码 0 + stderr 无 memory leak"——销毁序列漏一条就会
 * 留下未释放的堆块，多释放一条则 free 到非堆地址（访问违例）。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "test_common.h"

namespace {

const char *kU = "union U { p: own *i32; n: i32 }\n";

/* 本地别名：断言"退出 0 + 无 memory leak" / 编译期拒绝 + 诊断文本。
   实现在 tests/test_common.h（clux_run_capture / clux_expect_ok /
   clux_expect_reject），供多个所有权测试文件共用。 */
using ::clux_expect_ok;
using ::clux_expect_reject;

inline void expect_ok(const std::string &src, const std::string &tag) {
  clux_expect_ok(src, tag);
}
inline void expect_reject(const std::string &src, const char *needle,
                          const std::string &tag) {
  clux_expect_reject(src, needle, tag);
}

} // namespace

/* ============================ 正例 ============================ */

TEST(TagUnionOwn, SingleOwnMemberFreedAtScopeExit) {
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var u: own U = .U{ .p = new i32{42} };\n"
            "  if (u is p != true) { return 1; }\n"
            "  if (u.p.* != 42) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_single");
}

TEST(TagUnionOwn, OnlyActiveTagIsFreed) {
  /* 两个 own member：a 的 tag 是 m0、b 的 tag 是 m1。销毁序列必须按 tag
     条件释放——错释放另一个 member 会 free 到联合体里的整数垃圾地址。 */
  expect_ok("union U { a: own *i32; b: own *i32 }\n"
            "func main(): i32 {\n"
            "  var u1: own U = .U{ .a = new i32{11} };\n"
            "  var u2: own U = .U{ .b = new i32{22} };\n"
            "  if (u1 is a != true) { return 1; }\n"
            "  if (u2 is a != false) { return 2; }\n"
            "  if (u1.a.* != 11) { return 3; }\n"
            "  if (u2.b.* != 22) { return 4; }\n"
            "  return 0;\n"
            "}\n",
            "tu_two_tags");
}

TEST(TagUnionOwn, StructPayloadDestroyedRecursively) {
  expect_ok("struct Box { p: own *i32; n: i32 }\n"
            "union V { b: Box; k: i32 }\n"
            "func main(): i32 {\n"
            "  var v: own V = .V{ .b = .Box{ .p = new i32{9}, .n = 3 } };\n"
            "  if (v is b != true) { return 1; }\n"
            "  if (v.b.p.* != 9) { return 2; }\n"
            "  if (v.b.n != 3) { return 3; }\n"
            "  return 0;\n"
            "}\n",
            "tu_struct_payload");
}

TEST(TagUnionOwn, UnionInsideStructDestroyed) {
  expect_ok("union U { p: own *i32; n: i32 }\n"
            "struct S { u: U; tag: i32 }\n"
            "func main(): i32 {\n"
            "  var s: own S = .S{ .u = .U{ .p = new i32{5} }, .tag = 1 };\n"
            "  if (s.u is p != true) { return 1; }\n"
            "  if (s.u.p.* != 5) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_in_struct");
}

TEST(TagUnionOwn, NestedUnionDestroyed) {
  expect_ok("union Inner { p: own *i32; n: i32 }\n"
            "union Outer { i: Inner; k: i32 }\n"
            "func main(): i32 {\n"
            "  var o: own Outer = .Outer{ .i = .Inner{ .p = new i32{6} } };\n"
            "  if (o is i != true) { return 1; }\n"
            "  if (o.i is p != true) { return 2; }\n"
            "  if (o.i.p.* != 6) { return 3; }\n"
            "  return 0;\n"
            "}\n",
            "tu_nested");
}

TEST(TagUnionOwn, MoveTransfersOwnershipOnce) {
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var u: own U = .U{ .p = new i32{77} };\n"
            "  var m: own U = move(u);\n"
            "  if (m is p != true) { return 1; }\n"
            "  if (m.p.* != 77) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_move");
}

TEST(TagUnionOwn, OwnParameterConsumesValue) {
  expect_ok(std::string(kU) +
            "func take(u: own U): i32 {\n"
            "  if (u is p != true) { return -1; }\n"
            "  return u.p.* + 1;\n"
            "}\n"
            "func main(): i32 {\n"
            "  var u: own U = .U{ .p = new i32{41} };\n"
            "  if (take(move(u)) != 42) { return 1; }\n"
            "  return 0;\n"
            "}\n",
            "tu_param");
}

TEST(TagUnionOwn, FatalReturnTransfers) {
  expect_ok(std::string(kU) +
            "func mk(): fatal U {\n"
            "  return move(.U{ .p = new i32{8} });\n"
            "}\n"
            "func main(): i32 {\n"
            "  var u: own U = mk();\n"
            "  if (u is p != true) { return 1; }\n"
            "  if (u.p.* != 8) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_fatal_return");
}

TEST(TagUnionOwn, LoopFreesEachIteration) {
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var i: i32 = 0;\n"
            "  while (i < 3) {\n"
            "    var u: own U = .U{ .p = new i32{i} };\n"
            "    if (u is p != true) { return 1; }\n"
            "    if (u.p.* != i) { return 2; }\n"
            "    i = i + 1;\n"
            "  }\n"
            "  return 0;\n"
            "}\n",
            "tu_loop");
}

TEST(TagUnionOwn, BreakAndContinueFreeLocalUnion) {
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var i: i32 = 0;\n"
            "  while (i < 4) {\n"
            "    var u: own U = .U{ .p = new i32{i} };\n"
            "    if (u.p.* != i) { return 1; }\n"
            "    i = i + 1;\n"
            "    if (i == 2) { continue; }\n"
            "    if (i == 3) { break; }\n"
            "  }\n"
            "  return 0;\n"
            "}\n",
            "tu_break_continue");
}

TEST(TagUnionOwn, HeapUnionReleasedPayloadThenBlock) {
  /* new U{...} → fatal *U：销毁序列先 `FREE up.p`（payload）再 `FREE up`
     （堆块）；成员访问 `up.p` 通过指针自动解引用，判 tag 需显式 `up.*`。 */
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var up: own *U = new U{ .p = new i32{33} };\n"
            "  if (up.* is p != true) { return 1; }\n"
            "  if (up.p.* != 33) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_heap");
}

TEST(TagUnionOwn, LargeTagWidthCrossesByteBoundary) {
  /* 130 个 member → tag 宽度 2 字节：构造的 tag 哨兵（PUSH_I16）与销毁序列
     的 IS_TAG 必须一致，否则条件释放落到错误 member 上。 */
  std::string src = "union Big {\n";
  for (int i = 0; i < 129; i++)
    src += "  m" + std::to_string(i) + ": i32;\n";
  src += "  m129: own *i32\n}\n";
  src += "func main(): i32 {\n"
         "  var b: own Big = .Big{ .m129 = new i32{129} };\n"
         "  if (b is m129 != true) { return 1; }\n"
         "  if (b is m0 != false) { return 2; }\n"
         "  if (b.m129.* != 129) { return 3; }\n"
         "  return 0;\n"
         "}\n";
  expect_ok(src, "tu_big_tag");
}

TEST(TagUnionOwn, RefMemberIsNotFreed) {
  /* ref member 是借用：销毁当前 tag 为 p 时只释放 p；旧值 tag 为 r 时
     重新赋值不得 free 借用目标（曾实测 0xC0000005）。 */
  expect_ok("union H { r: ref *i32; p: own *i32 }\n"
            "func main(): i32 {\n"
            "  var x: i32 = 5;\n"
            "  var h: own H = .H{ .r = x.& };\n"
            "  h = .H{ .p = new i32{4} };\n"
            "  if (h is p != true) { return 1; }\n"
            "  if (h.p.* != 4) { return 2; }\n"
            "  if (x != 5) { return 3; }\n"
            "  return 0;\n"
            "}\n",
            "tu_ref_member");
}

TEST(TagUnionOwn, SliceMemberReleased) {
  expect_ok("union S { s: own []i32; n: i32 }\n"
            "func main(): i32 {\n"
            "  var v: own S = .S{ .s = make(i32, 3, 7, 8, 9) };\n"
            "  if (v is s != true) { return 1; }\n"
            "  if (len(v.s) != 3u64) { return 2; }\n"
            "  if (v.s[0] != 7) { return 3; }\n"
            "  return 0;\n"
            "}\n",
            "tu_slice_member");
}

TEST(TagUnionOwn, InferredVariableTakesOver) {
  /* 不写 own 的推断绑定：构造产 FATAL，变量接管后符号层记 OWN，
     作用域退出仍有销毁序列（否则静默泄漏）。 */
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var u = .U{ .p = new i32{1} };\n"
            "  if (u.p.* != 1) { return 1; }\n"
            "  return 0;\n"
            "}\n",
            "tu_infer");
}

TEST(TagUnionOwn, StructFieldReassignFreesOldPayload) {
  expect_ok("union U { p: own *i32; n: i32 }\n"
            "struct S { u: U; tag: i32 }\n"
            "func main(): i32 {\n"
            "  var s: own S = .S{ .u = .U{ .p = new i32{1} }, .tag = 1 };\n"
            "  s.u = .U{ .p = new i32{2} };\n"
            "  if (s.u is p != true) { return 1; }\n"
            "  if (s.u.p.* != 2) { return 2; }\n"
            "  return 0;\n"
            "}\n",
            "tu_field_reassign");
}

TEST(TagUnionOwn, InternKeyIncludesSlotOwnership) {
  /* 成员名 / payload 相同、只有槽位所有权不同 → 必须是**两个**类型实例。
     去重键漏掉 ownership 时，后定义者会继承前者的槽位所有权：B 的 ref
     member 被当成 own（构造直接报 "own slot"）或销毁释放借用目标。 */
  expect_ok("union A { p: own *i32; n: i32 }\n"
            "union B { p: ref *i32; n: i32 }\n"
            "func main(): i32 {\n"
            "  var x: i32 = 7;\n"
            "  var b: '<*> B = .B{ .p = x.& };\n"
            "  if (b.p.* != 7) { return 1; }\n"
            "  if (x != 7) { return 2; }\n"
            "  var a: own A = .A{ .p = new i32{9} };\n"
            "  if (a.p.* != 9) { return 3; }\n"
            "  return 0;\n"
            "}\n",
            "tu_intern_own");
}

TEST(TagUnionOwn, ClosureMoveCaptureOfUnionReleased) {
  /* 闭包按值捕获 union（move 捕获 + move 绑定闭包）→ 销毁序列按捕获槽的
     **type 对象**展开：`if (f.bb is p) { FREE f.bb.p }`。旧实现只发
     `FREE f.bb`，对聚合是空操作 → 捕获块内嵌堆块泄漏。 */
  expect_ok(std::string(kU) +
            "func main(): i32 {\n"
            "  var a: own U = .U{ .p = new i32{7} };\n"
            "  var f = move(func |(bb: own U = move(a))| get(): i32 {\n"
            "    if (bb is p != true) { return -1; }\n"
            "    return bb.p.*;\n"
            "  });\n"
            "  if (f() != 7) { return 1; }\n"
            "  return 0;\n"
            "}\n",
            "tu_closure_cap");
}

TEST(TagUnionOwn, ClosureMoveCaptureOfStructWithOwnFieldReleased) {
  expect_ok("struct Box { p: own *i32; n: i32 }\n"
            "func main(): i32 {\n"
            "  var a: own Box = .Box{ .p = new i32{7}, .n = 3 };\n"
            "  var f = move(func |(bb: own Box = move(a))| get(): i32 {\n"
            "    return bb.n;\n"
            "  });\n"
            "  if (f() != 3) { return 1; }\n"
            "  return 0;\n"
            "}\n",
            "tu_closure_cap_struct");
}

TEST(TagUnionOwn, NonOwnCaptureOfDyingValueRejected) {
  /* 捕获槽也是槽位：非 own 捕获槽收 FATAL = 无人接住 → 拒绝。 */
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var a: own U = .U{ .p = new i32{7} };\n"
                "  var f = move(func |(bb: U = move(a))| get(): i32 {\n"
                "    return 0;\n"
                "  });\n"
                "  return 0;\n"
                "}\n",
                "must be an own capture",
                "tu_reject_capture");
}

/* ============================ 负例 ============================ */

TEST(TagUnionOwn, NonOwnPositionRejectsDyingValue) {
  /* 构造含 own 槽位的 union 产 FATAL；NONE 位置必须拒绝（否则无人释放）。 */
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var u: U = .U{ .p = new i32{1} };\n"
                "  return 0;\n"
                "}\n",
                "dying value (FATAL)",
                "tu_reject_nonown");
}

TEST(TagUnionOwn, CopyOfOwnUnionRejected) {
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var a: own U = .U{ .p = new i32{1} };\n"
                "  var b: U = a;\n"
                "  return 0;\n"
                "}\n",
                "move-only",
                "tu_reject_copy");
}

TEST(TagUnionOwn, RefIntoOwnSlotRejected) {
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var x: i32 = 5;\n"
                "  var r: ref *i32 = x.&;\n"
                "  var u: own U = .U{ .p = r };\n"
                "  return 0;\n"
                "}\n",
                "own slot must take over",
                "tu_reject_ref_slot");
}

TEST(TagUnionOwn, OwnCopyIntoOwnSlotRejected) {
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var p: own *i32 = new i32{1};\n"
                "  var u: own U = .U{ .p = p };\n"
                "  return 0;\n"
                "}\n",
                "own slot must take over",
                "tu_reject_own_copy");
}

TEST(TagUnionOwn, RefAssignedToOwnMemberRejected) {
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var x: i32 = 5;\n"
                "  var r: ref *i32 = x.&;\n"
                "  var u: own U = .U{ .p = new i32{1} };\n"
                "  u.p = r;\n"
                "  return 0;\n"
                "}\n",
                "own slot must take over",
                "tu_reject_assign_ref");
}

TEST(TagUnionOwn, OwnCopyAssignedToOwnMemberRejected) {
  expect_reject(std::string(kU) +
                "func main(): i32 {\n"
                "  var q: own *i32 = new i32{2};\n"
                "  var u: own U = .U{ .p = new i32{1} };\n"
                "  u.p = q;\n"
                "  return 0;\n"
                "}\n",
                "own slot must take over",
                "tu_reject_assign_copy");
}
