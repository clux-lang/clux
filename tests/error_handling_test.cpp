/*
 * 错误处理语法糖：struct 左值上的 `.?' / `.!'。
 *
 *   b = a.?   -> if (a.ok()) b = a.value();
 *                else return Ret::of_error(a.error());
 *   b = a.!   -> if (a.ok()) b = a.value();
 *                else panic(a.panic());
 *
 * panic 是内建 noreturn 函数（stderr + abort），if 的终止分支不参与
 * 后续 TDZ 汇合。
 */

#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

static const char *kResult =
    "struct R {\n"
    "  okf: bool;\n"
    "  v: i32;\n"
    "  e: i32;\n"
    "  func ok(self: ref *const R): bool { return self.okf; }\n"
    "  func value(self: ref *const R): i32 { return self.v; }\n"
    "  func error(self: ref *const R): i32 { return self.e; }\n"
    "  func panic(self: ref *const R): str { return \"result error\"; }\n"
    "  func of_error(e: i32): R {\n"
    "    return .R{ .okf = false, .v = 0, .e = e };\n"
    "  }\n"
    "};\n";

TEST(ErrorHandling, TryAssignment) {
  clux_expect_ok(
      std::string(kResult) +
          "func test(r: R): R {\n"
          "  var b: i32 = undefined;\n"
          "  b = r.?;\n"
          "  return .R{ .okf = true, .v = b + 1, .e = 0 };\n"
          "}\n"
          "func main(): i32 {\n"
          "  var good: R = .R{ .okf = true, .v = 4, .e = 0 };\n"
          "  var bad: R = .R{ .okf = false, .v = 0, .e = 7 };\n"
          "  var a: R = test(good);\n"
          "  if (!a.ok() || a.value() != 5) { return 1; }\n"
          "  var c: R = test(bad);\n"
          "  if (c.ok() || c.error() != 7) { return 2; }\n"
          "  return 0;\n"
          "}\n",
      "error_try_assignment");
}

TEST(ErrorHandling, TryVarPredeclaresUndefined) {
  clux_expect_ok(
      std::string(kResult) +
          "func test(r: R): R {\n"
          "  var b = r.?;\n"
          "  return .R{ .okf = true, .v = b + 2, .e = 0 };\n"
          "}\n"
          "func main(): i32 {\n"
          "  var good: R = .R{ .okf = true, .v = 4, .e = 0 };\n"
          "  var a: R = test(good);\n"
          "  return a.value() - 6;\n"
          "}\n",
      "error_try_var");
}

TEST(ErrorHandling, PanicSugarSuccessBranch) {
  clux_expect_ok(
      std::string(kResult) +
          "func main(): i32 {\n"
          "  var good: R = .R{ .okf = true, .v = 4, .e = 0 };\n"
          "  var b = good.!;\n"
          "  return b - 4;\n"
          "}\n",
      "error_panic_ok");
}

TEST(ErrorHandling, PanicBranchDoesNotInhibitTdz) {
  clux_expect_ok(
      std::string(kResult) +
          "func pick(r: R): i32 {\n"
          "  var b: i32 = undefined;\n"
          "  if (r.ok()) { b = r.value(); } else { panic(\"bad\"); }\n"
          "  return b;\n"
          "}\n"
          "func main(): i32 {\n"
          "  var good: R = .R{ .okf = true, .v = 5, .e = 0 };\n"
          "  return pick(good) - 5;\n"
          "}\n",
      "error_panic_tdz");
}

TEST(ErrorHandling, OwnErrorValueIsRejected) {
  clux_expect_reject(
      "struct R {\n"
      "  okf: bool;\n"
      "  func ok(self: ref *const R): bool { return self.okf; }\n"
      "  func value(self: ref *const R): i32 { return 1; }\n"
      "  func error(self: ref *const R): fatal *i32 {\n"
      "    return new i32{ .i32{1} };\n"
      "  }\n"
      "  func of_error(e: fatal *i32): R {\n"
      "    return .R{ .okf = false };\n"
      "  }\n"
      "};\n"
      "func test(r: R): R { var b = r.?; return .R{ .okf = true }; }\n"
      "func main(): i32 { return 0; }\n",
      "error value from 'error()' must be copyable",
      "error_own_error");
}

TEST(ErrorHandling, NoreturnDecoratorUserFunction) {
  clux_expect_ok(
      "[[noreturn]] func fail(): void { panic(\"custom fail\"); }\n"
      "func pick(ok: bool): i32 {\n"
      "  var b: i32 = undefined;\n"
      "  if (ok) { b = 7; } else { fail(); }\n"
      "  return b;\n"
      "}\n"
      "func main(): i32 { return pick(true) - 7; }\n",
      "error_noreturn_user");
}

TEST(ErrorHandling, NoreturnDecoratorGenericFunction) {
  clux_expect_ok(
      "[[noreturn]] func fail[T](): void { panic(\"generic fail\"); }\n"
      "func pick(ok: bool): i32 {\n"
      "  var b: i32 = undefined;\n"
      "  if (ok) { b = 9; } else { fail[i32](); }\n"
      "  return b;\n"
      "}\n"
      "func main(): i32 { return pick(true) - 9; }\n",
      "error_noreturn_generic");
}

TEST(ErrorHandling, GenericResultStruct) {
  clux_expect_ok(
      "struct Result[T, E] {\n"
      "  good: bool;\n"
      "  val: ?T;\n"
      "  err: ?E;\n"
      "  func ok(self: ref *const Result[T, E]): bool { return self.good; }\n"
      "  func value(self: ref *const Result[T, E]): T { return self.val.!; }\n"
      "  func error(self: ref *const Result[T, E]): E { return self.err.!; }\n"
      "  func panic(self: ref *const Result[T, E]): str { return \"Result error\"; }\n"
      "  func of_value(v: T): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = true, .val = .?T{ v }, .err = nil };\n"
      "  }\n"
      "  func of_error(e: E): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = false, .val = nil, .err = .?E{ e } };\n"
      "  }\n"
      "}\n"
      "func half(x: i32): Result[i32, str] {\n"
      "  if (x < 0) { return Result[i32, str]::of_error(\"negative\"); }\n"
      "  return Result[i32, str]::of_value(x / 2);\n"
      "}\n"
      "func use(x: i32): Result[i32, str] {\n"
      "  var v = half(x).?;\n"
      "  return Result[i32, str]::of_value(v + 1);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: Result[i32, str] = use(8);\n"
      "  if (!a.ok() || a.value() != 5) { return 1; }\n"
      "  var b: Result[i32, str] = use(-1);\n"
      "  if (b.ok() || b.error() != \"negative\") { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "error_generic_result");
}

TEST(ErrorHandling, TryOperandEvaluatedOnce) {
  clux_expect_ok(
      std::string(kResult) +
          "var calls: i32 = 0;\n"
          "func mk(): R {\n"
          "  calls = calls + 1;\n"
          "  return .R{ .okf = true, .v = 6, .e = 0 };\n"
          "}\n"
          "func use(): R {\n"
          "  var v = mk().?;\n"
          "  return .R{ .okf = true, .v = v, .e = 0 };\n"
          "}\n"
          "func main(): i32 {\n"
          "  var a: R = use();\n"
          "  if (calls != 1) { return 1; }\n"
          "  return a.value() - 6;\n"
          "}\n",
      "error_try_once");
}

TEST(ErrorHandling, TakePlainOptional) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var x: ?i32 = .?i32{ 5 };\n"
      "  var y: i32 = take_opt(x);\n"
      "  if (x != nil) { return 1; }\n"
      "  return y - 5;\n"
      "}\n",
      "take_plain");
}

TEST(ErrorHandling, TakeOwnOptional) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var o: own ?own *i32 = .?own *i32{ new i32{ .i32{7} } };\n"
      "  var p: own *i32 = take_opt(o);\n"
      "  if (o != nil) { return 1; }\n"
      "  return p.* - 7;\n"
      "}\n",
      "take_own");
}

TEST(ErrorHandling, TakeOptionalField) {
  clux_expect_ok(
      "struct Node { next: ?own *i32; }\n"
      "func main(): i32 {\n"
      "  var n: own Node = .Node{ .next = .?own *i32{ new i32{ .i32{9} } } };\n"
      "  var p: own *i32 = take_opt(n.next);\n"
      "  if (n.next != nil) { return 1; }\n"
      "  return p.* - 9;\n"
      "}\n",
      "take_field");
}