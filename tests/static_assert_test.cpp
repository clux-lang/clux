/*
 * `static_assert`：**标准编译期断言语句**（2026-10）
 *
 * 语义：
 *   - 语法：`static_assert(<cond>, "<msg>");`，可出现在任何语句位置；
 *   - cond 在编译期求值（CTFE），必须是 bool：
 *       true  → 无操作，不生成运行期代码；
 *       false → 编译失败，诊断带上 msg（字符串字面量文本）；
 *   - cond 不是编译期常量 / 不是 bool → 编译失败；
 *   - 位于 `comptime if` 未选中分支里的 static_assert 不参与语义检查
 *     （随剪枝一起消失），因此可以做平台/泛型相关的条件断言。
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

std::string fmt(const std::string &src) {
  allocator_t *a = create_allocator(malloc, free);
  char *out = fmt_format_source(a, src.c_str(), src.size(), nullptr);
  std::string result = out ? out : "";
  if (out) allocator_free(a, (void **)&out);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
  return result;
}

} // namespace

/* ==================== 基本语义 ==================== */

TEST(StaticAssert, TrueConditionIsAccepted) {
  clux_expect_ok(prog("  static_assert(true, \"ok\");\n"
                      "  static_assert(1 == 1, \"one\");\n"
                      "  static_assert(2 > 1, \"two\");\n"),
                 "sa_true");
}

TEST(StaticAssert, FalseConditionIsRejected) {
  clux_expect_reject(prog("  static_assert(false, \"boom\");\n"), "boom",
                     "sa_false");
}

TEST(StaticAssert, NonConstantConditionIsRejected) {
  clux_expect_reject(
      prog("  var x: i32 = 1;\n"
           "  static_assert(x > 0, \"x\");\n"),
      "not a compile-time constant", "sa_nonconst");
}

TEST(StaticAssert, NonBoolConditionIsRejected) {
  clux_expect_reject(prog("  static_assert(1, \"int\");\n"), "must be bool",
                     "sa_nonbool");
}

TEST(StaticAssert, NonLiteralMessageUsesGenericDiagnostic) {
  /* msg 不是字符串字面量时不求值、不拼接文本，只给通用诊断。 */
  clux_expect_reject(prog("  static_assert(false, undefined_message);\n"),
                     "static_assert failed", "sa_nonliteral_msg");
}

TEST(StaticAssert, ComptimeVarSizeofAlignofConditions) {
  clux_expect_ok("comptime var N: const i32 = 5;\n"
                 "func main(): i32 {\n"
                 "  comptime var M = 3;\n"
                 "  static_assert(N == 5, \"N\");\n"
                 "  static_assert(M == 3, \"M\");\n"
                 "  static_assert(sizeof(i64) == 8 as u64, \"sizeof\");\n"
                 "  static_assert(alignof(i64) == 8 as u64, \"alignof\");\n"
                 "  return 0;\n"
                 "}\n",
                 "sa_ct_values");
}

/* ==================== comptime if 剪枝 ==================== */

TEST(StaticAssert, PrunedBranchIsAccepted) {
  clux_expect_ok(prog("  comptime if (false) {\n"
                      "    static_assert(false, \"never compiled\");\n"
                      "  }\n"),
                 "sa_pruned");
}

TEST(StaticAssert, PrunedBranchSkipsSemanticChecks) {
  /* 未选中分支不做语义检查：msg 可以是未定义名字。 */
  clux_expect_ok(prog("  comptime if (false) {\n"
                      "    static_assert(false, undefined_message_value);\n"
                      "  }\n"),
                 "sa_pruned_unchecked");
}

/* ==================== 泛型：禁止 T == 某类型 ==================== */

TEST(StaticAssert, GenericGuardPassesForOtherType) {
  clux_expect_ok("func f[T](x: T): i32 {\n"
                 "  comptime if (T == i32) {\n"
                 "    static_assert(false, \"f: T must not be i32\");\n"
                 "  }\n"
                 "  return 0;\n"
                 "}\n"
                 "func main(): i32 { return f[i64](0i64); }\n",
                 "sa_generic_ok");
}

TEST(StaticAssert, GenericGuardFailsForForbiddenType) {
  clux_expect_reject("func f[T](x: T): i32 {\n"
                     "  comptime if (T == i32) {\n"
                     "    static_assert(false, \"f: T must not be i32\");\n"
                     "  }\n"
                     "  return 0;\n"
                     "}\n"
                     "func main(): i32 { return f[i32](0); }\n",
                     "T must not be i32", "sa_generic_bad");
}

TEST(StaticAssert, DirectGenericConditionPasses) {
  clux_expect_ok("func f[T](x: T): i32 {\n"
                 "  static_assert(T != i32, \"f: T must not be i32\");\n"
                 "  return 0;\n"
                 "}\n"
                 "func main(): i32 { return f[i64](0i64); }\n",
                 "sa_generic_direct_ok");
}

TEST(StaticAssert, DirectGenericConditionFails) {
  clux_expect_reject("func f[T](x: T): i32 {\n"
                     "  static_assert(T != i32, \"f: T must not be i32\");\n"
                     "  return 0;\n"
                     "}\n"
                     "func main(): i32 { return f[i32](0); }\n",
                     "T must not be i32", "sa_generic_direct_bad");
}

/* ==================== comptime func：CTFE 执行时检查 ==================== */

TEST(StaticAssert, ComptimeFuncTrueConditionAccepted) {
  clux_expect_ok("comptime func f(x: i32): i32 {\n"
                 "  static_assert(x > 0, \"must be positive\");\n"
                 "  return x;\n"
                 "}\n"
                 "func main(): i32 { return f(1) - 1; }\n",
                 "sa_ct_ok");
}

TEST(StaticAssert, ComptimeFuncFalseConditionRejected) {
  clux_expect_reject("comptime func f(x: i32): i32 {\n"
                     "  static_assert(x > 0, \"must be positive\");\n"
                     "  return x;\n"
                     "}\n"
                     "func main(): i32 { return f(-1); }\n",
                     "must be positive", "sa_ct_bad");
}

/* ==================== 格式化 ==================== */

TEST(StaticAssert, FormatRoundTrip) {
  const std::string src = "func main(): i32 {\n"
                          "  static_assert(1 == 1, \"fine\");\n"
                          "  comptime if (false) {\n"
                          "    static_assert(1 == 2, \"nope\");\n"
                          "  }\n"
                          "  return 0;\n"
                          "}\n";
  const std::string out = fmt(src);
  EXPECT_NE(out.find("static_assert(1 == 1, \"fine\");"), std::string::npos)
      << out;
  EXPECT_EQ(fmt(out), out);
  /* 格式化产物仍可编译（false 断言在未选中分支）。 */
  clux_expect_ok(out, "sa_fmt_roundtrip");
}
