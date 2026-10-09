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
