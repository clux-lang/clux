/* defer（延迟执行块）—— 语法 / 捕获 / LIFO / 执行时机
 *
 *   defer ['<a,b>] [|cap, (name: T = init)|] { body }
 *
 * 语义（用户规格）：语义上等价于"在该作用域里创建了一个**带闭包的无参函数
 * 值**"，复用仓库既有的闭包捕获机制（捕获可见性 / 作用域标注 / 生命周期规则
 * 逐字沿用）；该值被压入**当前作用域的 defer 栈**，作用域退出（pop_scope）时
 * 在**内存释放（own/fatal dispose、scope 销毁）之前**把栈**逆序**弹出来逐个
 * 调用。
 *
 * 本文件覆盖：
 *   1. LIFO 顺序（同作用域三个 defer 逆序执行，拼接值判定）
 *   2. 捕获语义（clone 值而非引用：登记后修改外层变量，defer 读到的是登记时的值）
 *   3. 多作用域嵌套（内层先于外层）
 *   4. 循环体每次迭代各执行一次（迭代内 LIFO；continue/break 也各跑一次）
 *   5. return 路径（含嵌套被弹出作用域）
 *   6. defer 与 own 释放顺序（defer 体内通过 ref 捕获读到 own 堆块内容 → 证明
 *      它在 dispose 之前跑）
 *   7. 空捕获列表两种写法（`defer {` 与 `defer || {`）等价
 *   8. defer 体内嵌套 defer / switch 分支内的 defer / defer 调用函数
 *   9. 负例：未知捕获名、捕获类型名、own 裸捕获、ref 捕获缺标注、未初始化捕获、
 *      读未捕获的外层局部、defer 体内 return 值、comptime 内 defer、缺块体
 *
 * 判定夹具与 tests/struct_compose_test.cpp 同款：clux 程序把检查结果写进测试库
 * 的 td_i32，宿主侧读回；expect_verdict 断言 rc==0 且 td_i32==0，
 * expect_compile_error 断言 rc!=0（编译失败）。夹具自身的 allocator 泄漏检查
 * 与 driver 内部 allocator 的 "memory leak detected" 报告共同覆盖"无泄漏"。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "vm/ffi_lib.h"
}

#include "test_common.h"

namespace {

#if defined(_WIN32)
const char *kTestLib = "clux_ffi_testdata.dll";
#else
const char *kTestLib = "libclux_ffi_testdata.so";
#endif

void *df_alloc(size_t size) { return std::malloc(size); }
void df_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(df_alloc, df_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class DeferTest : public ::testing::Test {
 protected:
  static LibKeeper *keep;
  int32_t saved = 0;

  static void SetUpTestSuite() { keep = new LibKeeper(); }
  static void TearDownTestSuite() {
    delete keep;
    keep = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(keep->lib, nullptr) << "cannot open " << kTestLib;
    int32_t *v = verdict();
    ASSERT_NE(v, nullptr);
    saved = *v;
    *v = -1;
  }
  void TearDown() override {
    if (int32_t *v = verdict()) *v = saved;
  }

  int32_t *verdict() {
    char err[256] = {0};
    return keep->lib
               ? (int32_t *)ffi_lib_sym(keep->lib, "td_i32", err, sizeof err)
               : nullptr;
  }

  void expect_verdict(const std::string &body) {
    static int counter = 0;
    std::string src =
        "extern lib \"" +
        std::string(kTestLib) +
        "\" as td;\n"
        "extern var td::td_i32: i32;\n" +
        body +
        "func report(v: i32): i32 { td::td_i32 = v; return v; }\n";
    std::string path =
        (clux_test_path() /
         ("clux_defer_" + std::to_string(counter++) + ".cx"))
            .string();
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src.data(), 1, src.size(), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    ASSERT_EQ(rc, 0) << src;
    int32_t *v = verdict();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 0) << "clux verdict=" << *v << "\n" << src;
  }

  void expect_compile_error(const char *src) {
    static int counter = 0;
    std::string path =
        (clux_test_path() /
         ("clux_defer_bad_" + std::to_string(counter++) + ".cx"))
            .string();
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src, 1, strlen(src), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    EXPECT_NE(rc, 0) << src;
  }
};

LibKeeper *DeferTest::keep = nullptr;

}  // namespace

/* ---- 1. LIFO：同作用域三个 defer 逆序执行（拼接值判定：123 → 321） ---- */
TEST_F(DeferTest, LifoOrderSameScope) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  {\n"
      "    defer { seq = seq * 10 + 1; }\n"
      "    defer { seq = seq * 10 + 2; }\n"
      "    defer { seq = seq * 10 + 3; }\n"
      "    if (seq != 0) { return report(9); }\n"   /* 登记时不执行 */
      "  }\n"
      "  if (seq != 321) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 2. 空捕获列表两种写法等价：`defer {` = `defer || {` ---- */
TEST_F(DeferTest, EmptyCaptureFormsAreEquivalent) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  {\n"
      "    defer { seq = seq * 10 + 1; }\n"
      "    defer || { seq = seq * 10 + 2; }\n"
      "    defer | | { seq = seq * 10 + 3; }\n"    /* 有空白也算空列表 */
      "  }\n"
      "  if (seq != 321) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 3. 捕获是**值**（clone 语义，与闭包逐字一致）：
 * 登记后修改外层变量，defer 体内读到的是**登记时**的值。 ---- */
TEST_F(DeferTest, CaptureIsCloneOfValueAtRegistration) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var a: i32 = 7;\n"
      "  {\n"
      "    defer |a| { seq = seq * 10 + a; }\n"
      "    a = 1000;\n"                  /* 登记之后改：不影响已捕获的副本 */
      "  }\n"
      "  if (seq != 7) { return report(1); }\n"
      "  if (a != 1000) { return report(2); }\n"   /* 原变量本身被改了 */
      "  return report(0);\n"
      "}\n");
}

/* ---- 4. 多捕获两种形态：纯 id + 括号 VALUE DECL（与闭包同一套解析） ---- */
TEST_F(DeferTest, MultipleCapturesBothForms) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var a: i32 = 5;\n"
      "  var c: i32 = 100;\n"
      "  var d: i32 = 7;\n"
      "  {\n"
      "    defer |a, (b: i32 = c + d)| { seq = a + b; }\n"
      "    a = 0;\n"
      "    c = 0;\n"
      "    d = 0;\n"
      "  }\n"
      "  if (seq != 112) { return report(1); }\n"   /* 5 + (100+7) */
      "  return report(0);\n"
      "}\n");
}

/* ---- 5. 多作用域嵌套：内层先于外层（各自 LIFO）；函数体顶层 defer 在
 *        return 后的收尾（func_vcall 逐层 pop_scope）里执行 ---- */
TEST_F(DeferTest, NestedScopesInnerFirst) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func worker(): i32 {\n"
      "  defer { seq = seq * 10 + 1; }\n"        /* 函数体作用域：最后跑 */
      "  {\n"
      "    defer { seq = seq * 10 + 2; }\n"
      "    {\n"
      "      defer { seq = seq * 10 + 3; }\n"
      "      defer { seq = seq * 10 + 4; }\n"
      "    }\n"
      "    if (seq != 43) { return 9; }\n"        /* 只有最内层块跑过 */
      "  }\n"
      "  if (seq != 432) { return 8; }\n"         /* 中层跑过，函数体层未跑 */
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: i32 = worker();\n"
      "  if (r != 0) { return report(9); }\n"
      "  if (seq != 4321) { return report(1); }\n"  /* return 收尾跑完三层 */
      "  return report(0);\n"
      "}\n");
}

/* ---- 6. 循环体：每次迭代结束时执行该次迭代的 defer（迭代内 LIFO） ---- */
TEST_F(DeferTest, LoopBodyRunsPerIterationLifo) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  for (var i = 0; i < 3; i = i + 1) {\n"
      "    defer |i| { seq = seq * 10 + i; }\n"
      "    defer |i| { seq = seq * 10 + i + 1; }\n"
      "  }\n"
      /* iter0: 1,0 → 10；iter1: 2,1 → 1021；iter2: 3,2 → 102132 */
      "  if (seq != 102132) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 7. break / continue 跳出作用域同样执行 defer ---- */
TEST_F(DeferTest, BreakAndContinueRunDefers) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var i: i32 = 0;\n"
      "  while (i < 4) {\n"
      "    i = i + 1;\n"
      "    defer |i| { seq = seq * 10 + i; }\n"
      "    if (i == 2) { continue; }\n"   /* 该次迭代的 defer 在 continue 前跑 */
      "    if (i == 3) { break; }\n"      /* break 跳出循环体块也跑 */
      "  }\n"
      "  if (seq != 123) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 8. return：当前作用域与所有被弹出作用域的 defer 都执行 ---- */
TEST_F(DeferTest, ReturnRunsDefersOfAllPoppedScopes) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func inner(): i32 {\n"
      "  defer { seq = seq * 10 + 1; }\n"
      "  {\n"
      "    defer { seq = seq * 10 + 2; }\n"
      "    {\n"
      "      defer { seq = seq * 10 + 3; }\n"
      "      return 5;\n"                  /* 从最内层块 return */
      "    }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: i32 = inner();\n"
      "  if (r != 5) { return report(9); }\n"
      "  if (seq != 321) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 9. defer 与 own 释放顺序：defer 体内通过 ref 捕获**读 own 堆块内容**
 *（own 变量的 dispose 在 defer **之后**——若在之前，这里读到的是已释放内存） ---- */
TEST_F(DeferTest, DeferRunsBeforeOwnRelease) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "struct Box { v: i32; w: i32; }\n"
      "func main(): i32 {\n"
      "  {\n"
      "    var p: own *i32 = new i32{ 42 };\n"
      "    var b: own *Box = new Box{ .v = 5, .w = 6 };\n"
      /* ref 捕获须带作用域标注（与闭包同规则） */
      "    defer '<*> |(r: ref *i32 = p.*.&)| { seq = seq * 100 + r.*; }\n"
      "    defer '<*> |(q: ref *Box = b.*.&)| { seq = seq * 100 + q.w; }\n"
      "    if (p.* != 42 || b.v != 5) { return report(9); }\n"
      "  }\n"
      /* 逆序：先 q.w=6 → 6；再 r.*=42 → 642 */
      "  if (seq != 642) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 10. defer 体内可以调用函数（与普通函数体同一套语义） ---- */
TEST_F(DeferTest, DeferBodyCallsFunctions) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func bump(n: i32): i32 { seq = seq * 10 + n; return n; }\n"
      "func main(): i32 {\n"
      "  {\n"
      "    defer { _ = bump(1); }\n"
      "    defer { _ = bump(2); _ = bump(3); }\n"   /* 表达式语句结果显式丢弃 */
      "  }\n"
      /* 逆序：第二块先跑（2,3），再第一块（1） → 231 */
      "  if (seq != 231) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 11. defer 体内再登记 defer：内层在闭包自身作用域退出时执行 ---- */
TEST_F(DeferTest, NestedDeferInsideDeferBody) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  {\n"
      "    defer {\n"
      "      seq = 1;\n"
      "      defer { seq = seq * 10 + 2; }\n"
      "      seq = seq * 10 + 3;\n"        /* 1 → 13 */
      "    }\n"
      "  }\n"
      "  if (seq != 132) { return report(1); }\n"   /* 内层 defer：13 → 132 */
      "  return report(0);\n"
      "}\n");
}

/* ---- 12. switch 分支体内的 defer：分支块退出时执行 ---- */
TEST_F(DeferTest, SwitchCaseDefer) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var k: i32 = 2;\n"
      "  switch (k) {\n"
      "    (1) -> { defer { seq = 91; } }\n"
      "    (2) -> { defer { seq = 92; } }\n"
      "    default -> { defer { seq = 93; } }\n"
      "  }\n"
      "  if (seq != 92) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 13. defer 在函数体顶层：函数 scope 退出（return 兜底）时执行 ---- */
TEST_F(DeferTest, FunctionBodyTopLevelDefer) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func worker(): void {\n"
      "  defer { seq = seq * 10 + 1; }\n"
      "  defer { seq = seq * 10 + 2; }\n"
      "  seq = 9;\n"
      "  return;\n"                      /* 显式 return 同样触发 */
      "}\n"
      "func main(): i32 {\n"
      "  worker();\n"
      "  if (seq != 921) { return report(1); }\n"   /* 9 → 92 → 921 */
      "  return report(0);\n"
      "}\n");
}

/* ===========================================================================
 * 负例：语法 / 捕获 / 语义
 * =========================================================================== */

/* ---- 未知捕获名 / 捕获类型名 / 未初始化捕获（捕获规则与闭包同一套诊断） ---- */
TEST_F(DeferTest, CaptureResolutionErrors) {
  expect_compile_error(
      "func main(): i32 { defer |zzz| { } return 0; }\n");
  expect_compile_error(
      "type T = i32;\n"
      "func main(): i32 { defer |T| { } return 0; }\n");
  expect_compile_error(
      "func main(): i32 { var a: i32 = undefined; defer |a| { } return 0; }\n");
  /* 读未捕获的外层局部：defer 体就是闭包函数体，外层局部必须显式捕获 */
  expect_compile_error(
      "func main(): i32 { var a: i32 = 1; defer { a = a + 1; } return 0; }\n");
}

/* ---- own/fatal 裸捕获被拒（与闭包 §9.1 同规则）；显式 move() 接管则合法 ---- */
TEST_F(DeferTest, OwnCaptureByValueRejected) {
  expect_compile_error(
      "func main(): i32 { var p: own *i32 = new i32{ 1 };\n"
      "  defer |p| { } return 0; }\n");
}

/* ---- own 经 move() 转移进 defer（闭包持有所有权，§9.2 的合法口径） ---- */
TEST_F(DeferTest, OwnCaptureByMoveIsAllowed) {
  expect_verdict(
      "var seq: i32 = 0;\n"
      "func main(): i32 {\n"
      "  {\n"
      "    var p: own *i32 = new i32{ 41 };\n"
      "    defer |(q: own *i32 = move(p))| { seq = q.* + 1; }\n"
      "  }\n"
      "  if (seq != 42) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- ref 捕获必须带作用域标注（与闭包 §9.3 同规则） ---- */
TEST_F(DeferTest, RefCaptureNeedsScopeAnnotation) {
  expect_compile_error(
      "func main(): i32 { var a: i32 = 1;\n"
      "  defer |(r: ref *i32 = a.&)| { } return 0; }\n");
}

/* ---- defer 体是**无参 void 函数体**：return 值 / 参数表 / 缺块体都非法 ---- */
TEST_F(DeferTest, DeferBodyIsVoidNoParamFunction) {
  expect_compile_error(
      "func main(): i32 { defer { return 3; } return 0; }\n");
  expect_compile_error(
      "func main(): i32 { defer (x: i32) { } return 0; }\n");
  expect_compile_error(
      "func main(): i32 { defer x; return 0; }\n");
  expect_compile_error(
      "func main(): i32 { defer |a b| { } return 0; }\n");
}

/* ---- break / continue 不能跨出 defer（它就是另一个函数的函数体） ---- */
TEST_F(DeferTest, BreakInsideDeferRejected) {
  expect_compile_error(
      "func main(): i32 { defer { break; } return 0; }\n");
  expect_compile_error(
      "func main(): i32 {\n"
      "  for (var i = 0; i < 1; i = i + 1) { defer { continue; } }\n"
      "  return 0; }\n");
}

/* ---- 顶层 defer 非法（defer 是语句，只能出现在函数体内） ---- */
TEST_F(DeferTest, TopLevelDeferRejected) {
  expect_compile_error(
      "defer { }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- comptime 求值内 defer 正常工作（defer 改写为编译期语法糖后，
 *        __defer_N 函数 + call 在 CTFE 中自然执行，不再依赖运行期作用域退出） ---- */
TEST_F(DeferTest, ComptimeDeferWorks) {
  expect_verdict(
      "comptime func f(): i32 {\n"
      "  var x: i32 = 1;\n"
      "  defer |x| { x = x + 10; }\n"
      "  defer |x| { x = x + 100; }\n"
      "  return x;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: i32 = f();\n"
      "  if (r != 1) { return report(9); }\n"
      "  return report(0);\n"
      "}\n");
}
