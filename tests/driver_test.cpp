#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_test_XXXXXX";
  auto path_str = path.string();
  /* mkstemps is not available on Windows; use a simple unique name. */
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- Stage ①: load source ---- */

TEST(Driver, LoadSource) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main(): void {}");

  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, path.c_str(), &data, &len), 0);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(len, 20u);
  EXPECT_EQ(std::string(data, len), "func main(): void {}");

  allocator_free(alloc, (void **)&data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}

TEST(Driver, LoadSourceMissingFile) {
  allocator_t *alloc = create_allocator(malloc, free);
  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, "no/such/file.cx", &data, &len), -1);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
}

/* ---- Stage ②: lex into a pool ---- */

TEST(Driver, LexFileProducesTokens) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main(): void {\n  return 0;\n}\n");

  vec_t *pool = NULL;
  ASSERT_EQ(driver_lex_file(alloc, path.c_str(), &pool), 0);
  ASSERT_NE(pool, nullptr);
  ASSERT_GT(vec_len(pool), 0u);

  /* Collect kinds in order. */
  std::vector<token_kind_t> kinds;
  for (size_t i = 0; i < vec_len(pool); i++) {
    const token_t *t = (const token_t *)vec_get(pool, i);
    kinds.push_back(token_get_kind(t));
  }

  /* Expect the leading keyword "func" and an EOF terminator. */
  EXPECT_EQ(kinds.front(), TOKEN_TYPE_KEYWORD);
  EXPECT_EQ(kinds.back(), TOKEN_TYPE_EOF);

  vec_free(alloc, &pool);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}

/* ---- Stage ①+②+③: run entry point ---- */

TEST(Driver, RunFileValidReturnsZero) {
  /* 合法程序：词法 + 语法 + 语义全通过。main 无返回类型（void），
     函数体不含 return 值，sema 无诊断。 */
  std::string path = write_temp_file("func main(): void { var x = 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileNestedArrayConstructPasses) {
  /* 多维数组构造：内层 construct 的 value_make_array 不得残留 type value
     到操作数栈（回归：曾因此外层 construct 栈布局错位报 missing type slot）。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 {\n"
      "    .[2]i32 { 1, 2 },\n"
      "    .[2]i32 { 3, 4 }\n"
      "  };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultipleArrayConstructsPasses) {
  /* 同一作用域内多次数组构造（三个独立同类型数组）不应残留 type value */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = .[2]i32 { 3, 4 };\n"
      "  var c = .[2]i32 { 5, 6 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileArrayConstructAssignPasses) {
  /* 数组构造 + 数组变量间赋值（不涉及索引） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = a;\n"
      "  var c = .[2]i32 { 3, 4 };\n"
      "  var d = c;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexGetPasses) {
  /* 下标右值读取（INDEX_GET）：一维 + 多维 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  var x = a[0] + a[2];\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var y = m[1][0];\n"
      "  var i = 1;\n"
      "  var z = a[i];\n"
      "  return x + y + z;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexSetPasses) {
  /* 下标左值赋值（INDEX_SET）：直接赋值 + 复合赋值 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  a[1] = 99;\n"
      "  a[2] += 1;\n"
      "  a[0] *= 2;\n"
      "  var i = 1;\n"
      "  a[i] -= 2;\n"
      "  var s = a[0] + a[1] + a[2];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFillZeroPads) {
  /* fill 值包显式 0 填充（M2 construct 完全显式，无自动填充）：
     .[3]i32{ <0,2>, 10 } 总元素数 = 2 + 1 = 3 == 长度 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { <0,2>, 10 };\n"
      "  return a[0] + a[1] + a[2];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyOptionalFuncArrayFillsNil) {
  /* 显式 nil 元素（M2 construct 完全显式，无自动填充）：.[N]?func()->T{ nil, ... }，
     元素经 ?func() 变量取出后 == nil 判定（nil 判定只支持标识符） */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var fns = .[3]?func()->i32{ nil, nil, nil };\n"
      "  var e0:?func()->i32 = fns[0];\n"
      "  var e1:?func()->i32 = fns[1];\n"
      "  var e2:?func()->i32 = fns[2];\n"
      "  if (e0 == nil && e1 == nil && e2 == nil) {\n"
      "    printf(\"all nil\\n\");\n"
      "  } else {\n"
      "    printf(\"not nil\\n\");\n"
      "  }\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFilePartialFillOptionalFuncArray) {
  /* optional 函数数组：显式 f + 显式 nil（M2 无自动 0 填充）。nil 元素判空 +
     SOME 分支 .! 解包后调用返回 7 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var fns = .[2]?func()->i32{ f, nil };\n"
      "  var e1:?func()->i32 = fns[1];\n"
      "  if (e1 != nil) { return 1; }\n"
      "  var e0:?func()->i32 = fns[0];\n"
      "  if (e0 != nil) { var f0 = e0.!; return f0(); }\n"
      "  return 2;\n"
      "}\n"
      "func f():i32 { return 7; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileConstructCountExceedsRejected) {
  /* 超出声明长度的字段数 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2, 3 };\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultiIndexSubscriptRejected) {
  /* a[i,j] 多索引（泛型实参语法预留）落到数组下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[0, 1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexNonArrayRejected) {
  /* 对非数组类型下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var x = 42;\n"
      "  var y = x[0];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexStringIndexRejected) {
  /* 非整数下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[\"k\"];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimIndexSetPasses) {
  /* 多维下标左值赋值（借用引用写回直达原数组）：m[1][0] = 7 应生效 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  m[1][0] = 7;\n"
      "  m[0][1] += 10;\n"
      "  var i = 1;\n"
      "  var j = 1;\n"
      "  m[i][j] = 8;\n"
      "  var s = m[0][0] + m[0][1] + m[1][0] + m[1][1];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimBorrowIsIndependentOnBind) {
  /* 借用引用绑定变量时 materialize 深拷贝：var row = m[1] 是独立副本，
     修改 row 不影响 m（用户确认的借用生命周期语义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var row = m[1];\n"
      "  row[0] = 100;\n"
      "  var s = m[1][0] + m[1][1];\n"
      "  return s;\n"   /* m[1] 仍是 3,4 → 7 */
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMissingReturnsOne) {
  EXPECT_EQ(driver_run_file("no/such/file.cx"), 1);
}

TEST(Driver, RunFileLexErrorReturnsOne) {
  std::string path = write_temp_file("@ not a token\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- Stage ④: sema ---- */

TEST(Driver, RunFileSemaErrorReturnsOne) {
  /* 语义错误：实参类型不匹配（str → i32），sema 应快速失败返回 1 */
  std::string path =
      write_temp_file("func foo(a:i32): void { } func main(): void { foo(\"s\"); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileValidSemaPassesReturnsZero) {
  /* 合法程序：带返回类型（:i32）+ 函数调用 + 变量推断，sema 全通过 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }"
      "func main(): void { var x = add(1, 2); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileBareBlockReturnsZero) {
  /* 裸块语句（独立作用域）应完整通过流水线 */
  std::string path = write_temp_file(
      "func main():i32 { var x = 1; { var y = 2; x = x + y; } return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyBareBlockReturnsZero) {
  /* 空裸块语句也应完整通过流水线 */
  std::string path =
      write_temp_file("func main():i32 { var x = 1; { } return x; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalNilEndToEndPasses) {
  /* nil 端到端（M2 optional）：?func 变量 nil 初始化 + ==nil 判定 + 赋值 f1
     （T → ?T 隐式提升）+ SOME 分支 .! 解包后调用 + 再赋 nil */
  std::string path = write_temp_file(
      "func f1(a:i32):i32 { return a + 1; }"
      "func main():i32 {\n"
      "  var g:?func(i32)->i32 = nil;\n"
      "  if (g != nil) { return 1; }\n"
      "  g = f1;\n"
      "  if (g == nil) { return 2; }\n"
      "  if (g != nil) { var f = g.!; var r = f(10); if (r != 11) { return 3; } }\n"
      "  g = nil;\n"
      "  if (g != nil) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileNilAsTypeAnnotationRejected) {
  /* var a:nil 应被拒绝：nil 是值字面量，不是类型名（M2 下 nil 仍非类型） */
  std::string path =
      write_temp_file("func main():i32 { var a:nil = nil; return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalStrEndToEndPasses) {
  /* ?str 端到端：nil 初始化 + 双向 ==nil 判定 + 赋值"world"（str → ?str 隐式
     提升）+ SOME 分支 .! 解包读取 + 再赋 nil */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var s:?str = nil;\n"
      "  if (s != nil) { return 1; }\n"
      "  if (nil != s) { return 2; }\n"
      "  s = \"world\";\n"
      "  if (s == nil) { return 5; }\n"
      "  if (s != nil) { var t:str = s.!; if (t != \"world\") { return 6; } }\n"
      "  s = nil;\n"
      "  if (s != nil) { return 7; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertEndToEndPasses) {
  /* .! assert 解包端到端：SOME 分支解包读取 inner 值参与运算返回 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var s:?str = nil;\n"
      "  s = \"world\";\n"
      "  if (s != nil) {\n"
      "    var t:str = s.!;\n"
      "    if (t != \"world\") { return 1; }\n"
      "  } else { return 2; }\n"
      "  var n:?i32 = 40;\n"
      "  if (n != nil) { var v:i32 = n.!; return v + 2; }\n"
      "  return 3;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertOnNonePanics) {
  /* .! 对 none 解包 → 运行期 panic（error 值，driver 返回非 0） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:?i32 = nil;\n"
      "  var v:i32 = x.!;\n"
      "  return v;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertOnNonOptionalRejected) {
  /* .! 作用于非 optional 值 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 5;\n"
      "  var v:i32 = x.!;\n"
      "  return v;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapTryRejected) {
  /* .? try 语义未实现 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:?i32 = 5;\n"
      "  var v = x.?;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalStrArrayNilElementPasses) {
  /* optional str 数组：显式 str 元素 + 显式 nil 元素（M2 无自动 0 填充）。
     e0 非 nil（SOME），e1/e2 为 nil */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sarr: [3]?str = .[3]?str{ \"a\", nil, nil };\n"
      "  var e0:?str = sarr[0];\n"
      "  var e1:?str = sarr[1];\n"
      "  var e2:?str = sarr[2];\n"
      "  if (e0 == nil) { return 1; }\n"
      "  if (e1 != nil) { return 2; }\n"
      "  if (e2 != nil) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyStatementsReturnsZero) {
  /* 单独分号空语句：顶层（函数定义后）、语句间、声明后均应通过流水线 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; };\n"
      "func main():i32 {\n"
      "  ;\n"
      "  var x:i32 = 1; ;\n"
      "  return add(x, 2);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileParamShadowsCaptureReturnsZero) {
  /* 参数位于 closure_scope 的子 scope：捕获 x 与参数 x 同名时参数遮蔽
     捕获（函数体 return x 命中参数，返回 7 而非捕获的 100） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 100;\n"
      "  var f = func |x| (x:i32):i32 { return x; };\n"
      "  return f(7);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- const/volatile 限定类型 ---- */

TEST(Driver, ConstTdzFirstAssignAllowed) {
  /* const 变量 TDZ 首次赋值 = 初始化，豁免合法：
     var a:const i32 = undefined; a = 123; */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = undefined; a = 123; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstReassignAfterInitRejected) {
  /* const 变量已初始化后再赋值 → 语义错误 */
  std::string path = write_temp_file(
      "func main(): void { var a:const i32 = undefined; a = 123; a = 456; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstInitThenAssignRejected) {
  /* const 变量带初始值定义（flow_init=true）后再赋值 → 语义错误 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstCompoundAssignRejected) {
  /* const 变量复合赋值（读+写）同样禁止 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a += 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstReadAndArithmeticAllowed) {
  /* const 变量只读 + 参与运算合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 5; var b:i32 = a * 2; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, VolatileVarNormalOps) {
  /* volatile 变量读写/运算/赋值全合法（代理子类型） */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a = a + 1; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileCombined) {
  /* const volatile i32（固定组合顺序 volatile(const(i32))）：
     首次赋值豁免，之后禁止 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const volatile i32 = undefined; a = 7; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileReassignRejected) {
  std::string path = write_temp_file(
      "func main(): void { var a:volatile const i32 = undefined; a = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, VolatileCompoundAssignAllowed) {
  /* volatile 不影响赋值（非 const），复合赋值合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a += 2; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstValueCopyToNonConst) {
  /* const 值可复制到非 const 变量（const T extends T 复制语义） */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 10; var b:i32 = a; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarKeepsDeclaredType) {
  /* 显式类型标注 + 默认宽度字面量初始化：DEFINE 须按声明类型 implicit_cast
     init（var x:i64 = 7 的 7 是 i32 字面量，须拓宽为 i64）。此前 DEFINE 直接
     clone init 原值，运行时 x 实为 i32 与 sema 符号表 i64 不一致，后续
     `x = 9 as i64`（同类型赋值）误报 not a widening conversion。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  x = 9 as i64;\n"
      "  x = x + 5;\n"
      "  printf(\"%d\\n\", x as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarInferenceMatchesDeclared) {
  /* var t = x（推断自显式标注的 i64 变量）→ t 运行时亦为 i64，
     与 sema 推断一致；赋值 i64 值合法 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  var t = x;\n"
      "  t = 9 as i64;\n"
      "  printf(\"%d\\n\", t as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarNarrowLiteralRejected) {
  /* 显式 i64 标注 + 不可 widening 的初始化（i64 → i8 收窄）：
     sema 应拦截，运行期不执行 */
  std::string path = write_temp_file(
      "func main(): void { var x:i8 = 300; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

namespace {

/** 读取整个文件到 std::string（-1 表示不存在）。 */
std::string slurp(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
  fclose(fp);
  return out;
}

} // namespace

/* 源码 → .cxb → .cxs → .cxb：两次 .cxb 应字节级一致（往返稳定），
 * 且中间 .cxs 可被执行。 */
TEST(Driver, AsmBinRoundTripStable) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_test";
  std::filesystem::create_directories(dir);
  std::string src  = (dir / "prog.cx").string();
  std::string cxb1 = (dir / "prog.cxb").string();
  std::string cxs  = (dir / "prog.cxs").string();
  std::string cxb2 = (dir / "prog2.cxb").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():void { printf(\"ok\\n\"); }\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  /* 源码 → .cxb 与 .cxs（一次编译） */
  EXPECT_EQ(driver_build_bin(src.c_str(), cxb1.c_str()), 0);
  EXPECT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);

  /* .cxb → .cxs 与源码编译出的 .cxs 应一致（同一反汇编器） */
  std::string cxs2 = (dir / "prog_from_bin.cxs").string();
  EXPECT_EQ(driver_bin_to_asm(cxb1.c_str(), cxs2.c_str()), 0);
  EXPECT_EQ(slurp(cxs), slurp(cxs2));

  /* .cxs → .cxb，与直接编译的 .cxb 字节级一致 */
  EXPECT_EQ(driver_asm_to_bin(cxs.c_str(), cxb2.c_str()), 0);
  EXPECT_EQ(slurp(cxb1), slurp(cxb2));

  /* 中间产物可执行 */
  EXPECT_EQ(driver_run_asm(cxs.c_str()), 0);
  EXPECT_EQ(driver_run_bin(cxb2.c_str()), 0);

  std::filesystem::remove_all(dir);
}

/* 内容嗅探：按内容（而非扩展名）判定输入类型。 */
TEST(Driver, DetectInputByContent) {
  auto dir = std::filesystem::temp_directory_path() / "clux_detect_test";
  std::filesystem::create_directories(dir);

  /* 源码（关键字特征）——扩展名故意用 .txt，验证不依赖扩展名 */
  std::string src = (dir / "a.txt").string();
  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():void { var x:i32 = 1; }\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(src.c_str()), DRIVER_INPUT_SOURCE);

  /* 汇编文本（助记符/标签特征）——扩展名 .txt */
  std::string cxs = (dir / "b.txt").string();
  {
    FILE *fp = fopen(cxs.c_str(), "wb");
    const char *code = "; comment\nstart:\n    push_i32 1\n    halt\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(cxs.c_str()), DRIVER_INPUT_CXS);

  /* 汇编文本：以助记符行开头（无标签、无注释） */
  std::string cxs2 = (dir / "c.txt").string();
  {
    FILE *fp = fopen(cxs2.c_str(), "wb");
    const char *code = "push_i32 1\nhalt\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(cxs2.c_str()), DRIVER_INPUT_CXS);

  /* 源码首行是 C 风格注释，应跳过注释后按关键字判定 */
  std::string src2 = (dir / "d.txt").string();
  {
    FILE *fp = fopen(src2.c_str(), "wb");
    const char *code = "// clux source\nfunc main():void {}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(src2.c_str()), DRIVER_INPUT_SOURCE);

  /* 二进制 .cxb（magic）——扩展名故意用 .dat */
  std::string cxsrc = (dir / "prog.cx").string();
  std::string cxb = (dir / "e.dat").string();
  {
    FILE *fp = fopen(cxsrc.c_str(), "wb");
    const char *code = "func main():void {}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  ASSERT_EQ(driver_build_bin(cxsrc.c_str(), cxb.c_str()), 0);
  EXPECT_EQ(driver_detect_input(cxb.c_str()), DRIVER_INPUT_CXB);

  /* 空文件 / 纯空白 → UNKNOWN */
  std::string empty = (dir / "f.txt").string();
  {
    FILE *fp = fopen(empty.c_str(), "wb");
    const char *code = "   \n\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(empty.c_str()), DRIVER_INPUT_UNKNOWN);

  /* 文件不存在 → UNKNOWN */
  EXPECT_EQ(driver_detect_input((dir / "nope.bin").string().c_str()),
            DRIVER_INPUT_UNKNOWN);

  /* 名称映射稳定 */
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_SOURCE), "source");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_CXS), "cxs");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_CXB), "cxb");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_UNKNOWN), "unknown");

  std::filesystem::remove_all(dir);
}

/* 内容嗅探驱动的互转：即使扩展名不标准也能正确转换。 */
TEST(Driver, ConvByContentSniffing) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_sniff";
  std::filesystem::create_directories(dir);

  /* 用 .txt 扩展名承载汇编文本 → 应能汇编为 .cxb 并执行 */
  std::string weird = (dir / "prog.txt").string();
  {
    FILE *fp = fopen(weird.c_str(), "wb");
    const char *code =
        "_start:\n"
        "    push_func_type\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    push \"void\"\n"
        "    func_type_return\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_function [main]\n"
        "    bind_func 64\n"
        "    set_func_name \"main\"\n"
        "    push_undefined\n"
        "    define \"main\"\n"
        "    halt\n"
        "main:\n"
        "    push_scope\n"
        "    push_undefined\n"
        "    ret\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(weird.c_str()), DRIVER_INPUT_CXS);

  std::string out_cxb = (dir / "out.bin").string();
  EXPECT_EQ(driver_asm_to_bin(weird.c_str(), out_cxb.c_str()), 0);
  EXPECT_EQ(driver_detect_input(out_cxb.c_str()), DRIVER_INPUT_CXB);

  /* 转回文本仍可执行（往返） */
  std::string out_cxs = (dir / "back.txt").string();
  EXPECT_EQ(driver_bin_to_asm(out_cxb.c_str(), out_cxs.c_str()), 0);
  EXPECT_EQ(driver_run_asm(out_cxs.c_str()), 0);
  EXPECT_EQ(driver_run_bin(out_cxb.c_str()), 0);

  std::filesystem::remove_all(dir);
}

/* 互转的失败路径：文件不存在 / 输入内容损坏。 */
TEST(Driver, ConvErrorPaths) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_err";
  std::filesystem::create_directories(dir);

  /* 源文件不存在 */
  EXPECT_NE(driver_asm_to_bin((dir / "nope.cxs").string().c_str(),
                              (dir / "out.cxb").string().c_str()), 0);
  EXPECT_NE(driver_bin_to_asm((dir / "nope.cxb").string().c_str(),
                              (dir / "out.cxs").string().c_str()), 0);

  /* .cxs 内容非法（未知助记符） */
  std::string bad_cxs = (dir / "bad.cxs").string();
  {
    FILE *fp = fopen(bad_cxs.c_str(), "wb");
    const char *code = "FOOBAR 1\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_NE(driver_asm_to_bin(bad_cxs.c_str(),
                              (dir / "bad.cxb").string().c_str()), 0);

  /* .cxb 内容非法（坏 magic） */
  std::string bad_cxb = (dir / "bad.cxb").string();
  {
    FILE *fp = fopen(bad_cxb.c_str(), "wb");
    const char code[] = "NOTABINARYFILE";
    fwrite(code, 1, sizeof code - 1, fp);
    fclose(fp);
  }
  EXPECT_NE(driver_bin_to_asm(bad_cxb.c_str(),
                              (dir / "bad_out.cxs").string().c_str()), 0);

  std::filesystem::remove_all(dir);
}

/* ---- 类型提升区（hoist）---- */

/* 编译产物含 hoist 区：类型构造收敛到产物最前的提升区（先定义类型，
   再定义函数，函数体最后），槽位发 LOAD_TYPE <id> 引用。
   driver_build_asm 产出的 .cxs 文本应可见
   DEFINE_TYPE / SEAL / PUSH_ARRAY / DEFINE_BOUND / LOAD_TYPE 指令。 */
TEST(Driver, BuildAsmHoistSectionPresent) {
  auto dir = std::filesystem::temp_directory_path() / "clux_hoist";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code =
        "func main():i32 {\n"
        "  var a = .[3]i32 { 1, 2, 3 };\n"
        "  var s = a[0] + a[1] + a[2];\n"
        "  return s;\n"
        "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());

  /* hoist 区：两遍扫描——pass 1 数组 PUSH_ARRAY→DEFINE_TYPE 声明登记 +
     pass 2 数组 LOAD_TYPE→DEFINE_BOUND→SEAL 定义封闭 */
  EXPECT_NE(text.find("DEFINE_TYPE"), std::string::npos);
  EXPECT_NE(text.find("SEAL"), std::string::npos);
  EXPECT_NE(text.find("PUSH_ARRAY"), std::string::npos);
  EXPECT_NE(text.find("DEFINE_BOUND"), std::string::npos);
  /* 槽位引用：函数体 LOAD_TYPE <id> 查表构造数组 */
  EXPECT_NE(text.find("LOAD_TYPE"), std::string::npos);

  std::filesystem::remove_all(dir);
}

/* 顶层 type def 名字绑定插在 hoist 两遍之间（类型定义自动提升）：
   pass 1 声明（PUSH_ARRAY→DEFINE_TYPE <id> 登记开放对象）→ typedef 绑定
   （LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "name"）→ pass 2 定义
   （LOAD_TYPE <id>→DEFINE_BOUND→SEAL）。断言 .cxs 文本中 DEFINE "Pair"
   出现在 SEAL（pass 2 收尾）之前。 */
TEST(Driver, BuildAsmTypeDefBindingBetweenHoistPasses) {
  auto dir = std::filesystem::temp_directory_path() / "clux_typedef_hoist";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code =
        "type Pair = [2]i32;\n"
        "func main():i32 {\n"
        "  var p:Pair = .[2]i32{10, 20};\n"
        "  return p[0];\n"
        "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());

  /* typedef 绑定 DEFINE "Pair" 在 hoist 区内：位于 pass 1 的 DEFINE_TYPE 与
     pass 2 的 SEAL 之间（类型定义自动提升） */
  size_t pos_decl = text.find("DEFINE_TYPE");
  size_t pos_bind = text.find("DEFINE \"Pair\"");
  size_t pos_seal = text.find("SEAL");
  ASSERT_NE(pos_decl, std::string::npos);
  ASSERT_NE(pos_bind, std::string::npos);
  ASSERT_NE(pos_seal, std::string::npos);
  EXPECT_LT(pos_decl, pos_bind) << "typedef 绑定应晚于 pass 1 声明";
  EXPECT_LT(pos_bind, pos_seal) << "typedef 绑定应早于 pass 2 定义收尾 SEAL";

  /* 绑定序列：LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "Pair" */
  EXPECT_NE(text.find("PUSH_UNDEFINED"), std::string::npos);

  std::filesystem::remove_all(dir);
}

/* 限定符数组组合：数组类型表达式 [N]T 的 T 是任意类型表达式，元素类型带
   const/volatile 天然支持。构造时字面量经"加限定符身份转换"（i32 →
   volatile i32 / const i32，非拓宽非缩窄，身份拷贝），编译运行端到端。 */
TEST(Driver, RunFileQualifiedElementArray) {
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var vp:[2]volatile i32 = .[2]volatile i32{1, 2};\n"
      "  vp[0] = vp[0] + 5;\n"
      "  var cp:[2]const i32 = .[2]const i32{7, 8};\n"
      "  return vp[0] + vp[1] + cp[0] + cp[1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* typedef 类型提升端到端：别名链 + 数组 + 限定符组合 + 函数签名引用全局
   type def，验证提升区把类型构造收敛到产物最前、typedef 绑定在两遍之间。 */
TEST(Driver, RunFileTypeDefAutoHoist) {
  std::string path = write_temp_file(
      "type Elem = i64;\n"
      "type Pair = [2]Elem;\n"
      "type PairAlias = Pair;\n"
      "type VolatilePair = [2]volatile i32;\n"
      "func sum_pair(p:Pair):i64 { return p[0] + p[1]; }\n"
      "func main():i32 {\n"
      "  var pa:PairAlias = .[2]i64{10, 20};\n"
      "  var s = sum_pair(pa);\n"
      "  var vp:VolatilePair = .[2]volatile i32{1, 2};\n"
      "  vp[0] = vp[0] + 5;\n"
      "  return (s as i32) + vp[0] + vp[1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* comptime 折叠写回端到端：comptime func 返回数组，调用点折叠为
   AST_CONSTRUCT（类型位 AST_TYPE_REF），编译运行全链路通过。
   hoist 区构造 [3]i32，函数体 LOAD_TYPE 引用，CONSTRUCT 建值，下标求和。 */
TEST(Driver, RunFileComptimeFoldArraySum) {
  std::string path = write_temp_file(
      "comptime func make_arr(): [3]i32 {\n"
      "  var r = .[3]i32{0, 0, 0};\n"
      "  r[0] = 4;\n"
      "  r[1] = 5;\n"
      "  r[2] = 6;\n"
      "  return r;\n"
      "}\n"
      "func main():i32 {\n"
      "  var a = make_arr();\n"
      "  return a[0] + a[1] + a[2];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* const/volatile 类型槽位同样走 hoist 区两遍扫描（pass 1 PUSH_CONST /
   PUSH_VOLATILE → DEFINE_TYPE <id> 声明登记开放对象；pass 2 LOAD_TYPE 拉回
   → LOAD sub → SET_TYPE 设 sub → SEAL 封闭，依赖后序），编译运行端到端。 */
TEST(Driver, RunFileQualifierTypesViaHoist) {
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a:const i32 = 7;\n"
      "  var b:volatile i32 = 3;\n"
      "  return a + b;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- extends：编译期类型计算 ---- */

TEST(Driver, RunFileExtendsCompileTimeFold) {
  /* extends 是纯编译期运算：sema 阶段 ctfe 求值后折叠为 bool 常量，
     运行期零指令。同类型/数组同型/const 兼容 → true；不同类型/长度不同 → false。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = i32 extends i32;\n"
      "  var b = i32 extends i64;\n"
      "  var c = [2]i32 extends [2]i32;\n"
      "  var d = [2]i32 extends [3]i32;\n"
      "  var e = const i32 extends i32;\n"
      "  if (a && !b && c && !d && e) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileExtendsFoldToBoolConst) {
  /* 折叠验证：反汇编 .cxs 中 extends 表达式应全部为 PUSH_BOOL 常量，
     不残留任何运行时类型计算指令。 */
  auto dir = std::filesystem::temp_directory_path() / "clux_ext_fold";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():i32 {\n"
                       "  var a = i32 extends i32;\n"
                       "  var b = i32 extends i64;\n"
                       "  if (a && !b) { return 1; }\n"
                       "  return 0;\n"
                       "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("PUSH_BOOL"), std::string::npos);
  /* extends 无字节码助记符（纯编译期），折叠后也不得出现 LOAD_TYPE 拉取 */
  EXPECT_EQ(text.find("EXTENDS"), std::string::npos);

  std::filesystem::remove_all(dir);
}

TEST(Driver, RunFileExtendsNonTypeRejected) {
  /* 非类型操作数：sema 阶段 ctfe 求值失败（value_extends → "extends: type
     value required"），编译报错退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var a = 5 extends i32;\n"
                                     "  return a;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- extends + 三元选择类型：type RHS 编译期折叠 ---- */

TEST(Driver, RunFileTypeDefExtendsTernary) {
  /* 需求 2 核心：type RHS 用 extends + 三元选择类型。extends 二元遇到
     即折叠为 bool（ctfe 求值），三元按真实 bool 惰性选分支——选中的分支
     是类型值，整个 rhs 收敛为 LOAD_TYPE。真/假两分支与内建/别名/数组
     操作数全覆盖。 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = i64;\n"
      "type T = A extends i32 ? B : A;\n"   // true → i64
      "type U = A extends i64 ? B : A;\n"   // false → i32
      "type V = B extends i64 ? i32 : f64;\n" // true → i32
      "func main():i32 {\n"
      "  var t:T = 100;\n"
      "  var u:U = 5;\n"
      "  var v:V = 3;\n"
      "  if (t != 100 || u != 5 || v != 3) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryFalseBranch) {
  /* extends 条件为假选 else 分支：别名操作数 + f32/f64 分支类型区分。
     Alt=f64：Alt extends i32 为 false → 选 f32（f64 字面量赋 f32 会报错，
     用 1.5f32 验证类型确实选中 f32 分支）。 */
  std::string path = write_temp_file(
      "type Alt = f64;\n"
      "type P = Alt extends i32 ? i64 : f32;\n"   // false → f32
      "type Q = Alt extends f64 ? i64 : f32;\n"   // true → i64
      "func main():i32 {\n"
      "  var p:P = 1.5f32;\n"
      "  var q:Q = 2;\n"
      "  if (p != 1.5 || q != 2) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryArray) {
  /* 数组操作数：同型 extends true，长度不同 false → 三元选对应分支 */
  std::string path = write_temp_file(
      "type A = [2]i32;\n"
      "type T = A extends [2]i32 ? i64 : f32;\n"  // true → i64
      "type U = A extends [3]i32 ? i64 : f32;\n"  // false → f32
      "func main():i32 {\n"
      "  var t:T = 2;\n"
      "  var u:U = 1.5f32;\n"
      "  if (t != 2 || u != 1.5) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernarySignature) {
  /* 选择出的类型用于函数签名（参数/返回类型），别名透明 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type R = A extends i32 ? i64 : i32;\n"
      "func double_it(v:R):R { return v * 2; }\n"
      "func main():i32 {\n"
      "  var r = double_it(21);\n"
      "  if (r != 42) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryAliasChain) {
  /* 三元两分支引用其它 type def（别名链），选中的分支即目标类型 */
  std::string path = write_temp_file(
      "type Base = i32;\n"
      "type Alt = f64;\n"
      "type Pick1 = Base extends i32 ? Base : Alt;\n"  // true → i32
      "type Pick2 = Alt extends i32 ? Base : Alt;\n"   // false → f64
      "func main():i32 {\n"
      "  var p1:Pick1 = 3;\n"
      "  var p2:Pick2 = 1.5f64;\n"
      "  if (p1 != 3 || p2 != 1.5) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- extends + 三元混合运算（普通值上下文） ---- */

TEST(Driver, RunFileExtendsTernaryMixedValue) {
  /* 用户场景：var val = i32 extends i32 ? 1 : 0。extends 折叠为 bool
     字面量 → 三元 cond 编译期确定 → 运行期返回选中分支值。内建/别名/
     数组操作数 + 真/假分支全覆盖。 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = f64;\n"
      "func main():i32 {\n"
      "  var val  = i32 extends i32 ? 1 : 0;\n"
      "  var val2 = i32 extends i64 ? 1 : 0;\n"
      "  var val3 = [2]i32 extends [2]i32 ? 1 : 0;\n"
      "  var val4 = [2]i32 extends [3]i32 ? 1 : 0;\n"
      "  var a = A extends i32 ? 100 : 200;\n"
      "  var b = A extends f64 ? 100 : 200;\n"
      "  var c = B extends f64 ? 1 : 0;\n"
      "  if (val != 1 || val2 != 0 || val3 != 1 || val4 != 0 ||\n"
      "      a != 100 || b != 200 || c != 1) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileExtendsTernaryNestedAndIf) {
  /* extends 折叠后用于嵌套三元与 if 条件：cond 均编译期确定 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = f64;\n"
      "func main():i32 {\n"
      "  var d = (A extends i32) ? (B extends f64 ? 5 : 6) : 7;\n"
      "  var r:i32 = 0;\n"
      "  if (A extends i32) { r = 42; }\n"
      "  if (d != 5 || r != 42) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 三元条件表达式 cond ? a : b ---- */

TEST(Driver, RunFileTernaryRuntime) {
  /* 运行时三元：cond 为真取 then、为假取 else；两分支惰性（未选中分支
     不求值）；结果可用于运算/返回。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a:i32 = 3;\n"
      "  var b:i32 = 7;\n"
      "  var r:i32 = (a > b) ? a : b;\n"
      "  if ((a < b) ? true : false) { r = r + 1; }\n"
      "  return r;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryNestedAndMixed) {
  /* 嵌套三元右结合 + 与二元运算混合：a ? b : c ? d : e → a ? b : (c ? d : e)。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 0;\n"
      "  var y:i32 = 1;\n"
      "  var z:i32 = 2;\n"
      "  var w:i32 = 3;\n"
      "  var r:i32 = (x == 0) ? x : (y == 1) ? y : (z == 2) ? z : w;\n"
      "  if (r != 0) { return 9; }\n"
      "  var s:i32 = (x != 0) ? x : (y == 1) ? y : (z == 2) ? z : w;\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryLazyBranches) {
  /* 惰性求值验证：未选中分支不执行（若执行会因副作用/非法访问而失败）。
     三元右侧是数组越界读（运行期会报错）——cond 为真时 else 分支不得求值。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var arr:[2]i32 = .[2]i32{10, 20};\n"
      "  var i:i32 = 0;\n"
      "  var r:i32 = (i < 2) ? arr[i] : arr[99];\n"
      "  if (r != 10) { return 7; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryCompileTimeFold) {
  /* ctfe 折叠验证：comptime var 的初始化含三元 → 编译期求值折叠为常量。
     运行时仍按折叠后的常量执行。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  comptime var a = (1 < 2) ? 5 : 9;\n"
      "  comptime var b = (3 > 4) ? 11 : 13;\n"
      "  return a + b;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryBranchTypeMismatchRejected) {
  /* 两分支类型不一致：sema 诊断（三元结果类型须一致），编译失败退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:i32 = 1;\n"
                                     "  var r = (x > 0) ? 1 : true;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryCondNotBoolRejected) {
  /* 条件非 bool：sema 诊断（ternary condition operand must be bool），退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:i32 = 1;\n"
                                     "  var r = x ? 1 : 2;\n"
                                     "  return r;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefGlobal) {
  /* 全局 type def：内建 rhs（PUSH 内建名）→ var 显式类型标注与运算 */
  std::string path = write_temp_file(
      "type MyInt = i64;\n"
      "func main():i32 {\n"
      "  var x:MyInt = 42;\n"
      "  var y = x + 1;\n"
      "  return y as i32;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefCompositeAndAlias) {
  /* 复合类型 rhs 折叠 LOAD_TYPE + 别名链 + 函数签名引用 */
  std::string path = write_temp_file(
      "type Pair = [2]i32;\n"
      "type Alias = Pair;\n"
      "func first(p:Pair):i32 { return p[0]; }\n"
      "func main():i32 {\n"
      "  var p:Alias = .[2]i32{10, 20};\n"
      "  return first(p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocal) {
  /* 局部 type def：块作用域遮蔽 + var 显式类型 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  type Local = i32;\n"
      "  var x:Local = 7;\n"
      "  {\n"
      "    type Inner = i64;\n"
      "    var y:Inner = 9;\n"
      "    x = x + (y as i32);\n"
      "  }\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoisted) {
  /* 局部 type def 提升：使用在定义之前（前向引用）→ 运行期块入口先 DEFINE
     类型名，var 槽位解析成功，结果正确 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:Local = 7;\n"
      "  type Local = i32;\n"
      "  {\n"
      "    var y:Inner = 9;\n"
      "    type Inner = i64;\n"
      "    x = x + (y as i32);\n"
      "  }\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoistedLoop) {
  /* 局部 type def 提升进循环体：每次迭代 PUSH_SCOPE 后入口 DEFINE（幂等），
     var 使用定义之前的类型名 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sum:i32 = 0;\n"
      "  var i:i32 = 0;\n"
      "  while (i < 3) {\n"
      "    var v:LoopT = i + 1;\n"
      "    type LoopT = i32;\n"
      "    sum = sum + v;\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return sum;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 局部函数（local function）端到端                                      */
/* ================================================================ */

TEST(Driver, RunFileLocalFuncBasic) {
  /* 局部函数定义 + 调用：函数体内块作用域注册，运行期块入口提升 DEFINE，
     调用经 PUSH name 作用域查找（外层函数体内可见） */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func inc(x:i32): i32 { return x + 1; }\n"
      "  func dbl(x:i32): i32 { return x * 2; }\n"
      "  return inc(dbl(n));\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(10);\n"   /* inc(dbl(10)) = 21 */
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncHoistedForwardReference) {
  /* 提升语义：局部函数定义在后，外层函数体内调用先于定义点（块入口
     先 DEFINE 全部局部函数，前向引用安全） */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  var r = caller(n);\n" /* 调用先于定义 */
      "  func caller(x:i32): i32 { return x + 1; }\n" /* 定义在后 */
      "  return r;\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCallsGlobal) {
  /* 局部函数体内调用全局函数：函数体查找链 = 参数 + 全局，合法 */
  std::string path = write_temp_file(
      "func g():i32 { return 100; }\n"
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "func outer(n:i32):i32 {\n"
      "  func inc(x:i32): i32 { return x + g(); }\n"
      "  return add(inc(n), 1);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(5);\n"   /* inc(5)=105, add(105,1)=106 */
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncNestedBlock) {
  /* 嵌套块局部函数：块作用域内定义 + 调用 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var acc:i32 = 0;\n"
      "  {\n"
      "    func twice(x:i32): i32 { return x * 2; }\n"
      "    acc = twice(3);\n"
      "  }\n"
      "  _ = acc;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCallRejected) {
  /* 局部函数体内调用兄弟函数：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func caller(x:i32): i32 { return callee(x); }\n"
      "  func callee(x:i32): i32 { return x; }\n"
      "  return caller(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncRecursionRejected) {
  /* 局部函数递归：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func dec(x:i32): i32 {\n"
      "    if (x <= 0) { return 0; }\n"
      "    return dec(x - 1);\n"
      "  }\n"
      "  return dec(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(5);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSelfCaptureRecursion) {
  /* 显式捕获自身递归：func |fib| fib(...) 定义点 STORE 后绑定新实例 →
     fib(10) = 55 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func |fib| fib(n: i32): i32 {\n"
      "    if (n <= 1) { return n; }\n"
      "    return fib(n - 1) + fib(n - 2);\n"
      "  }\n"
      "  var r: i32 = fib(10);\n"
      "  if (r != 55) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSelfCaptureWithOuterVar) {
  /* 捕获外层变量 + 自身递归：[base, pow] 混合捕获 → pow(4) = 2^4 = 16 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 2;\n"
      "  func |base, pow| pow(n: i32): i32 {\n"
      "    if (n == 0) { return 1; }\n"
      "    return base * pow(n - 1);\n"
      "  }\n"
      "  var r: i32 = pow(4);\n"
      "  if (r != 16) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCapture) {
  /* 兄弟函数捕获：a 显式捕获 b（定义在后）→ a(5) = b(5) = 50 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func |b| a(n: i32): i32 {\n"
      "    return b(n);\n"
      "  }\n"
      "  func b(n: i32): i32 {\n"
      "    return n * 10;\n"
      "  }\n"
      "  var r: i32 = a(5);\n"
      "  if (r != 50) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncValueRefBeforeDef) {
  /* 定义点前值引用有捕获局部函数（var f = b，b 定义在后）：块入口实例化
     绑定，f/b 浅拷贝共享 func_t，捕获在 b 定义点 SET_CLOSURE 填齐 →
     f(2) = 3*2 = 6（值引用放行，不报 TDZ） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  var f = b;\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  var r: i32 = f(2);\n"
      "  if (r != 6) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCaptureBackwardWithCaptures) {
  /* 后向兄弟捕获 + 兄弟也有捕获：a 捕获 b（定义在后、b 捕获 base）→
     a(2) = b(2) = 3*2 = 6（兄弟捕获链运行时正确解析） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  func |b| a(n: i32): i32 {\n"
      "    return b(n);\n"
      "  }\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  var r: i32 = a(2);\n"
      "  if (r != 6) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCallBeforeDefRejected) {
  /* 定义点前调用有捕获局部函数：值引用放行后调用点 TDZ 拦截保持不变 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  var r: i32 = b(2);\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  return r;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncNoCaptureSelfRefRejected) {
  /* 无捕获自身引用仍编译期拦截（提示加入捕获列表） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func fib(n: i32): i32 {\n"
      "    if (n <= 1) { return n; }\n"
      "    return fib(n - 1) + fib(n - 2);\n"
      "  }\n"
      "  var r: i32 = fib(10);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCaptureOuterRejected) {
  /* 局部函数捕获外层局部变量：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  var k = 10;\n"
      "  func inc(x:i32): i32 { return x + k; }\n"
      "  return inc(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncShadowsGlobalRejected) {
  /* 局部函数遮蔽全局函数名：显式拒绝 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "func outer(n:i32):i32 {\n"
      "  func add(x:i32): i32 { return x; }\n"
      "  return add(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoistedExprRef) {
  /* 提升的 type 名作表达式引用（type value）：运行期块入口 DEFINE 先于
     使用点——var t = Local 引用定义在后的 type Local */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var t = Local;\n"
      "  type Local = i32;\n"
      "  var x:i32 = 7;\n"
      "  _ = t;\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefRhsNotTypeRejected) {
  /* rhs 非类型值：sema 诊断，退出 1 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  type Bad = 42;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefDuplicateRejected) {
  /* 重复定义：sema 诊断，退出 1 */
  std::string path = write_temp_file("type A = i32;\n"
                                     "type A = i64;\n"
                                     "func main():i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefUnknownTypeRejected) {
  /* var 显式类型引用不存在的类型：3b 兜底 "unknown type"，退出 1 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:Nope = 5;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefTypeValueExpr) {
  /* type value 是真实值：可作表达式（var t = T 推断为 type 类型） */
  std::string path = write_temp_file(
      "type T = i32;\n"
      "func main():i32 {\n"
      "  var t = T;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 函数签名类型端到端：type F = func(i32,i32)->i32（M2 函数类型）。
   typedef RHS 走 ctfe 求值构造 func 签名 → hoist 区两遍扫描
   PUSH_FUNC_TYPE/FUNC_TYPE_PARAM/FUNC_TYPE_RETURN/SEAL → DEFINE 绑定。 */
TEST(Driver, RunFileTypeDefFuncSignature) {
  std::string path = write_temp_file(
      "type add_fn_t = func(i32,i32)->i32;\n"
      "func main():i32 {\n"
      "  var f:add_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNoReturn) {
  /* 显式 void 返回：func(i32)->void */
  std::string path = write_temp_file(
      "type void_fn_t = func(i32)->void;\n"
      "func main():i32 {\n"
      "  var f:void_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNoArrowFails) {
  /* 无 '->' 返回类型 func(i32) → 不允许隐式 void，解析报错 */
  std::string path = write_temp_file(
      "type void_fn_t = func(i32);\n"
      "func main():i32 {\n"
      "  var f:void_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNested) {
  /* 嵌套复合签名 func([4]i32)->func(i32)->i32 */
  std::string path = write_temp_file(
      "type complex_t = func([4]i32)->func(i32)->i32;\n"
      "func main():i32 {\n"
      "  var f:complex_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureAsParamType) {
  /* 函数签名类型作为函数参数类型（函数作为值的前置） */
  std::string path = write_temp_file(
      "type add_fn_t = func(i32,i32)->i32;\n"
      "func apply(f:add_fn_t, x:i32, y:i32):i32 { return 0; }\n"
      "func main():i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 函数值（function as value）端到端                                    */
/* ================================================================ */

TEST(Driver, RunFileFuncValueAssignAndCall) {
  /* var f = add; f(3,4) 经变量调用函数值 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var f = add;\n"
      "  var val = f(3, 4);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueAsArgument) {
  /* 函数值传参：apply(add,10,20) */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func apply(f:func(i32,i32)->i32, x:i32, y:i32):i32 {\n"
      "  return f(x, y);\n"
      "}\n"
      "func main():void {\n"
      "  var r = apply(add, 10, 20);\n"
      "  printf(\"%d\\n\", r);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueAsReturn) {
  /* 函数值返回：get_add() 返回 add，再经变量调用 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func get_add():func(i32,i32)->i32 {\n"
      "  return add;\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_add();\n"
      "  printf(\"%d\\n\", f(5, 6));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueExplicitTypeAndReassign) {
  /* 显式 func 类型 + 函数值再赋值（f = g） */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func sub(a:i32, b:i32):i32 {\n"
      "  return a - b;\n"
      "}\n"
      "func main():void {\n"
      "  var f: func(i32,i32)->i32 = add;\n"
      "  var g = sub;\n"
      "  f = g;\n"
      "  printf(\"%d\\n\", f(20, 8));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeFold) {
  /* comptime func 返回函数值：编译期折叠，add_fn(1,2) 输出 3 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "comptime func get_add():func(i32,i32)->i32 {\n"
      "  return add;\n"
      "}\n"
      "func main():void {\n"
      "  var add_fn = get_add();\n"
      "  var val = add_fn(1,2);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeReturnsLiteral) {
  /* comptime func 返回匿名函数字面量：折叠产物 LOAD_FUNCTION <fid>，
     运行期调用字面量函数体（+1 得 42） */
  std::string path = write_temp_file(
      "comptime func get_f():func(i32)->i32 {\n"
      "  return func(x:i32):i32 { return x + 1; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_f();\n"
      "  printf(\"%d\\n\", f(41));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeReturnsLocalFunc) {
  /* comptime func 内定义局部函数并返回：局部函数进入运行时字节码
     （prescan 无条件递归 comptime body），折叠产物按 fid 加载调用 */
  std::string path = write_temp_file(
      "comptime func get_f():func(i32)->i32 {\n"
      "  func inc(x:i32):i32 { return x + 1; }\n"
      "  return inc;\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_f();\n"
      "  printf(\"%d\\n\", f(41));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueCallExpressionCallee) {
  /* 一般 callee 表达式（函数值调用）：comptime func 返回的函数值立即调用
     get_fn()()、嵌套返回再调用、函数字面量直接调用 */
  std::string path = write_temp_file(
      "comptime func get_fn():func()->i32 {\n"
      "  return func():i32 { return 123; };\n"
      "}\n"
      "comptime func get_add():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32): i32 { return a + b; };\n"
      "}\n"
      "func make(): func(i32)->i32 {\n"
      "  return func(x: i32): i32 { return x * 2; };\n"
      "}\n"
      "func main():void {\n"
      "  var a = get_fn()();\n"
      "  var b = get_add()(19, 23);\n"
      "  var c = make()(21);\n"
      "  var d = func(x:i32):i32 { return x + 1; }(41);\n"
      "  printf(\"%d %d %d %d\\n\", a, b, c, d);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueCallExpressionRejected) {
  /* 非函数值表达式作 callee：编译期拒绝（cannot call value of type i32） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x: i32 = 5;\n"
      "  var r = x(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeNestedCalls) {
  /* comptime func body 内调用另一个 comptime func（嵌套折叠）：walk 阶段
     只做 shadow 类型检查，真实调用点 CTFE 折叠 */
  std::string path = write_temp_file(
      "comptime func double_(a:i32):i32 {\n"
      "  return a * 2;\n"
      "}\n"
      "comptime func quad(a:i32):i32 {\n"
      "  return double_(double_(a));\n"
      "}\n"
      "comptime var Q = quad(3);\n"
      "func main():void {\n"
      "  var x = Q;\n"
      "  printf(\"%d\\n\", x);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeMultipleFactories) {
  /* comptime func 多次调用各自返回字面量：fid 按定义点唯一分配，折叠产物
     分别加载不同函数对象 */
  std::string path = write_temp_file(
      "comptime func make_plus():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32):i32 { return a + b; };\n"
      "}\n"
      "comptime func make_mul():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32):i32 { return a * b; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = make_plus();\n"
      "  var g = make_mul();\n"
      "  printf(\"%d %d\\n\", f(20, 22), g(6, 7));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueBuiltinPrintf) {
  /* 内建 printf 与普通函数同等：函数值赋值 + 调用 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var p = printf;\n"
      "  p(\"hello %s %d\\n\", \"world\", 42);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueVarShadowsNameRejected) {
  /* 遮蔽平等：var add=2 遮蔽函数名，调用报错（不可调用而非函数不存在） */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var add = 2;\n"
      "  var val = add(1,2);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueTypeMismatchRejected) {
  /* 签名不匹配：func(i32)->i32 槽位收 func(i32,i32)->i32 报错 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var f: func(i32)->i32 = add;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 函数字面量（function literal）端到端                                */
/* ================================================================ */

TEST(Driver, RunFileFuncLiteralBasic) {
  /* 匿名函数字面量：var f = func(...){...}; f(41) → 42 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func(x: i32): i32 { return x + 1; };\n"
      "  var val = f(41);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralNamed) {
  /* 具名字面量：函数有显示名，但不绑定作用域名字（外部不可按名调用） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func inc(x: i32): i32 { return x * 2; };\n"
      "  var val = f(21);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralMultiParam) {
  /* 多参数 + 显式 func 类型槽位 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f: func(i32,i32)->i32 = func(a: i32, b: i32): i32 { return a * b; };\n"
      "  var val = f(6, 7);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralAsArgument) {
  /* 字面量直接传给函数参数（高阶调用）：apply(func(...), 10, 20) */
  std::string path = write_temp_file(
      "func apply(f:func(i32,i32)->i32, x:i32, y:i32):i32 {\n"
      "  return f(x, y);\n"
      "}\n"
      "func main():void {\n"
      "  var r = apply(func(a: i32, b: i32): i32 { return a + b; }, 10, 20);\n"
      "  printf(\"%d\\n\", r);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralNested) {
  /* 嵌套字面量：内层字面量作为外层字面量求值产物，outer(1) → 102 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var outer = func(n: i32): i32 {\n"
      "    var inner = func(x: i32): i32 { return x + 100; };\n"
      "    return inner(n + 1);\n"
      "  };\n"
      "  printf(\"%d\\n\", outer(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralLocalFuncInBody) {
  /* 字面量 body 内可定义局部函数并调用：f(14) → 42 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func(n: i32): i32 {\n"
      "    func triple(x: i32): i32 { return x * 3; }\n"
      "    return triple(n);\n"
      "  };\n"
      "  printf(\"%d\\n\", f(14));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralCaptureRejected) {
  /* 未声明捕获即访问外层局部变量：仍编译期拒绝（需显式捕获列表） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var base = 10;\n"
      "  var f = func(x: i32): i32 { return x + base; };\n"
      "  var val = f(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLoopClosuresIndependentInstances) {
  /* 循环内函数定义每次求值生成独立实例：fns[i] 三个闭包各自捕获
     items[0]=0 / items[1]=1 / items[2]=2，输出 "0 1 2"。
     若共享同一函数对象则全部捕获最后一次迭代值 → "2 2 2"。 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var fns = .[3](func()->i32){ func():i32 { return -1; }, func():i32 { return -1; }, func():i32 { return -1; } };\n"
      "  var items = .[3]i32 {0,1,2};\n"
      "  for(var i = 0;i<3;i+=1) {\n"
      "    fns[i] = func|(val = items[i])|():i32 { return val; };\n"
      "  }\n"
      "  printf(\"%d %d %d\\n\", fns[0](), fns[1](), fns[2]());\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 闭包（closure）端到端：捕获列表 |a,(b:i32=expr)|                   */
/* ================================================================ */

TEST(Driver, RunFileClosureBasicLiteral) {
  /* 字面量纯 id 捕获：func |base| add(x) 捕获外层 base（clone），
     定义点绑定 → f(5) → 10 + 5 = 15 */
  std::string path = write_temp_file(
      "func make_adder():func(i32)->i32 {\n"
      "  var base = 10;\n"
      "  return func |base| add(x: i32): i32 { return base + x; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = make_adder();\n"
      "  printf(\"%d\\n\", f(5));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureCloneSemantics) {
  /* 捕获是 clone 值而非引用：闭包创建后修改外层变量不影响闭包内捕获值 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var base = 10;\n"
      "  var f = func |base| add(x: i32): i32 { return base + x; };\n"
      "  base = 1000;\n"
      "  printf(\"%d\\n\", f(5));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureParenCapture) {
  /* 括号捕获（VALUE DECL 临时构造）：(b: i32 = c + d) 定义点求值，
     a 捕获外层 5，b 临时构造 107 → f(1) = 5 + 107 + 1 = 113 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var a = 5;\n"
      "  var c = 100;\n"
      "  var d = 7;\n"
      "  var f = func |a, (b: i32 = c + d)| add(x: i32): i32 { return a + b + x; };\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureLocalFunc) {
  /* 语句级局部函数带捕获：提升 + 定义点绑定 → f(1) = 42 + 1 = 43 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureTdzCompileTimeRejected) {
  /* 闭包捕获 TDZ：定义点前引用有捕获局部函数 → 编译期报错（exit≠0），
     不静默到运行期（捕获槽 undefined 占位参与运算报 "operator +"） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  printf(\"%d\\n\", f(1));\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureTdzAfterDefinitionOk) {
  /* 定义点之后引用有捕获局部函数：捕获已绑定 → 正常输出 43 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureNoCaptureForwardRefOk) {
  /* 无捕获局部函数定义点前调用：hoist 只绑定地址，无捕获槽 → 合法 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  printf(\"%d\\n\", g(1));\n"
      "  func g(v: i32): i32 { return v + 1; }\n"
      "  printf(\"%d\\n\", g(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureLoopRebind) {
  /* 循环内重建闭包：每次迭代捕获当前 i 的 clone → 0*10 + 1*10 + 2*10 = 30 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var total = 0;\n"
      "  for (var i = 0; i < 3; i = i + 1) {\n"
      "    var f = func |i| mul(n: i32): i32 { return i * n; };\n"
      "    total = total + f(10);\n"
      "  }\n"
      "  printf(\"%d\\n\", total);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureCaptureUndefinedRejected) {
  /* 捕获列表中引用未定义变量 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func |nope| add(x: i32): i32 { return nope + x; };\n"
      "  var val = f(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureFuncTypeCaptureRejected) {
  /* 函数类型签名不能带捕获列表（闭包不参与类型） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f: func |x| (i32)->i32 = func(x:i32):i32 { return x; };\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureGlobalFuncCaptureRejected) {
  /* 全局函数不允许捕获列表 */
  std::string path = write_temp_file(
      "func |x| g(v: i32): i32 { return x + v; }\n"
      "func main():void {\n"
      "  var val = g(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 全局变量（运行时实体，init 编译期折叠）                             */
/* ================================================================ */

TEST(Driver, RunFileGlobalVarRead) {
  /* 全局变量函数体内读取（经 func_vcall root_scope 接线） */
  std::string path = write_temp_file(
      "var g: i32 = 42;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", g);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarMutateAcrossFuncs) {
  /* 全局变量跨函数修改：setg 写 root_scope，main 读到新值 */
  std::string path = write_temp_file(
      "var g: i32 = 42;\n"
      "var s: str = \"hello\";\n"
      "func setg(v: i32):i32 { g = v; return 0; }\n"
      "func main():i32 {\n"
      "  printf(\"%d %s\\n\", g, s);\n"
      "  _ = setg(100);\n"
      "  printf(\"%d\\n\", g);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarFuncRefInit) {
  /* 全局变量 init 折叠为函数引用：var f = add; 调用 f */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "var f = add;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", f(3, 4));\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarExprInit) {
  /* 全局变量 init 为编译期可计算表达式：折叠为字面量发射 */
  std::string path = write_temp_file(
      "var x = (1 + 2) * 3 - 4;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", x);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarInitNotConstantRejected) {
  /* 全局变量 init 引用其他全局变量（运行期实体）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "var a: i32 = 1;\n"
      "var b: i32 = a;\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarUnknownTypeRejected) {
  /* 非法类型名（string 非内建，内建为 str）→ sema 拒绝，不落到运行时 */
  std::string path = write_temp_file(
      "var s: string = \"hi\";\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- switch 语句端到端（docs m2-design §4：if 语法糖） ---- */

TEST(Driver, RunFileSwitchMatchReturnsCase) {
  /* 基本匹配：命中 case 分支执行并返回 */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1)->{ return 10; }\n"
      "    (2)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(1) != 10) { return 1; }\n"
      "  if (pick(2) != 20) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDefaultFallback) {
  /* default 兜底：未命中任何 case 走 default */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1)->{ return 10; }\n"
      "    (2)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(7) != 30) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchMultiPattern) {
  /* 多模式（逗号 = || 链）：命中任一模式即执行该分支 */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1, 2)->{ return 10; }\n"
      "    (3, 4, 5)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(1) != 10) { return 1; }\n"
      "  if (pick(2) != 10) { return 2; }\n"
      "  if (pick(4) != 20) { return 3; }\n"
      "  if (pick(5) != 20) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchNoFallthrough) {
  /* 无 fallthrough：命中分支后不落入后续分支（default 不被执行） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var r = 0;\n"
      "  switch (1) {\n"
      "    (1)->{ r = 100; }\n"
      "    (2)->{ r = 200; }\n"
      "    default->{ r = 300; }\n"
      "  }\n"
      "  if (r != 100) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchRuntimeCond) {
  /* 运行期条件（函数调用 + 算术）：文档定义 switch 条件为运行期表达式 */
  std::string path = write_temp_file(
      "func f():i32 { return 3; }\n"
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  var r = 0;\n"
      "  switch (x + f()) {\n"
      "    (5)->{ r = 50; }\n"
      "    default->{ r = 99; }\n"
      "  }\n"
      "  if (r != 50) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchNested) {
  /* 嵌套 switch：内外层独立判定 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 1;\n"
      "  var y = 2;\n"
      "  var r = 0;\n"
      "  switch (x) {\n"
      "    (1)->{\n"
      "      switch (y) {\n"
      "        (2)->{ r = 12; }\n"
      "        default->{ r = 19; }\n"
      "      }\n"
      "    }\n"
      "    default->{ r = 90; }\n"
      "  }\n"
      "  if (r != 12) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchStrCond) {
  /* str 条件：模式可为字符串字面量 */
  std::string path = write_temp_file(
      "func pick(s:str):i32 {\n"
      "  switch (s) {\n"
      "    (\"one\")->{ return 1; }\n"
      "    (\"two\", \"three\")->{ return 23; }\n"
      "    default->{ return 0; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(\"one\") != 1) { return 1; }\n"
      "  if (pick(\"three\") != 23) { return 2; }\n"
      "  if (pick(\"zzz\") != 0) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDefiniteAssignAllPaths) {
  /* 有 default：全分支赋值 → 确定性初始化，switch 后读取合法 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  var r:i32 = undefined;\n"
      "  switch (x) {\n"
      "    (1)->{ r = 10; }\n"
      "    (2)->{ r = 20; }\n"
      "    default->{ r = 30; }\n"
      "  }\n"
      "  if (r != 20) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchTypeMismatchRejected) {
  /* 模式与 cond 类型不可比 → 编译期拒绝，运行时不产出 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  switch (x) {\n"
      "    (\"a\")->{ return 1; }\n"
      "    default->{ return 2; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDuplicateDefaultRejected) {
  /* 重复 default → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  switch (x) {\n"
      "    (1)->{ return 1; }\n"
      "    default->{ return 2; }\n"
      "    default->{ return 3; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}




/* ---- enum（枚举）---- */

TEST(Driver, RunFileEnumDeclAndComparePasses) {
  /* enum 声明 + main 内 var 初始化 + 同 enum 判等（值断言写在程序内） */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == Color::Red) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumTwoStepCastPasses) {
  /* enum → 声明底层 i32 → i8 两步 cast 通过 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  var v: i8 = c as i32 as i8;\n"
      "  if (v == 1) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumMultiVariantCompare) {
  /* 多 variant 判等：Red==Green → false 分支 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Green;\n"
      "  if (c == Color::Red) { return 1; }\n"
      "  if (c != Color::Green) { return 2; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumGlobalVarInit) {
  /* 全局 enum 变量 init 折叠（Color::Red 折叠为 AST_ENUM_REF 字面量） */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2 }\n"
      "var g: Color = Color::Green;\n"
      "func main():i32 {\n"
      "  if (g == Color::Green) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumU8UnderlyingPasses) {
  /* u8 底层 + 值超出底层范围（300 超出 u8）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:u8 { Red = 300 }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumStrictSeparationRejected) {
  /* 严格分离：enum vs 底层 i32 判等 → 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == 1) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumVariantValueNarrowingRejected) {
  /* variant 值与底层不兼容（i32 字面量赋给 i8 底层）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i8 { Red = 300 }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumSkipStepCastRejected) {
  /* 跳步 cast（enum → i8 而非声明底层 i32）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  var v: i8 = c as i8;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumAssignIntRejected) {
  /* enum 变量被整型赋值（i32 → enum 无隐式转换）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  c = 5;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalDefInFunc) {
  /* 局部 enum（函数体内定义）：声明/使用/判等/两步 cast/三元组合 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  enum Color:i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == Color::Red) {} else { return 1; }\n"
      "  var v: i8 = c as i32 as i8;\n"
      "  if (v != 1) { return 2; }\n"
      "  var tag: i32 = (c == Color::Blue) ? 100 : 200;\n"
      "  if (tag != 200) { return 3; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalNestedBlocksShadow) {
  /* 局部 enum 在嵌套块中定义 + 同名遮蔽（外层全局 vs 内层块） */
  std::string path = write_temp_file(
      "enum A:i32 { X = 1, Y = 2 }\n"
      "func main():i32 {\n"
      "  var total: i32 = 0;\n"
      "  {\n"
      "    enum A:i32 { X = 10, Y = 20 }\n"
      "    var a: A = A::Y;\n"
      "    total += a as i32;\n"
      "  }\n"
      "  var g: A = A::X;\n"
      "  total += g as i32;\n"
      "  if (total != 21) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalForwardRef) {
  /* 局部 enum 前向引用（提升语义：名字整个块内可见，与局部 type def 一致） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  enum Color:i32 { Red = 1, Green = 2 }\n"
      "  if (c != Color::Red) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalInLoopBody) {
  /* 局部 enum 在 for 循环体内：每轮定义独立（运行时名字绑定在块作用域） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  for (var i: i32 = 0; i < 2; i = i + 1) {\n"
      "    enum E:i32 { A = 1, B = 2 }\n"
      "    var e: E = E::B;\n"
      "    if (e != E::B) { return 1; }\n"
      "  }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalStrictSeparationRejected) {
  /* 局部 enum 严格分离同样生效：variant 不能赋给整型变量 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  enum Color:i32 { Red = 1 }\n"
      "  var x: i32 = Color::Red;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalVariantNotConstRejected) {
  /* 局部 enum variant 值引用运行时变量 → 编译期拒绝（须编译期常量） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var n: i32 = 5;\n"
      "  enum Color:i32 { Red = n }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- struct（结构体）---- */

TEST(Driver, RunFileStructDeclAndVarPasses) {
  /* struct 声明 + 声明 struct 类型变量（无值构造，仅类型定义） */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var p: Point = undefined;\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructLocalDefPasses) {
  /* 局部 struct（函数体内定义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  struct Point { x: i32; y: i32; }\n"
      "  var p: Point = undefined;\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructNestedFieldTypePasses) {
  /* 字段类型可为其他 struct 类型（布局依赖构造） */
  std::string path = write_temp_file(
      "struct Inner { a: i32; }\n"
      "struct Outer { i: Inner; b: i64; }\n"
      "func main():i32 {\n"
      "  var o: Outer = undefined;\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructDuplicateFieldRejected) {
  /* 重复字段名 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: i32; x: i64; }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructUnknownFieldTypeRejected) {
  /* 未知字段类型 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: Nope; }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEmptyFieldListRejected) {
  /* 空字段列表 {} → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
