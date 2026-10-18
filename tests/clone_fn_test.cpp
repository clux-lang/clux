/*
 * 生成的 clone 函数（2026-10，与销毁函数同构）。
 *
 * `clone(x)` 不再在调用点扁平展开整个槽位树，而是生成一次 clone 函数：
 *   kind 0 `__clone_<n>(dv: ref *T): fatal T`        —— 值克隆
 *   kind 1 `__cloneptr_<n>(dv: ref *U): fatal *U`    —— 指针克隆（新块 + 克隆被指向值）
 *   kind 2 `__cloneslice_<n>(dv: ref []T): fatal []T`—— 切片克隆（循环逐元素 clone）
 * 函数体内部按槽位展开，嵌套复合/指针/切片槽位再调用对应函数 —— 自引用类型
 * （链表/树）由函数自身递归，天然终止（运行时递归是合理的，扁平化留给未来）。
 *
 * 覆盖：语义（深拷贝独立性 + 无泄漏）、字节码（生成了 clone 函数并经 CALL 调用）、
 * 递归类型（函数体内出现对自身的 PUSH）、切片 prelude（return 语句的 prelude）。
 */
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";

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
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    n++;
    pos += needle.size();
  }
  return n;
}

} // namespace

/* ==================== 语义：深拷贝独立 + 无泄漏 ==================== */

TEST(CloneFn, StructDeepCopyIndependent) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                 "  var c: own Box = clone(b);\n"
                 "  c.p.* = 9;\n"
                 "  if (b.p.* != 7) { return 1; }\n"   /* 源不受影响 */
                 "  if (c.n != 1) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cfn_struct");
}

TEST(CloneFn, PointerCloneAllocatesNewBlock) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var p: own *Box = new Box{ .p = new i32{7}, .n = 1 };\n"
                 "  var q: own *Box = clone(p);\n"
                 "  q.p.* = 9;\n"
                 "  if (p.p.* != 7) { return 1; }\n"
                 "  if (q.n != 1) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cfn_ptr");
}

TEST(CloneFn, SliceCloneIndependentBuffer) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  var c: own []i32 = clone(s);\n"
      "  c[0] = 99;\n"
      "  if (s[0] != 1) { return 1; }\n"
      "  if (c[0] != 99) { return 2; }\n"
      "  if (len(c) != 3u64) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cfn_slice");
}

TEST(CloneFn, SliceOfStructsDeepCopy) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var s: own []Box = make(Box, 2,\n"
                 "      .Box{ .p = new i32{1}, .n = 1 },\n"
                 "      .Box{ .p = new i32{2}, .n = 2 });\n"
                 "  var c: own []Box = clone(s);\n"
                 "  c[0].p.* = 9;\n"
                 "  if (s[0].p.* != 1) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cfn_slice_struct");
}

TEST(CloneFn, OptionAndNestedStruct) {
  clux_expect_ok(
      "struct Inner { p: own *i32; n: i32 }\n"
      "struct Outer { i: ?own *Inner; k: i32 }\n"
      "func main(): i32 {\n"
      "  var o: own Outer = .Outer{\n"
      "      .i = .?own *Inner{ new Inner{ .p = new i32{7}, .n = 1 } },"
      " .k = 2 };\n"
      "  var c: own Outer = clone(o);\n"
      "  c.i.!.p.* = 9;\n"
      "  if (o.i.!.p.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfn_option_nested");
}

TEST(CloneFn, RecursiveListClone) {
  clux_expect_ok(
      "struct Node { next: ?own *Node; v: i32 }\n"
      "func main(): i32 {\n"
      "  var n: own Node = .Node{\n"
      "      .next = .?own *Node{ new Node{\n"
      "          .next = .?own *Node{ nil }, .v = 2 } }, .v = 1 };\n"
      "  var c: own Node = clone(n);\n"
      "  c.next.!.v = 9;\n"
      "  if (n.next.!.v != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfn_recursive");
}

/* ==================== 字节码：函数生成 + CALL ==================== */

TEST(CloneFn, BytecodeEmitsCloneFunction) {
  std::string text;
  ASSERT_TRUE(disasm_of(std::string(kBox) +
                            "func main(): i32 {\n"
                            "  var b: own Box = .Box{ .p = new i32{7},"
                            " .n = 1 };\n"
                            "  var c: own Box = clone(b);\n"
                            "  return c.n;\n"
                            "}\n",
                        "cfn_bc", &text))
      << text;
  EXPECT_NE(text.find("SET_FUNC_NAME \"__clone_0\""), std::string::npos)
      << text;
  EXPECT_NE(text.find("DEFINE \"__clone_0\""), std::string::npos) << text;
  EXPECT_GE(count_substr(text, "PUSH \"__clone_0\""), (size_t)1) << text;
}

TEST(CloneFn, SliceCloneUsesSliceFunction) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "func main(): i32 {\n"
                  "  var s: own []i32 = make(i32, 2, 1, 2);\n"
                  "  var c: own []i32 = clone(s);\n"
                  "  return c[0];\n"
                  "}\n",
                  "cfn_bc2", &text))
      << text;
  EXPECT_NE(text.find("SET_FUNC_NAME \"__cloneslice_0\""), std::string::npos)
      << text;
  /* 切片 prelude 挂在 return 语句上：clone 函数体内必须有 make + 循环 */
  EXPECT_NE(text.find("MAKE 0"), std::string::npos) << text;
}

TEST(CloneFn, RecursiveCloneCallsItself) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "struct Node { next: ?own *Node; v: i32 }\n"
                  "func main(): i32 {\n"
                  "  var n: own Node = .Node{ .next = .?own *Node{ nil },"
                  " .v = 1 };\n"
                  "  var c: own Node = clone(n);\n"
                  "  return c.v;\n"
                  "}\n",
                  "cfn_bc3", &text))
      << text;
  /* 自引用：值克隆 ↔ 指针克隆互相调用（Node → *Node → Node）→ 至少一个
     clone 函数被 PUSH ≥ 2 次（入口 + 递归）。 */
  size_t max_calls = 0;
  for (const char *nm : {"__clone_0", "__clone_1", "__cloneptr_2"}) {
    const std::string needle = std::string("PUSH \"") + nm + "\"";
    size_t n = count_substr(text, needle);
    if (n > max_calls) max_calls = n;
  }
  EXPECT_GE(max_calls, (size_t)2) << text;
}
