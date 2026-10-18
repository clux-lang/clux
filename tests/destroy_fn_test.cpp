/*
 * 生成的销毁函数（方案 B，2026-10）。
 *
 * 含 own 槽位的**复合类型**不再在每个销毁点内联展开，而是生成一次
 * `__destroy_<n>(dv: ref *T)`（body 销毁 T 的 own 槽位），销毁点只发
 * `CALL __destroy_<n>(&value)`；`own *T` 槽位传指针值本身。自引用类型
 * （链表/树）由函数自身递归调用，天然终止（不再需要运行期 FREE_RECURSIVE）。
 *
 * 覆盖：
 *   - 语义：单层/嵌套/递归/数组/option/union 的销毁都无泄漏（clux_expect_ok）；
 *   - 字节码：确实生成了 `__destroy_*` 函数并被调用（而不是内联展开）；
 *   - 自引用：销毁函数体里出现对自身的 `PUSH "__destroy_0"`（递归调用）。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";

/* 编译 → 反汇编成文本（driver 的 .cxb/.cxs 通道）。 */
bool disasm_of(const std::string &src, const std::string &tag,
               std::string *out_text) {
  const std::string src_path = write_temp_file(src, tag);
  const std::string bin_path = clux_test_temp_dir() + "/" + tag + ".cxb";
  const std::string asm_path = clux_test_temp_dir() + "/" + tag + ".cxs";
  if (driver_build_bin(src_path.c_str(), bin_path.c_str()) != 0) return false;
  if (driver_bin_to_asm(bin_path.c_str(), asm_path.c_str()) != 0) return false;
  std::ifstream in(asm_path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out_text = ss.str();
  std::remove(bin_path.c_str());
  std::remove(asm_path.c_str());
  return true;
}

size_t count_substr(const std::string &hay, const std::string &needle) {
  size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) { n++; pos += needle.size(); }
  return n;
}

} // namespace

/* ==================== 语义：销毁正确、无泄漏 ==================== */

TEST(DestroyFn, SingleStruct) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                 "  if (b.p.* != 7) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "dfn_single");
}

TEST(DestroyFn, NestedStructs) {
  clux_expect_ok(
      "struct Inner { p: own *i32; n: i32 }\n"
      "struct Mid { i: own *Inner; m: i32 }\n"
      "struct Outer { b: own *Mid; o: i32 }\n"
      "func main(): i32 {\n"
      "  var x: own Outer = .Outer{\n"
      "      .b = new Mid{ .i = new Inner{ .p = new i32{7}, .n = 1 },"
      " .m = 2 }, .o = 3 };\n"
      "  if (x.b.i.p.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "dfn_nested");
}

TEST(DestroyFn, RecursiveList) {
  clux_expect_ok(
      "struct Node { next: ?own *Node; v: i32 }\n"
      "func main(): i32 {\n"
      "  var c: own *Node = new Node{\n"
      "      .next = .?own *Node{ new Node{\n"
      "          .next = .?own *Node{ new Node{ .next = .?own *Node{ nil },"
      " .v = 1 } }, .v = 2 } }, .v = 3 };\n"
      "  if (c.next.!.next.!.v != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "dfn_list");
}

TEST(DestroyFn, ArrayOfStructs) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var a: own [2]Box = .[2]Box{\n"
                 "      .Box{ .p = new i32{1}, .n = 1 },\n"
                 "      .Box{ .p = new i32{2}, .n = 2 } };\n"
                 "  if (a[0].p.* != 1) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "dfn_array");
}

TEST(DestroyFn, UnionWithOwnPayload) {
  clux_expect_ok(
      "union U { a: own *i32; b: i32 }\n"
      "func main(): i32 {\n"
      "  var u: own U = .U{ .a = new i32{7} };\n"
      "  if (u.a.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "dfn_union");
}

TEST(DestroyFn, SliceOfStructs) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var s: own []Box = make(Box, 2,\n"
                 "      .Box{ .p = new i32{1}, .n = 1 },\n"
                 "      .Box{ .p = new i32{2}, .n = 2 });\n"
                 "  if (s[0].p.* != 1) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "dfn_slice");
}

/* ==================== 字节码：函数生成 + 调用 ==================== */

TEST(DestroyFn, BytecodeHasDestroyFunctionAndCall) {
  std::string text;
  ASSERT_TRUE(disasm_of(std::string(kBox) +
                            "func main(): i32 {\n"
                            "  var b: own Box = .Box{ .p = new i32{7},"
                            " .n = 1 };\n"
                            "  return 0;\n"
                            "}\n",
                        "dfn_bc", &text))
      << text;
  /* 定义 + 调用 */
  EXPECT_NE(text.find("SET_FUNC_NAME \"__destroy_0\""), std::string::npos)
      << text;
  EXPECT_NE(text.find("DEFINE \"__destroy_0\""), std::string::npos) << text;
  EXPECT_GE(count_substr(text, "PUSH \"__destroy_0\""), (size_t)1) << text;
  /* 销毁点不再是内联 FREE 序列（顶层只有一个 FREE 是 i32 块或没有）：
     本程序的 own 槽位销毁全部在 __destroy_0 体内。 */
  EXPECT_EQ(count_substr(text, "CALL 1"), count_substr(text, "PUSH \"__destroy_0\""))
      << text;
}

TEST(DestroyFn, NestedTypesGetSeparateFunctions) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "struct Inner { p: own *i32; n: i32 }\n"
                  "struct Mid { i: own *Inner; m: i32 }\n"
                  "func main(): i32 {\n"
                  "  var m: own Mid = .Mid{ .i = new Inner{ .p = new i32{7},"
                  " .n = 1 }, .m = 2 };\n"
                  "  return 0;\n"
                  "}\n",
                  "dfn_bc2", &text))
      << text;
  EXPECT_NE(text.find("SET_FUNC_NAME \"__destroy_0\""), std::string::npos)
      << text;
  EXPECT_NE(text.find("SET_FUNC_NAME \"__destroy_1\""), std::string::npos)
      << text;
}

TEST(DestroyFn, RecursiveTypeCallsItself) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "struct Node { next: ?own *Node; v: i32 }\n"
                  "func main(): i32 {\n"
                  "  var n: own Node = .Node{ .next = .?own *Node{ nil },"
                  " .v = 1 };\n"
                  "  return 0;\n"
                  "}\n",
                  "dfn_bc3", &text))
      << text;
  /* 自引用：函数体内部有对自身的调用 → PUSH "__destroy_0" 至少出现 2 次
     （入口调用 + 递归调用）。 */
  EXPECT_GE(count_substr(text, "PUSH \"__destroy_0\""), (size_t)2) << text;
}
