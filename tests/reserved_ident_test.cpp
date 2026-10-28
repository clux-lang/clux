/*
 * 保留标识符规则（2026-10）：
 *   - 以 `__` 开头的标识符**保留**给编译器生成名（`__destroy_<n>` / `__clone_<n>`
 *     / `__ctx` / `__cl_<n>` / `__deep_clone` …）与未来的魔法方法 —— 用户代码
 *     不得声明或引用；
 *   - 裸 `_` 是丢弃标记（`_ = expr`），不能作为标识符；
 *   - 名字中间含 `__`（`a__b`）与单下划线开头（`_tmp`）合法。
 *
 * 覆盖所有声明位置（变量/参数/捕获/函数/类型/struct/union/cunion/enum/字段/
 * import 别名）与表达式位置的 `__` 引用。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

void expect_reject(const std::string &src, const char *needle,
                   const std::string &tag) {
  clux_expect_reject(src, needle, tag);
}

} // namespace

/* ==================== `__` 前缀：全部声明位置拒绝 ==================== */

TEST(ReservedIdent, VariableName) {
  expect_reject("func main(): i32 {\n  var __x: i32 = 1;\n  return 0;\n}\n",
                "identifiers starting with '__' are reserved", "ri_var");
}

TEST(ReservedIdent, ParameterName) {
  expect_reject("func f(__p: i32): i32 { return __p; }\n"
                "func main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_param");
}

TEST(ReservedIdent, FunctionName) {
  expect_reject("func __f(): i32 { return 0; }\n"
                "func main(): i32 { return __f(); }\n",
                "identifiers starting with '__' are reserved", "ri_func");
}

TEST(ReservedIdent, TypeName) {
  expect_reject("type __T = i32;\nfunc main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_type");
}

TEST(ReservedIdent, StructNameAndField) {
  expect_reject("struct __S { a: i32 }\nfunc main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_struct");
  expect_reject("struct S { __f: i32 }\nfunc main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_field");
}

TEST(ReservedIdent, UnionAndEnum) {
  expect_reject("union __U { a: i32 }\nfunc main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_union");
  expect_reject("enum __E: i32 { A = 1 }\nfunc main(): i32 { return 0; }\n",
                "identifiers starting with '__' are reserved", "ri_enum");
}

TEST(ReservedIdent, CaptureName) {
  expect_reject("func main(): i32 {\n"
                "  var a: i32 = 1;\n"
                "  var f = func |(__q = a)| get(): i32 { return __q; };\n"
                "  return f() - 1;\n"
                "}\n",
                "identifiers starting with '__' are reserved", "ri_capture");
}

TEST(ReservedIdent, ExpressionReference) {
  /* 引用（不只是声明）也被拒绝：`__deep_clone` 是编译器内部名，用户不可调用。 */
  expect_reject("func main(): i32 {\n"
                "  return __deep_clone(1);\n"
                "}\n",
                "identifiers starting with '__' are reserved", "ri_use");
}

/* ==================== 裸 `_` 作为标识符拒绝 ==================== */

TEST(ReservedIdent, BareUnderscoreVariable) {
  expect_reject("func main(): i32 {\n  var _: i32 = 1;\n  return 0;\n}\n",
                "'_' is the discard marker and cannot be used as an identifier",
                "ri_underscore");
}

TEST(ReservedIdent, BareUnderscoreParameterAndField) {
  expect_reject("func f(_: i32): i32 { return 0; }\n"
                "func main(): i32 { return 0; }\n",
                "'_' is the discard marker", "ri_underscore_param");
  expect_reject("struct S { _: i32 }\nfunc main(): i32 { return 0; }\n",
                "'_' is the discard marker", "ri_underscore_field");
}

/* ==================== 合法形态保持可用 ==================== */

TEST(ReservedIdent, SingleUnderscorePrefixAllowed) {
  clux_expect_ok("func main(): i32 {\n"
                 "  var _tmp: i32 = 7;\n"
                 "  var a__b: i32 = 1;\n"   /* 中间含 __ 合法 */
                 "  if (_tmp != 7) { return 1; }\n"
                 "  if (a__b != 1) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "ri_ok_prefix");
}
