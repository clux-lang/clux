/*
 * 两处修复的回归：
 *
 * 1. **nil 比较不再只允许变量**（原限制是 bug）：字段 / 元素 / 解引用 / 解包链 /
 *    调用结果都能和 nil 比较（编译为 OPT_IS_NONE tag 指令）。链表判空因此可以
 *    直接写 `n.next != nil`（这也是生成递归销毁函数/循环所需的判定形态）。
 *
 * 2. **提前 return 不再销毁"后面才声明"的变量**：Pass 3a 预注册块内全部变量，
 *    旧实现按"作用域里所有符号"收集销毁序列 → 提前 return 的路径里出现
 *    `PUSH "p"`（p 尚未 DEFINE）→ 运行期 "undefined variable"。修复：符号带
 *    `declared` 标记（Pass 3b 走到定义点才置位），销毁只收集已声明的变量。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kNode = "struct Node { next: ?own *Node; v: i32 }\n";
const char *kPick = "func pick(): ?i32 { return .?i32{ 5 }; }\n";

} // namespace

/* ==================== nil 比较：非变量表达式 ==================== */

TEST(NilCompareExpr, Field) {
  clux_expect_ok(std::string(kNode) +
                 "func main(): i32 {\n"
                 "  var n: own Node = .Node{ .next = .?own *Node{ nil },"
                 " .v = 1 };\n"
                 "  if (n.next != nil) { return 1; }\n"
                 "  if (n.next == nil) { return 0; }\n"
                 "  return 2;\n"
                 "}\n",
                 "nil_field");
}

TEST(NilCompareExpr, Element) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var arr: [2]?i32 = .[2]?i32{ nil, .?i32{ 7 } };\n"
      "  if (arr[0] != nil) { return 1; }\n"
      "  if (arr[1] == nil) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "nil_elem");
}

TEST(NilCompareExpr, Deref) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *?i32 = new ?i32{ nil };\n"
      "  if (p.* != nil) { return 1; }\n"
      "  if (p.* == nil) { return 0; }\n"
      "  return 2;\n"
      "}\n",
      "nil_deref");
}

TEST(NilCompareExpr, CallResult) {
  clux_expect_ok(std::string(kPick) +
                 "func main(): i32 {\n"
                 "  if (pick() == nil) { return 1; }\n"
                 "  if (pick().! != 5) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "nil_call");
}

TEST(NilCompareExpr, UnwrapChain) {
  /* `n.next.!.next` 是解包后的字段（链式），也能判空。 */
  clux_expect_ok(std::string(kNode) +
                 "func main(): i32 {\n"
                 "  var n: own Node = .Node{\n"
                 "      .next = .?own *Node{ new Node{\n"
                 "          .next = .?own *Node{ nil }, .v = 2 } }, .v = 1 };\n"
                 "  if (n.next.!.next != nil) { return 1; }\n"
                 "  if (n.next.!.next == nil) { return 0; }\n"
                 "  return 2;\n"
                 "}\n",
                 "nil_unwrap_chain");
}

TEST(NilCompareExpr, NonOptionalExpressionRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var a: [2]i32 = .[2]i32{ 1, 2 };\n"
      "  if (a[0] == nil) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "is not optional; cannot compare with nil", "nil_nonopt");
}

/* ==================== 声明顺序：提前 return 的作用域 ==================== */

TEST(DestroyDeclOrder, EarlyReturnSkipsLaterOwnVar) {
  /* 修复前：return 1 的销毁序列里含 PUSH "p"（p 尚未 DEFINE）→
     运行期 "undefined variable"。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a: i32 = 1;\n"
      "  if (a == 1) { return 1; }\n"
      "  var p: own *i32 = new i32{ 7 };\n"
      "  if (p.* != 7) { return 6; }\n"
      "  return 0;\n"
      "}\n",
      "decl_early_return");
}

TEST(DestroyDeclOrder, LaterOwnVarDestroyedOnNormalPath) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a: i32 = 0;\n"
      "  if (a == 1) { return 1; }\n"
      "  var p: own *i32 = new i32{ 7 };\n"
      "  if (p.* != 7) { return 2; }\n"
      "  return 0;\n"          /* p 在正常路径上正常销毁（无泄漏） */
      "}\n",
      "decl_normal_path");
}

TEST(DestroyDeclOrder, LaterAggregateDestroyedOnNormalPath) {
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var a: i32 = 0;\n"
      "  if (a == 1) { return 1; }\n"
      "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  if (b.n != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "decl_aggregate");
}

TEST(DestroyDeclOrder, LaterVarNotDestroyedOnBreakPath) {
  /* break 路径同理：break 之前的变量才销毁，之后声明的不碰。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var total: i32 = 0;\n"
      "  for (var i: i32 = 0; i < 3; i = i + 1) {\n"
      "    var head: own *i32 = new i32{ i };\n"
      "    if (i == 1) { break; }\n"
      "    var tail: own *i32 = new i32{ 9 };\n"
      "    total = total + head.* + tail.*;\n"
      "  }\n"
      "  return 0;\n"
      "}\n",
      "decl_break");
}
