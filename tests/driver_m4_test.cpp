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


} // namespace

/* ---- M4 切片与字符串（m4-design §1/§2/§4） ---- */

TEST(DriverM4, MakeAndIndexSlice) {
  /* make 构造 []T + 下标读（m4-design §2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{10, 20, 30};\n"
      "  var s: []i32 = make(a[0].&, 3);\n"
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
      "  var a: [3]i32 = .[3]i32{1, 2, 3};\n"
      "  var s: []i32 = make(a[0].&, 3);\n"
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
      "  var a: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var s: []i32 = make(a[0].&, 5);\n"
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
  /* arr[1:3] 切片表达式：[N]T → []T，借用指向 arr 内部 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{10, 20, 30, 40, 50};\n"
      "  var s: []i32 = arr[1:3];\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 20) { return 2; }\n"
      "  if (s[1] != 30) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceExpressionFullRange) {
  /* arr[:] 全范围切片：[N]T → []T，长度 = N */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{7, 8, 9};\n"
      "  var s: []i32 = arr[:];\n"
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
      "  var s: []i32 = arr[:2];\n"
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
      "  var s: []i32 = arr[3:];\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 4) { return 2; }\n"
      "  if (s[1] != 5) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceOfSlice) {
  /* 切片再切片：s[a:b] 对 []T 再次切片 → []T */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [6]i32 = .[6]i32{10, 20, 30, 40, 50, 60};\n"
      "  var s: []i32 = arr[1:5];\n"
      "  var t: []i32 = s[1:3];\n"
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
      "  var s: []i32 = arr[:];\n"
      "  s[0] = 99;\n"
      "  if (arr[0] != 99) { return 1; }\n"
      "  if (s[0] != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, SliceAsFunctionParam) {
  /* 切片作函数参数（[]T 借用传递） */
  std::string path = write_temp_file(
      "func sum(s: []i32): i32 {\n"
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
      "  var s: []i32 = arr[:];\n"
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
      "  var a: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var s: []i32 = make(a[0].&, 5);\n"
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
  /* 空切片 make(p, 0) → len=0，不下标访问 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [1]i32 = .[1]i32{1};\n"
      "  var s: []i32 = make(a[0].&, 0);\n"
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
      "  var s: []i32 = arr[2:2];\n"
      "  if (len(s) != 0u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M4 §8 str 切片 → [] const u8 ---- */

TEST(DriverM4, StrSliceFullRange) {
  /* str[:] → [] const u8：全范围字节视图（char 字面量是 u8，可与 const u8 比较） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var bytes: [] const u8 = s[:];\n"
      "  if (len(bytes) != 5u64) { return 1; }\n"
      "  if (bytes[0] != 'h') { return 2; }\n"
      "  if (bytes[4] != 'o') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceSubRange) {
  /* str[1:4] → [] const u8：子范围 "ell" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: [] const u8 = s[1:4];\n"
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
  /* str[:3] → [] const u8：前缀 "hel" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: [] const u8 = s[:3];\n"
      "  if (len(sub) != 3u64) { return 1; }\n"
      "  if (sub[0] != 'h') { return 2; }\n"
      "  if (sub[2] != 'l') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceSuffix) {
  /* str[2:] → [] const u8：后缀 "llo" */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: [] const u8 = s[2:];\n"
      "  if (len(sub) != 3u64) { return 1; }\n"
      "  if (sub[0] != 'l') { return 2; }\n"
      "  if (sub[2] != 'o') { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceEmpty) {
  /* str[3:3] → [] const u8：空范围 len=0 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  var sub: [] const u8 = s[3:3];\n"
      "  if (len(sub) != 0u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, StrSliceAsFunctionParam) {
  /* str 切片作函数参数（[] const u8 借用传递 + 遍历求和） */
  std::string path = write_temp_file(
      "func sum_bytes(s: [] const u8): u64 {\n"
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
      "  var bytes: [] const u8 = s[:];\n"
      "  var r: u64 = sum_bytes(bytes);\n"
      "  /* 'a'=97 + 'b'=98 + 'c'=99 = 294 */\n"
      "  if (r != 294u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M4 §9 所有权交互 + §7 切片比较 ---- */

TEST(DriverM4, SliceEqualitySameView) {
  /* §7 切片比较 ==：同一数组切同一范围 → ptr+len 相同 → 相等 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [5]i32 = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var a: []i32 = arr[1:4];\n"
      "  var b: []i32 = arr[1:4];\n"
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
      "  var a: []i32 = arr[0:3];\n"
      "  var b: []i32 = arr[0:2];\n"
      "  if (a == b) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, OwnSliceAsFunctionParam) {
  /* §0.5：`[]T` **可以**作函数参数——参数位用 接收所有权。
     调用点 `first(make(...))`：make 产 FATAL → 形参 OWN（函数负责释放）。 */
  std::string path = write_temp_file(
      "func first(s: []i32): i32 {\n"
      "  return s[0];\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{42, 99, 7};\n"
      "  var r: i32 = first(a[:]);\n"
      "  if (r != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §3.4 所有权流转：move / clone ---- */

TEST(DriverM4, ImplicitBorrowOwnToRef) {
  /* §3.4 隐式借用 []T → []T：赋值不转移所有权 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{5, 10, 15};\n"
      "  var s: []i32 = make(a[0].&, 3);\n"
      "  var r: []i32 = s;  /* 隐式借用 */\n"
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
      "  var s: []i32 = arr[0:5];  /* 5 > 3 越界 */\n"
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
      "  var s: []i32 = arr[3:1];  /* low > high */\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.1 作用域标注 ---- */

TEST(DriverM4, ScopeAnnotationSliceReturn) {
  /* §9.1 作用域标注：返回 []T 标注 '<a，借用来源是参数 a */
  std::string path = write_temp_file(
      "func sub(a: []i32): '<a> []i32 {\n"
      "  return a[0:2];\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr: [4]i32 = .[4]i32{10, 20, 30, 40};\n"
      "  var s: []i32 = sub(arr[:]);\n"
      "  if (len(s) != 2u64) { return 1; }\n"
      "  if (s[0] != 10) { return 2; }\n"
      "  if (s[1] != 20) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.2 禁止 []T 作函数参数 ---- */

/* ---- §9.3 闭包捕获切片 ---- */

TEST(DriverM4, ClosureCaptureRefSlice) {
  /* §9.3 闭包捕获 []T：须标注 '<s>，闭包内可读 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr: [3]i32 = .[3]i32{7, 8, 9};\n"
      "  var s: []i32 = arr[:];\n"
      "  var f = func '<s> |s| sum(arg: []i32): i32 {\n"
      "    return arg[0] + arg[1] + arg[2];\n"
      "  };\n"
      "  var r: i32 = f(s);\n"
      "  if (r != 24) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §9.4 禁止全局 []T ---- */

/* ---- §10.3 const 修饰 ---- */

TEST(DriverM4, ConstSliceElementReadOnly) {
  /* §10.3 [] const T：元素只读，写入 → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: str = \"hi\";\n"
      "  var bytes: [] const u8 = s[:];\n"
      "  bytes[0] = 'x';  /* const 元素不可写 */\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- §11.2 len 对 []T 也可用 ---- */

TEST(DriverM4, LenFatalSlice) {
  /* §11.2 len 对 []T 也可用（make 产 直接传 len） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: [4]i32 = .[4]i32{1, 2, 3, 4};\n"
      "  var n: u64 = len(make(a[0].&, 4));\n"
      "  if (n != 4u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM4, MakeElementsFoldComptimeVar) {
  /* 回归：make 的元素槽必须把 comptime 变量引用折叠为字面量并**写回 AST**。
     `sema_expr` 通过可写节点槽（`ast_node_t **`）做折叠写回，而这里曾传
     局部变量副本且不回写 ⇒ 折叠结果丢失 ⇒ comptime 变量引用漏到运行期
     （该名字在运行期不存在）→ `run: exec: undefined variable`。 */
  std::string path = write_temp_file(
      "comptime var N = 5;\n"
      "func main(): i32 {\n"
      "  var a: [2]i32 = .[2]i32{ N, N + 1 };\n"
      "  var s: []i32 = make(a[0].&, 2);\n"
      "  if (s[0] != 5) { return 1; }\n"
      "  if (s[1] != 6) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
