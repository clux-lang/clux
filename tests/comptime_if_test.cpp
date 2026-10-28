/*
 * `comptime if`：**编译期条件语句的语法树裁剪**（2026-10）
 *
 * 语义：
 *   - 条件必须可编译期求值（字面量 / 全局 comptime var / sizeof / 运算符组合），
 *     结果必须是 bool；
 *   - 存活分支的语句被**拼接**进外层块 —— 大括号是**剪枝范围，不是作用域**：
 *     `comptime if (f) { var a = 1; } else { var a = 2; } printf("%d", a);` 合法；
 *   - 丢弃分支**只经过解析期语法检查**（不注册符号、不做类型检查）——所以
 *     平台相关代码可以类型完全不同；
 *   - 分支内禁止 `comptime var` / `comptime func`（结构检查递归进嵌套块，
 *     但不进入函数体）；`comptime if` 不能出现在 `comptime func` 体内。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"
#include "parser/fmt.h"
#include "core/allocator.h"

namespace {

std::string prog(const std::string &body) {
  return "func main(): i32 {\n" + body + "  return 0;\n}\n";
}

} // namespace

/* ==================== 裁剪与"非作用域"语义 ==================== */

TEST(ComptimeIf, TrueBranchDefinesNameUsedAfter) {
  clux_expect_ok(prog("  comptime if (true) { var a = 1; } else { var a = 2; }\n"
                      "  if (a != 1) { return 1; }\n"),
                 "cif_true");
}

TEST(ComptimeIf, FalseBranchWins) {
  clux_expect_ok(prog("  comptime if (false) { var a = 1; } else { var a = 2; }\n"
                      "  if (a != 2) { return 1; }\n"),
                 "cif_false");
}

TEST(ComptimeIf, NoElseAndCondFalseRemovesStatement) {
  clux_expect_ok(prog("  var a: i32 = 0;\n"
                      "  comptime if (false) { a = 1; }\n"
                      "  if (a != 0) { return 1; }\n"),
                 "cif_no_else");
}

TEST(ComptimeIf, BranchesMayHaveDifferentTypes) {
  /* 丢弃分支里 `a` 是 str——完全不参与语义检查 */
  clux_expect_ok(prog("  comptime if (true) { var a = 1; } else { var a = \"str\"; }\n"
                      "  if (a != 1) { return 1; }\n"),
                 "cif_types");
}

TEST(ComptimeIf, DiscardedBranchSemanticErrorsIgnored) {
  clux_expect_ok(prog(
      "  comptime if (true) { var a = 1; } else {\n"
      "    var b: NoSuchType = undefined;\n"     /* 未定义类型 */
      "    undefined_function(1, 2, 3);\n"       /* 未定义函数 */
      "    break;\n"                             /* 循环外 break */
      "  }\n"
      "  if (a != 1) { return 1; }\n"),
      "cif_discard_errors");
}

TEST(ComptimeIf, GlobalComptimeVarCondition) {
  clux_expect_ok("comptime var N: const i32 = 2;\n"
                 "func main(): i32 {\n"
                 "  comptime if (N == 1) { var r = 10; } else if (N == 2) { var r = 20; } else { var r = 30; }\n"
                 "  if (r != 20) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cif_global_flag");
}

TEST(ComptimeIf, SizeofCondition) {
  /* sizeof 返回 u64：与整数字面量比较需要显式转换（与运行期 if 的类型规则一致）。 */
  clux_expect_ok(prog("  comptime if (sizeof(i64) == 8 as u64) { var a = 1; } else { var a = 2; }\n"
                      "  if (a != 1) { return 1; }\n"),
                 "cif_sizeof");
}

/* ==================== 控制流归属外层结构 ==================== */

TEST(ComptimeIf, ReturnInBranch) {
  clux_expect_ok("func f(): i32 {\n"
                 "  comptime if (true) { return 7; } else { return 8; }\n"
                 "}\n"
                 "func main(): i32 {\n"
                 "  if (f() != 7) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cif_return");
}

TEST(ComptimeIf, BreakBelongsToEnclosingLoop) {
  clux_expect_ok(prog("  var s: i32 = 0;\n"
                      "  for (var i: i32 = 0; i < 5; i = i + 1) {\n"
                      "    comptime if (false) { break; } else { s = s + 1; }\n"
                      "    if (i == 2) { break; }\n"
                      "  }\n"
                      "  if (s != 3) { return 1; }\n"),
                 "cif_break");
}

TEST(ComptimeIf, NestedFuncInsideBranchIsAllowed) {
  /* 嵌套函数体是独立作用域：其中的 comptime var 合法 */
  clux_expect_ok(prog("  comptime if (true) {\n"
                      "    func g(): i32 { comptime var Z: i32 = 2; return Z; }\n"
                      "    if (g() != 2) { return 1; }\n"
                      "  }\n"),
                 "cif_nested_func");
}

/* ==================== 限制 ==================== */

TEST(ComptimeIf, RejectsComptimeVarInBranch) {
  clux_expect_reject(prog("  comptime if (true) { comptime var X: i32 = 1; }\n"),
                     "cannot be defined inside a 'comptime if' branch",
                     "cif_reject_ctvar");
}

TEST(ComptimeIf, RejectsComptimeFuncInBranch) {
  clux_expect_reject(
      prog("  comptime if (true) { comptime func g(): i32 { return 1; } }\n"),
      "cannot be defined inside a 'comptime if' branch", "cif_reject_ctfunc");
}

TEST(ComptimeIf, RejectsComptimeVarInNestedBlockOfBranch) {
  clux_expect_reject(
      prog("  comptime if (true) { if (true) { comptime var X: i32 = 1; } }\n"),
      "cannot be defined inside a 'comptime if' branch", "cif_reject_nested");
}

TEST(ComptimeIf, RejectsInsideComptimeFunc) {
  clux_expect_reject(
      "comptime func f(): i32 {\n"
      "  comptime if (true) { return 1; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "not supported inside a 'comptime func'", "cif_reject_in_ctfunc");
}

TEST(ComptimeIf, RejectsNonConstantCondition) {
  clux_expect_reject(prog("  var x: i32 = 1;\n"
                          "  comptime if (x > 0) { return 1; }\n"),
                     "not a compile-time constant", "cif_reject_nonconst");
}

TEST(ComptimeIf, RejectsNonBoolCondition) {
  clux_expect_reject(prog("  comptime if (1) { return 1; }\n"),
                     "condition must be bool", "cif_reject_nonbool");
}

/* ==================== 格式化稳定性 ==================== */

namespace {
std::string fmt(const std::string &src) {
  allocator_t *a = create_allocator(malloc, free);
  char *out = fmt_format_source(a, src.c_str(), src.size(), nullptr);
  std::string result = out ? out : "";
  if (out) allocator_free(a, (void **)&out);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  return result;
}
} // namespace

TEST(ComptimeIf, FormatRoundTrip) {
  const std::string src =
      "func main(): i32 {\n"
      "  comptime if (true) { var a = 1; } else { var a = 2; }\n"
      "  return a - 1;\n"
      "}\n";
  const std::string out = fmt(src);
  EXPECT_NE(out.find("comptime if (true)"), std::string::npos) << out;
  EXPECT_NE(out.find("} else {"), std::string::npos) << out;
  /* 格式化后的程序仍然可运行（裁剪语义不变） */
  clux_expect_ok(out, "cif_fmt_roundtrip");
}
