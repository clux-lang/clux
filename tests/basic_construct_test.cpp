/*
 * 基本类型也支持 `.<type>{<value>}` 构造（2026-10）。
 *
 *   var a = .i32{123};        // ≡ var a: i32 = 123;  （OWN_NONE，可拷贝）
 *   var f = .f64{1.5};
 *   var u = .u8{200};         // 显式写出类型 ⇒ 允许收窄（等价 C 复合字面量 (T){v}）
 *   var i = .i32{3.7};        // 3（截断）
 *   var s = .str{"hi"};
 *
 * 规则：恰好 1 个**位置值**（不写字段名）；值先按隐式转换、失败再按显式转换；
 * 结果类型就是写出的类型，所有权状态 NONE（与字面量同构，可自由拷贝/传参/返回）。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

std::string prog(const std::string &body) {
  return "func main(): i32 {\n" + body + "  return 0;\n}\n";
}

} // namespace

/* ==================== 基本类型构造 ==================== */

TEST(BasicConstruct, IntLiteral) {
  clux_expect_ok(prog("  var a = .i32{123};\n"
                      "  if (a != 123) { return 1; }\n"),
                 "bct_int");
}

TEST(BasicConstruct, AllScalarKinds) {
  clux_expect_ok(prog("  var b = .bool{true};\n"
                      "  var f = .f64{1.5};\n"
                      "  var g = .f32{2.5};\n"
                      "  var u = .u8{200};\n"
                      "  var w = .i64{7};\n"
                      "  var q = .usize{9};\n"
                      "  if (!b) { return 1; }\n"
                      "  if (f != 1.5) { return 2; }\n"
                      "  if (u != 200 as u8) { return 3; }\n"
                      "  if (w != 7) { return 4; }\n"
                      "  if (q != 9 as usize) { return 5; }\n"),
                 "bct_scalars");
}

TEST(BasicConstruct, ExplicitNarrowing) {
  /* 显式写出类型 ⇒ 允许收窄/截断（同 C 的 (T){v}） */
  clux_expect_ok(prog("  var x = .i32{3.7};\n"
                      "  if (x != 3) { return 1; }\n"
                      "  var u = .u8{300};\n"
                      "  if (u != 44 as u8) { return 2; }\n"),
                 "bct_narrow");
}

TEST(BasicConstruct, StrValue) {
  clux_expect_ok(prog("  var s = .str{\"hi\"};\n"
                      "  if (len(s) != 2 as u64) { return 1; }\n"),
                 "bct_str");
}

TEST(BasicConstruct, OwnNoneAndCopyable) {
  /* 结果是普通值（OWN_NONE）：可自由拷贝、可作为 own 指针的构造值。 */
  clux_expect_ok(prog("  var a = .i32{5};\n"
                      "  var b = a;\n"
                      "  if (b != 5) { return 1; }\n"
                      "  var p: own *i32 = new i32{ .i32{9} };\n"
                      "  if (p.* != 9) { return 2; }\n"),
                 "bct_copy");
}

TEST(BasicConstruct, InsideAggregate) {
  clux_expect_ok("struct S { a: i32; b: f64 }\n"
                 "func main(): i32 {\n"
                 "  var s = .S{ .a = .i32{3}, .b = .f64{2.5} };\n"
                 "  if (s.a != 3) { return 1; }\n"
                 "  if (s.b != 2.5) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "bct_aggregate");
}

TEST(BasicConstruct, ComptimeAndReturn) {
  clux_expect_ok(
      "comptime func five(): i32 { return .i32{5}; }\n"
      "func main(): i32 {\n"
      "  if (five() != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "bct_comptime");
}

/* ==================== 语法约束 ==================== */

TEST(BasicConstruct, RejectsNamedField) {
  clux_expect_reject(prog("  var a = .i32{ .value = 1 };\n"),
                     "positional value", "bct_named");
}

TEST(BasicConstruct, RejectsMultipleValues) {
  clux_expect_reject(prog("  var a = .i32{ 1, 2 };\n"),
                     "exactly one value", "bct_multi");
}

TEST(BasicConstruct, RejectsEmpty) {
  clux_expect_reject(prog("  var a = .i32{};\n"),
                     "requires exactly one value", "bct_empty");
}

TEST(BasicConstruct, RejectsIncompatibleValue) {
  /* 标量之间都走显式转换（`.bool{1.5}` = true，与 C 的 (bool)1.5 一致）；
     真正不兼容的是 str ↔ 数值。 */
  clux_expect_reject(prog("  var a = .i32{\"hi\"};\n"),
                     "cannot initialize", "bct_incompat");
}

TEST(BasicConstruct, ScalarCrossCastAllowed) {
  clux_expect_ok(prog("  var t = .bool{1.5};\n"
                      "  var z = .bool{0.0};\n"
                      "  var n = .i32{true};\n"
                      "  if (!t) { return 1; }\n"     /* (bool)1.5 == true */
                      "  if (z) { return 2; }\n"      /* (bool)0.0 == false */
                      "  if (n != 1) { return 3; }\n"),
                 "bct_cross_cast");
}
