/*
 * 深层 / 自引用（递归）类型的销毁与 clone。
 *
 * 覆盖本轮补的三处：
 *   1. **struct 的 own 指针字段**必须递归进被指向值展开（`struct Outer { b: own
 *      *Box }` 之前只 FREE 掉 Box 块，Box.p 泄漏）；
 *   2. **自引用类型**（链表/树/互递归）的静态展开会无限递归 → 环边退化为
 *      `FREE_RECURSIVE`（运行期按槽位所有权递归释放，现已改生成销毁函数）；
 *      clone 的环边走生成的 clone 函数（自递归）；
 *   3. **`ptr_clear_owned_recursive` 只清指针、不进被指向值**（旧实现会改写
 *      共享堆块：`new Mid{ .i = move(inner) }` 后 `mid.i.p` 变 NULL）。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kChain3 =
    "struct Inner { p: own *i32; n: i32 }\n"
    "struct Mid { i: own *Inner; m: i32 }\n"
    "struct Outer { b: own *Mid; o: i32 }\n";

} // namespace

/* ==================== 多层 own 指针字段 ==================== */

TEST(DeepOwn, NestedOwnPointerFieldDestroy) {
  /* 两层：Outer.b: own *Box，Box.p: own *i32 —— 修复前只 FREE 掉 Box 块。 */
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "struct Outer { b: own *Box; m: i32 }\n"
      "func main(): i32 {\n"
      "  var o: own Outer = .Outer{ .b = new Box{ .p = new i32{7}, .n = 1 },"
      " .m = 1 };\n"
      "  if (o.b.p.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "deep_nested_field");
}

TEST(DeepOwn, ThreeLevelChain) {
  clux_expect_ok(std::string(kChain3) +
                 "func main(): i32 {\n"
                 "  var x: own Outer = .Outer{\n"
                 "      .b = new Mid{ .i = new Inner{ .p = new i32{7}, .n = 1 },"
                 " .m = 2 }, .o = 3 };\n"
                 "  if (x.b.m != 2) { return 1; }\n"
                 "  if (x.b.i.n != 1) { return 2; }\n"
                 "  if (x.b.i.p.* != 7) { return 3; }\n"
                 "  return 0;\n"
                 "}\n",
                 "deep_chain3");
}

TEST(DeepOwn, StagedConstructionKeepsInnerPointers) {
  /* 回归：`new Mid{ .i = move(inner) }` 曾经把 inner 的 p 清成 NULL
     （ptr_clear_owned_recursive 递归进被指向值 → 改写共享堆块）。 */
  clux_expect_ok(std::string(kChain3) +
                 "func main(): i32 {\n"
                 "  var inner: own *Inner = new Inner{ .p = new i32{7}, .n = 1 };\n"
                 "  var mid: own *Mid = new Mid{ .i = move(inner), .m = 2 };\n"
                 "  if (mid.i.n != 1) { return 1; }\n"
                 "  if (mid.i.p.* != 7) { return 2; }\n"
                 "  var x: own Outer = .Outer{ .b = move(mid), .o = 3 };\n"
                 "  if (x.b.i.p.* != 7) { return 3; }\n"
                 "  return 0;\n"
                 "}\n",
                 "deep_staged");
}

TEST(DeepOwn, SliceFieldOfOwnPointers) {
  clux_expect_ok(
      "struct Holder { s: own []own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var h: own Holder = .Holder{\n"
      "      .s = make(own *i32, 2, new i32{1}, new i32{2}), .n = 2 };\n"
      "  if (h.s[0].* != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "deep_slice_field");
}

/* ==================== 自引用（递归）类型 ==================== */

TEST(DeepOwn, RecursiveListDestroy) {
  clux_expect_ok(
      "struct Node { next: ?own *Node; v: i32 }\n"
      "func main(): i32 {\n"
      "  var n: own Node = .Node{\n"
      "      .next = .?own *Node{ new Node{\n"
      "          .next = .?own *Node{ nil }, .v = 2 } }, .v = 1 };\n"
      "  if (n.next.!.v != 2) { return 1; }\n"
      "  return 0;\n"          /* 环边 FREE_RECURSIVE 销毁整条链 */
      "}\n",
      "deep_list");
}

TEST(DeepOwn, RecursiveListClone) {
  clux_expect_ok(
      "struct Node { next: ?own *Node; v: i32 }\n"
      "func main(): i32 {\n"
      "  var c: own *Node = new Node{\n"
      "      .next = .?own *Node{ new Node{\n"
      "          .next = .?own *Node{ new Node{ .next = .?own *Node{ nil },"
      " .v = 1 } }, .v = 2 } }, .v = 3 };\n"
      "  var d: own *Node = clone(c);\n"
      "  if (d.next.!.next.!.v != 1) { return 1; }\n"
      "  if (c.next.!.v != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "deep_list_clone");
}

TEST(DeepOwn, MutuallyRecursiveTypes) {
  clux_expect_ok(
      "struct A { b: ?own *B }\n"
      "struct B { a: ?own *A; v: i32 }\n"
      "func main(): i32 {\n"
      "  var a: own A = .A{ .b = .?own *B{ new B{ .a = .?own *A{ nil },"
      " .v = 7 } } };\n"
      "  if (a.b.!.v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "deep_mutual");
}

TEST(DeepOwn, RecursiveTreeDestroy) {
  clux_expect_ok(
      "struct Tree { l: ?own *Tree; r: ?own *Tree; v: i32 }\n"
      "func main(): i32 {\n"
      "  var t: own Tree = .Tree{\n"
      "      .l = .?own *Tree{ new Tree{ .l = .?own *Tree{ nil },"
      " .r = .?own *Tree{ nil }, .v = 1 } },\n"
      "      .r = .?own *Tree{ new Tree{ .l = .?own *Tree{ nil },"
      " .r = .?own *Tree{ nil }, .v = 2 } },\n"
      "      .v = 3 };\n"
      "  if (t.l.!.v != 1) { return 1; }\n"
      "  if (t.r.!.v != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "deep_tree");
}
