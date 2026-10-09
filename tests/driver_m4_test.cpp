#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_m4_test_XXXXXX";
  auto path_str = path.string();
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- M4 切片与字符串（m4-design §1/§2/§4） ---- */

TEST(DriverM4, MakeAndIndexSlice) {
  /* make 构造 fatal []T + 下标读（m4-design §2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 10, 20, 30);\n"
      "  if (s[0] != 10) { return 1; }\n"
      "  if (s[1] != 20) { return 2; }\n"
      "  if (s[2] != 30) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, MakeAndIndexWriteSlice) {
  /* 下标写：s[i] = v 经 INDEX_SET 写回切片元素 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  s[0] = 100;\n"
      "  s[1] = 200;\n"
      "  s[2] = 300;\n"
      "  if (s[0] != 100) { return 1; }\n"
      "  if (s[1] != 200) { return 2; }\n"
      "  if (s[2] != 300) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, LenSlice) {
  /* len(s) 返回切片元素个数（u64） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 5, 1, 2, 3, 4, 5);\n"
      "  var n: u64 = len(s);\n"
      "  if (n != 5u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, LenArray) {
  /* len(arr) 返回数组编译期常量长度（u64） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [4]i32 = .[4]i32{10, 20, 30, 40};\n"
      "  var n: u64 = len(arr);\n"
      "  if (n != 4u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceExpressionFromArray) {
  /* arr[1:3] 切片表达式：[N]T → ref []T，借用指向 arr 内部 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{10, 20, 30, 40, 50};\n"
      "  var s: ref []i32 = arr[1:3];\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 20) { return 2; }\n"
      "  if (s[1] != 30) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceExpressionFullRange) {
  /* arr[:] 全范围切片：[N]T → ref []T，长度 = N */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{7, 8, 9};\n"
      "  var s: ref []i32 = arr[:];\n"
      "  if (len(s) != 3u64) { return 1; }\n"
      "  if (s[0] != 7) { return 2; }\n"
      "  if (s[1] != 8) { return 3; }\n"
      "  if (s[2] != 9) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceExpressionPrefix) {
  /* arr[:n] 前缀切片：取前 n 个元素 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var s: ref []i32 = arr[:2];\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 1) { return 2; }\n"
      "  if (s[1] != 2) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceExpressionSuffix) {
  /* arr[n:] 后缀切片：从 n 到末尾 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var s: ref []i32 = arr[3:];\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 4) { return 2; }\n"
      "  if (s[1] != 5) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceOfSlice) {
  /* 切片再切片：s[a:b] 对 ref []T 再次切片 → ref []T */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [6]i32 = .[6]i32{10, 20, 30, 40, 50, 60};\n"
      "  var s: ref []i32 = arr[1:5];\n"
      "  var t: ref []i32 = s[1:3];\n"
      "  if (len(t) != 2u64) { return 1; }\n"
      "  if (t[0] != 30) { return 2; }\n"
      "  if (t[1] != 40) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceViewModifiesOriginal) {
  /* 切片借用指向原数组内部：经切片写入可见于原数组 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{1, 2, 3};\n"
      "  var s: ref []i32 = arr[:];\n"
      "  s[0] = 99;\n"
      "  if (arr[0] != 99) { return 1; }\n"
      "  if (s[0] != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, MakeWithFillInit) {
  /* make + <v,N> fill 展开：make(i32, 4, <7, 4>) → [7,7,7,7] */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 4, <7, 4>);\n"
      "  if (s[0] != 7) { return 1; }\n"
      "  if (s[1] != 7) { return 2; }\n"
      "  if (s[2] != 7) { return 3; }\n"
      "  if (s[3] != 7) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, MakeMixedFillAndSingle) {
  /* make 混合 fill + 单值：make(i32, 5, <1, 2>, 3, <4, 2>) → [1,1,3,4,4] */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 5, <1, 2>, 3, <4, 2>);\n"
      "  if (s[0] != 1) { return 1; }\n"
      "  if (s[1] != 1) { return 2; }\n"
      "  if (s[2] != 3) { return 3; }\n"
      "  if (s[3] != 4) { return 4; }\n"
      "  if (s[4] != 4) { return 5; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceAsFunctionParam) {
  /* 切片作函数参数（ref []T 借用传递） */
  std::string path = write_temp_file(
      "func sum(s: ref []i32): i32 {\n"
      "  var total: i32 = 0;\n"
      "  var i: u64 = 0u64;\n"
      "  while (i < len(s)) {\n"
      "    total = total + s[i];\n"
      "    i = i + 1u64;\n"
      "  }\n"
      "  return total;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr: [4]i32 = .[4]i32{1, 2, 3, 4};\n"
      "  var s: ref []i32 = arr[:];\n"
      "  var r: i32 = sum(s);\n"
      "  if (r != 10) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, MakeSliceSumLoop) {
  /* make 构造 + while 遍历求和（端到端） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 5, 1, 2, 3, 4, 5);\n"
      "  var total: i32 = 0;\n"
      "  var i: u64 = 0u64;\n"
      "  while (i < len(s)) {\n"
      "    total = total + s[i];\n"
      "    i = i + 1u64;\n"
      "  }\n"
      "  if (total != 15) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceZeroLength) {
  /* 空切片 make(i32, 0) → len=0，不下标访问 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 0);\n"
      "  if (len(s) != 0u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceEmptyRange) {
  /* arr[2:2] 空范围切片：len=0 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [4]i32 = .[4]i32{1, 2, 3, 4};\n"
      "  var s: ref []i32 = arr[2:2];\n"
      "  if (len(s) != 0u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M4 §8 str 切片 → ref [] const u8 ---- */

TEST(DriverM4, StrSliceFullRange) {
  /* str[:] → ref [] const u8：全范围字节视图（char 字面量是 u8，可与 const u8 比较） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var bytes: ref [] const u8 = s[:];\n"
      "  if (len(bytes) != 5u64) { return 1; }\n"
      "  if (bytes[0] != 'h') { return 2; }\n"
      "  if (bytes[4] != 'o') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceSubRange) {
  /* str[1:4] → ref [] const u8：子范围 "ell" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: ref [] const u8 = s[1:4];\n"
      "  if (len(sub) != 3u64) { return 1; }\n"
      "  if (sub[0] != 'e') { return 2; }\n"
      "  if (sub[1] != 'l') { return 3; }\n"
      "  if (sub[2] != 'l') { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSlicePrefix) {
  /* str[:3] → ref [] const u8：前缀 "hel" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: ref [] const u8 = s[:3];\n"
      "  if (len(sub) != 3u64) { return 1; }\n"
      "  if (sub[0] != 'h') { return 2; }\n"
      "  if (sub[2] != 'l') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceSuffix) {
  /* str[2:] → ref [] const u8：后缀 "llo" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: ref [] const u8 = s[2:];\n"
      "  if (len(sub) != 3u64) { return 1; }\n"
      "  if (sub[0] != 'l') { return 2; }\n"
      "  if (sub[2] != 'o') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceEmpty) {
  /* str[3:3] → ref [] const u8：空范围 len=0 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: ref [] const u8 = s[3:3];\n"
      "  if (len(sub) != 0u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceAsFunctionParam) {
  /* str 切片作函数参数（ref [] const u8 借用传递 + 遍历求和） */
  std::string path = write_temp_file(
      "func sum_bytes(s: ref [] const u8): u64 {\n"
      "  var total: u64 = 0u64;\n"
      "  var i: u64 = 0u64;\n"
      "  while (i < len(s)) {\n"
      "    total = total + s[i];\n"
      "    i = i + 1u64;\n"
      "  }\n"
      "  return total;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: str = \"abc\";\n"
      "  var bytes: ref [] const u8 = s[:];\n"
      "  var r: u64 = sum_bytes(bytes);\n"
      "  /* 'a'=97 + 'b'=98 + 'c'=99 = 294 */\n"
      "  if (r != 294u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M4 §9 所有权交互 + §7 切片比较 ---- */

TEST(DriverM4, NestedSlice) {
  /* §9.5 嵌套切片 []own []T：make 内层切片 + 外层切片 + 二维下标 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var row0: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  var row1: own []i32 = make(i32, 3, 4, 5, 6);\n"
      "  var grid: own []own []i32 = make(own []i32, 2, row0, row1);\n"
      "  if (len(grid) != 2u64) { return 1; }\n"
      "  if (grid[0][0] != 1) { return 2; }\n"
      "  if (grid[0][2] != 3) { return 3; }\n"
      "  if (grid[1][0] != 4) { return 4; }\n"
      "  if (grid[1][2] != 6) { return 5; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceEqualitySameView) {
  /* §7 切片比较 ==：同一数组切同一范围 → ptr+len 相同 → 相等 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var a: ref []i32 = arr[1:4];\n"
      "  var b: ref []i32 = arr[1:4];\n"
      "  if (!(a == b)) { return 1; }\n"
      "  if (a != b) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceEqualityDifferentRange) {
  /* §7 切片比较 !=：同源不同范围 → len 不同 → 不等 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var a: ref []i32 = arr[0:3];\n"
      "  var b: ref []i32 = arr[0:2];\n"
      "  if (a == b) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceAsStructField) {
  /* §9.6 切片作 struct 字段：own []u8 字段 + u64 字段（char 字面量是 u8） */
  std::string path = write_temp_file(
      "struct Buffer {\n"
      "  data: own []u8;\n"
      "  cap:  u64;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var d: own []u8 = make(u8, 4, 'A', 'B', 'C', 'D');\n"
      "  var buf: Buffer = .Buffer { .data = d, .cap = 4u64 };\n"
      "  if (len(buf.data) != 4u64) { return 1; }\n"
      "  if (buf.data[0] != 'A') { return 2; }\n"
      "  if (buf.data[3] != 'D') { return 3; }\n"
      "  if (buf.cap != 4u64) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, FatalSliceAsFunctionParam) {
  /* §9.2 fatal []T 作函数参数：make 产 fatal []T 直接传递，
     函数内须 move 接管（fatal 参数必须被消费） */
  std::string path = write_temp_file(
      "func first(s: fatal []i32): i32 {\n"
      "  var owned: own []i32 = move(s);\n"
      "  return owned[0];\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: i32 = first(make(i32, 3, 42, 99, 7));\n"
      "  if (r != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §3.4 所有权流转：move / clone ---- */

TEST(DriverM4, MoveSlice) {
  /* §3.4 move(own []T) → fatal []T：转移所有权，源置空 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 10, 20, 30);\n"
      "  var m: own []i32 = move(s);\n"
      "  if (m[0] != 10) { return 1; }\n"
      "  if (m[2] != 30) { return 2; }\n"
      "  if (len(m) != 3u64) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, CloneSlice) {
  /* §3.4 clone(own []T) → fatal []T：深拷贝，各独立堆块互不影响 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  var c: own []i32 = clone(s);\n"
      "  c[0] = 99;\n"
      "  if (s[0] != 1) { return 1; }   /* 原切片不受影响 */\n"
      "  if (c[0] != 99) { return 2; }  /* 克隆独立修改 */\n"
      "  if (len(c) != 3u64) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, ImplicitBorrowOwnToRef) {
  /* §3.4 隐式借用 own []T → ref []T：赋值不转移所有权 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 5, 10, 15);\n"
      "  var r: ref []i32 = s;  /* 隐式借用 */\n"
      "  if (r[0] != 5) { return 1; }\n"
      "  if (len(r) != 3u64) { return 2; }\n"
      "  r[1] = 20;  /* 经借用写入原堆块 */\n"
      "  if (s[1] != 20) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §4.1 运行期越界检查 ---- */

TEST(DriverM4, SliceOutOfBoundsHigh) {
  /* §4.1 运行期越界：high > len → 运行期错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{1, 2, 3};\n"
      "  var s: ref []i32 = arr[0:5];  /* 5 > 3 越界 */\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceOutOfBoundsLowGTHigh) {
  /* §4.1 运行期越界：low > high → 运行期错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var s: ref []i32 = arr[3:1];  /* low > high */\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.1 作用域标注 ---- */

TEST(DriverM4, ScopeAnnotationSliceReturn) {
  /* §9.1 作用域标注：返回 ref []T 标注 '<a，借用来源是参数 a */
  std::string path = write_temp_file(
      "func sub(a: ref []i32): '<a> ref []i32 {\n"
      "  return a[0:2];\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr: [4]i32 = .[4]i32{10, 20, 30, 40};\n"
      "  var s: ref []i32 = sub(arr[:]);\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 10) { return 2; }\n"
      "  if (s[1] != 20) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.2 禁止 own []T 作函数参数 ---- */

TEST(DriverM4, OwnSliceParamRejected) {
  /* §9.2 own []T 不能作函数参数（须用 fatal/ref）→ 编译错误 */
  std::string path = write_temp_file(
      "func bad(s: own []i32): i32 {\n"
      "  return s[0];\n"
      "}\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, FatalSliceParamMustBeConsumed) {
  /* §9.2 fatal []T 参数未消费 → 编译错误 */
  std::string path = write_temp_file(
      "func bad(s: fatal []i32): i32 {\n"
      "  return 0;  /* fatal 未被 move/return 消费 */\n"
      "}\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.3 闭包捕获切片 ---- */

TEST(DriverM4, ClosureCaptureRefSlice) {
  /* §9.3 闭包捕获 ref []T：须标注 '<s>，闭包内可读 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{7, 8, 9};\n"
      "  var s: ref []i32 = arr[:];\n"
      "  var f = func '<s> |s| sum(arg: ref []i32): i32 {\n"
      "    return arg[0] + arg[1] + arg[2];\n"
      "  };\n"
      "  var r: i32 = f(s);\n"
      "  if (r != 24) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.4 禁止全局 own []T ---- */

TEST(DriverM4, GlobalOwnSliceRejected) {
  /* §9.4 禁止全局 own []T → 编译错误 */
  std::string path = write_temp_file(
      "var g: own []i32 = make(i32, 2, 1, 2);\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §10.3 const 修饰 ---- */

TEST(DriverM4, ConstSliceElementReadOnly) {
  /* §10.3 [] const T：元素只读，写入 → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hi\";\n"
      "  var bytes: ref [] const u8 = s[:];\n"
      "  bytes[0] = 'x';  /* const 元素不可写 */\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §11.2 len 对 fatal []T 也可用 ---- */

TEST(DriverM4, LenFatalSlice) {
  /* §11.2 len 对 fatal []T 也可用（make 产 fatal 直接传 len） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var n: u64 = len(make(i32, 4, 1, 2, 3, 4));\n"
      "  if (n != 4u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
