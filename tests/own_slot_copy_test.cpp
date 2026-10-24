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

/* 合法写法 3：遍历 own 链表用 `ref` 视图（读 `q.n` 只做 nil 判定 / 解包）。 */TEST(OwnSlotCopy, AllowsRefTraversalOfOwnList) {
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
