/* `?own *T` 等「嵌套 own 槽位」的复制回归。
 *
 * 背景（access violation 实测）：own 是 move-only，但顶层 ownership 为
 * NONE 的嵌套形态（`?own *T`、含 own 字段的聚合）此前绕过了 sema 的
 * copy 检查：
 *
 *   var p = self.n;   // 借源视图被复制成“自己拥有”的位置
 *   p = q.n;          // compound_assign_transfer 释放 p 旧块并清空 q.n
 *
 * 运行期 aggregate 赋值按“转移”语义实现（释放左值旧块 + 清空右值 own
 * 指针），前提是 sema 已保证非 fatal 的右值不携带 own 资源。嵌套 own 的
 * copy 让该前提失效 → 释放借源仍拥有的块（双释放）并把借源指针清成 nil。
 *
 * 现在初始化 / 赋值 / 成员赋值 / 构造字段四个位置都拒绝这类复制，
 * 合法写法是 `clone(...)` 或绑定 `ref` 视图。
 */
#include "test_common.h"

/* 变量初始化：`var p = self.n`（ref 接收者字段，借源视图）。 */
TEST(OwnSlotCopy, RejectsVarInitFromRefField) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var p = self.n;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var r: ref *Node = a;\n"
      "  return f(r);\n"
      "}\n",
      "own is move-only", "own_copy_init");
}

/* 显式类型标注不豁免：`var p: ?own *Node = self.n`。 */
TEST(OwnSlotCopy, RejectsAnnotatedVarInitFromRefField) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var p: ?own *Node = self.n;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var r: ref *Node = a;\n"
      "  return f(r);\n"
      "}\n",
      "own is move-only", "own_copy_init_annot");
}

/* 赋值：`p = q.n`（旧实现会先释放 p 的旧块，再清空 q.n → 崩溃）。 */
TEST(OwnSlotCopy, RejectsAssignmentFromRefField) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var p: ?own *Node = nil;\n"
      "  p = self.n;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var r: ref *Node = a;\n"
      "  return f(r);\n"
      "}\n",
      "own is move-only", "own_copy_assign");
}

/* 成员赋值：own 槽位字段只能被将亡值接管。 */
TEST(OwnSlotCopy, RejectsMemberAssignmentFromRefField) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node, other: ref *Node): i32 {\n"
      "  self.n = other.n;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var r: ref *Node = a;\n"
      "  return f(r, r);\n"
      "}\n",
      "own is move-only", "own_copy_member");
}

/* 构造字段：`.Pair{ .a = self.n }` 同样不能复制 own 槽位值。 */
TEST(OwnSlotCopy, RejectsConstructFieldFromRefField) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "struct Pair { a: ?own *Node; k: i32; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var s: Pair = .Pair{ .a = self.n, .k = 1 };\n"
      "  return s.k;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var r: ref *Node = a;\n"
      "  return f(r);\n"
      "}\n",
      "own is move-only", "own_copy_ctor");
}

/* 合法写法 1：clone() 得到独立所有权（FATAL → own 位置接管）。 */
TEST(OwnSlotCopy, AllowsCloneIntoOwnPosition) {
  clux_expect_ok(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var p: own ?own *Node = clone(self.n);\n"
      "  if (p != nil) { return 1; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var b: own *Node = new Node{ .v = 2, .n = move(a) };\n"
      "  var r: ref *Node = b;\n"
      "  return f(r);\n"
      "}\n",
      "own_copy_clone");
}

/* 合法写法 2：推断类型 `var p = clone(q.n)`。
 *
 * `clone` 产 fatal `own ?own *T`，strip_fatal 合成所有权包装类型；该 intern
 * 类型此前没登记过 → 没有 type_expr，运行期变量拿到的不是 optional，
 * `p == nil` 报 "opt is none requires an optional value"。 */
TEST(OwnSlotCopy, AllowsInferredCloneOfOwnSlotField) {
  clux_expect_ok(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func f(self: ref *Node): i32 {\n"
      "  var p = clone(self.n);\n"
      "  if (p == nil) { return 0; }\n"
      "  return 1;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *Node = new Node{ .v = 1, .n = nil };\n"
      "  var b: own *Node = new Node{ .v = 2, .n = move(a) };\n"
      "  var r: ref *Node = b;\n"
      "  if (f(r) != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "own_copy_clone_inferred");
}

/* ============================================================================
 * fatal 实参的对称规则：fatal（将亡）值必须由 **own 形参**接管。
 *
 * 传给 ref / 非 own 形参时没有任何位置接住资源——临时值（new / 构造 /
 * clone / fatal 返回值）随调用结束无人释放 → 泄漏（实测 4 字节）。
 * 校验点在 `func_shadow_call`（实参 → 形参转换的唯一入口）。
 * ========================================================================== */

TEST(OwnSlotCopy, RejectsFatalArgToRefParam) {
  clux_expect_reject(
      "struct P { x: i32; }\n"
      "func show(p: ref *P): i32 { return p.x; }\n"
      "func main(): i32 { return show(new P{ .x = 1 }); }\n",
      "must be caught by an 'own' position", "fatal_arg_ref");
}

TEST(OwnSlotCopy, RejectsFatalArgToNestedOwnParam) {
  clux_expect_reject(
      "struct Node { v: i32; n: ?own *Node; }\n"
      "func take(n: ?own *Node): i32 { return 0; }\n"
      "func main(): i32 {\n"
      "  return take(.?own *Node{ new Node{ .v = 1, .n = nil } });\n"
      "}\n",
      "must be caught by an 'own' position", "fatal_arg_nested_own");
}

TEST(OwnSlotCopy, RejectsFatalArgToByValueOwnSlotParam) {
  clux_expect_reject(
      "struct Box { p: own *i32; }\n"
      "func take(b: Box): i32 { return 0; }\n"
      "func main(): i32 { return take(.Box{ .p = new i32{1} }); }\n",
      "must be caught by an 'own' position", "fatal_arg_by_value");
}

TEST(OwnSlotCopy, RejectsMovedValueToRefParam) {
  clux_expect_reject(
      "struct P { x: i32; }\n"
      "func show(p: ref *P): i32 { return p.x; }\n"
      "func main(): i32 {\n"
      "  var a: own *P = new P{ .x = 1 };\n"
      "  return show(move(a));\n"
      "}\n",
      "must be caught by an 'own' position", "fatal_arg_move_ref");
}

/* 合法：own 形参接管 fatal 构造，调用方不再持有（无泄漏）。 */
TEST(OwnSlotCopy, AllowsFatalArgToOwnParam) {
  clux_expect_ok(
      "struct Box { p: own *i32; }\n"
      "func take(b: own Box): i32 { return b.p.*; }\n"
      "func main(): i32 {\n"
      "  if (take(.Box{ .p = new i32{7} }) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "fatal_arg_own");
}

/* 合法：own 变量可以借出（`b.&` / 方法调用），所有权仍在变量。 */
TEST(OwnSlotCopy, AllowsBorrowOfOwnAggregate) {
  clux_expect_ok(
      "struct Box {\n"
      "  p: own *i32;\n"
      "  func get(self: ref *Box): i32 { return self.p.*; }\n"
      "}\n"
      "func peek(b: ref *Box): i32 { return b.p.*; }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{7} };\n"
      "  if (b.get() != 7) { return 1; }\n"
      "  if (peek(b.&) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "fatal_arg_borrow");
}
TEST(OwnSlotCopy, AllowsRefTraversalOfOwnList) {
  clux_expect_ok(
      "struct List[T] {\n"
      "  v: T;\n"
      "  n: ?own *List[T];\n"
      "  func get(self: ref *List[T]): T { return self.v; }\n"
      "  func sum(self: ref *List[T]): T {\n"
      "    var acc: T = self.v;\n"
      "    var q: ref *List[T] = self;\n"
      "    while (q.n != nil) {\n"
      "      q = q.n.!;\n"
      "      acc += q.get();\n"
      "    }\n"
      "    return acc;\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: own *List[i32] = new List[i32]{ .v = 1, .n = nil };\n"
      "  var b: own *List[i32] = new List[i32]{ .v = 2, .n = move(a) };\n"
      "  if (b.sum() != 3) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "own_copy_refwalk");
}
