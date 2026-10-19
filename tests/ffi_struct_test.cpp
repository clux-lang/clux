/* M6 FFI：**聚合类型**（struct / cunion / enum / char*）跨 C ABI 的完整支持
 * （docs/m6-design.md §14.20）
 *
 * clux 的 struct 布局就是 C 布局（编译器保证平凡同构），因此两侧同名结构体逐字节
 * 对应。覆盖 x64 的三类传参/返回 ABI 类别：
 *   - 8 字节（INTEGER，走整数寄存器 RAX）
 *   - 16 字节全 double（SSE，走 XMM0/XMM1）
 *   - 24 字节（MEMORY，走栈 / 隐藏指针对）
 * 以及：嵌套结构体、指向结构体的指针、变参里的结构体、结构体回调、enum、char*。
 *
 * 被测函数来自 `tests/ffi_testdata.c` 编出的测试动态库（见 CMakeLists 的
 * `clux_ffi_testdata` SHARED 目标）。
 *
 * **判定方式**：clux 程序把自己的检查结果写进库里的 `td_i32`（数据符号），宿主侧
 * 读回断言——`driver_run_file` 的返回值只表示"编译并运行成功"，不代表 main 的返回
 * 值，所以不能用它判定语义正确性。
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

void *td_alloc(size_t size) { return std::malloc(size); }
void td_free(void *p) { std::free(p); }

/* 长期持有的库句柄：句柄归零会卸载 DLL 并**重置导出变量**，宿主侧就读不到 clux
   写进去的判定值了（见 docs/m6-design.md §14.17.3）。 */
struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(td_alloc, td_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
  int32_t *i32() {
    char err[256] = {0};
    return lib ? (int32_t *)ffi_lib_sym(lib, "td_i32", err, sizeof err) : nullptr;
  }
  /* 库里的 CluxPoint { i32 x; i32 y; } */
  int32_t *point() {
    char err[256] = {0};
    return lib ? (int32_t *)ffi_lib_sym(lib, "td_point", err, sizeof err) : nullptr;
  }
  int32_t *color() {
    char err[256] = {0};
    return lib ? (int32_t *)ffi_lib_sym(lib, "td_color", err, sizeof err) : nullptr;
  }
};

/* 每个测试结束时把库里的判定值/被改写的数据符号还原，避免互相污染 */
class FfiStructTest : public ::testing::Test {
 protected:
  static LibKeeper *keep;
  int32_t saved_point[2] = {0, 0};
  int32_t saved_color = 0;

  static void SetUpTestSuite() { keep = new LibKeeper(); }
  static void TearDownTestSuite() {
    delete keep;
    keep = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(keep->lib, nullptr) << "cannot open " << kTestLib;
    int32_t *p = keep->point();
    if (p) {
      saved_point[0] = p[0];
      saved_point[1] = p[1];
    }
    int32_t *c = keep->color();
    if (c) saved_color = *c;
    if (int32_t *v = keep->i32()) *v = -1; /* -1 = clux 没写判定值（异常信号） */
  }

  void TearDown() override {
    if (int32_t *p = keep->point()) {
      p[0] = saved_point[0];
      p[1] = saved_point[1];
    }
    if (int32_t *c = keep->color()) *c = saved_color;
    if (int32_t *v = keep->i32()) *v = -123456; /* 还原初始值 */
  }

  /* 写临时 clux 源文件（自动补 printf 前言）并运行；返回驱动状态 */
  int run(const std::string &src) {
    std::string path =
        (clux_test_path() /
         ("clux_ffi_struct_" + std::to_string(counter_++) + ".cx"))
            .string();
    std::string full = clux_with_printf_prelude(src);
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return -1;
    fwrite(full.data(), 1, full.size(), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    return rc;
  }

  /* clux 程序写回判定值（0 = 全部通过） */
  void expect_verdict(const std::string &src) {
    ASSERT_EQ(run(src), 0) << src;
    int32_t *v = keep->i32();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 0) << "clux verdict=" << *v << "\n" << src;
  }

 private:
  static int counter_;
};

LibKeeper *FfiStructTest::keep = nullptr;
int FfiStructTest::counter_ = 0;

/* 程序里常见的收尾：把判定变量写进库里的 td_i32 */
const char *kReport = "  td::td_i32 = verdict;\n  return verdict;\n}\n";

std::string prelude() {
  return std::string("extern lib td from \"") + kTestLib +
         "\";\n"
         "extern cfunc td::clux_point_sum(p: Point): i32;\n"
         "extern cfunc td::clux_dpoint_sum(p: DPoint): f64;\n"
         "extern cfunc td::clux_big_sum(v: Big): i64;\n"
         "extern cfunc td::clux_point_make(x: i32, y: i32): Point;\n"
         "extern cfunc td::clux_dpoint_make(x: f64, y: f64): DPoint;\n"
         "extern cfunc td::clux_big_make(a: i32, b: i64, c: f64): Big;\n"
         "extern cfunc td::clux_nested_sum(v: Nested): i32;\n"
         "extern cfunc td::clux_nested_make(x: i32, y: i32, z: i32): Nested;\n"
         "extern cfunc td::clux_point_ptr_sum(p: ref *Point): i32;\n"
         "extern cfunc td::clux_point_va_sum(n: i32, ...): i32;\n"
         "extern cfunc td::clux_color_next(c: Color): Color;\n"
         "extern cfunc td::clux_greeting(): str;\n"
         "extern cfunc td::clux_call_point_cb(cb: cfunc(Point) -> Point, x: i32, y: i32): i32;\n"
         "extern var td::td_point: Point;\n"
         "extern var td::td_color: Color;\n"
         "extern var td::td_i32: i32;\n";
}

const char *kTypes =
    "struct Point { x: i32; y: i32; }\n"
    "struct DPoint { x: f64; y: f64; }\n"
    "struct Big { a: i32; b: i64; c: f64; }\n"
    "struct Inner { a: i32; }\n"
    "struct Nested { p: Point; z: i32; }\n"
    "enum Color: i32 { Red = 1, Green = 2 }\n";

}  // namespace

/* ① 按值传 struct：三种 ABI 类别（8B / 16B-SSE / 24B-MEMORY） */
TEST_F(FfiStructTest, StructByValueParameters) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var p: Point = .Point{ .x = 10, .y = 32 };\n"
                 "  if (td::clux_point_sum(p) != 42) { verdict = 1; }\n"
                 "  var d: DPoint = .DPoint{ .x = 1.5, .y = 2.25 };\n"
                 "  if (td::clux_dpoint_sum(d) != 3.75) { verdict = 2; }\n"
                 "  var b: Big = .Big{ .a = 1, .b = 2, .c = 3.5 };\n"
                 "  if (td::clux_big_sum(b) != 6) { verdict = 3; }\n" +
                 kReport);
}

/* ② 按值返回 struct：8B 走 RAX、16B SSE 走 XMM、24B 走隐藏指针 */
TEST_F(FfiStructTest, StructByValueReturns) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var made: Point = td::clux_point_make(7, 8);\n"
                 "  if (made.x != 7 || made.y != 8) { verdict = 1; }\n"
                 "  var dmade: DPoint = td::clux_dpoint_make(4.5, 5.5);\n"
                 "  if (dmade.x != 4.5 || dmade.y != 5.5) { verdict = 2; }\n"
                 "  var bmade: Big = td::clux_big_make(9, 10, 0.5);\n"
                 "  if (bmade.a != 9 || bmade.b != 10 || bmade.c != 0.5) { verdict = 3; }\n"
                 "  // 用返回值继续当实参传回去（验证返回的值是完整的 C 布局）\n"
                 "  if (td::clux_point_sum(made) != 15) { verdict = 4; }\n"
                 "  if (td::clux_big_sum(bmade) != 19) { verdict = 5; }\n" +
                 kReport);
}

/* ③ 嵌套结构体：描述符递归构造（外层含内层 struct 字段） */
TEST_F(FfiStructTest, NestedStructByValue) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var v: Nested = .Nested{ .p = .Point{ .x = 20, .y = 30 }, .z = 4 };\n"
                 "  if (td::clux_nested_sum(v) != 54) { verdict = 1; }\n"
                 "  var made: Nested = td::clux_nested_make(1, 2, 3);\n"
                 "  if (made.p.x != 1 || made.p.y != 2 || made.z != 3) { verdict = 2; }\n" +
                 kReport);
}

/* ④ 指向结构体的指针（`ref *Point`）：修掉 pass 顺序缺陷后才能解析 */
TEST_F(FfiStructTest, PointerToStruct) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var p: Point = .Point{ .x = 6, .y = 7 };\n"
                 "  if (td::clux_point_ptr_sum(p.&) != 13) { verdict = 1; }\n" +
                 kReport);
}

/* ⑤ 变参里的结构体（C 对聚合类型不做默认提升） */
TEST_F(FfiStructTest, StructInVariadicArguments) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var a: Point = .Point{ .x = 1, .y = 2 };\n"
                 "  var b: Point = .Point{ .x = 3, .y = 4 };\n"
                 "  if (td::clux_point_va_sum(2, a, b) != 10) { verdict = 1; }\n" +
                 kReport);
}

/* ⑥ struct 数据符号：读库里那块内存 + 整块写回（宿主侧复核） */
TEST_F(FfiStructTest, StructDataSymbol) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var g: Point = td::td_point;\n"
                 "  if (g.x != 3 || g.y != 4) { verdict = 1; }\n"
                 "  td::td_point = .Point{ .x = 55, .y = 66 };\n"
                 "  var after: Point = td::td_point;\n"
                 "  if (after.x != 55 || after.y != 66) { verdict = 2; }\n" +
                 kReport);
  /* 宿主侧独立复核：clux 写进库里的确实是那块内存（TearDown 会还原） */
  int32_t *p = keep->point();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p[0], 55);
  EXPECT_EQ(p[1], 66);
}

/* ⑦ enum：按值传参/返回 + enum 数据符号（clux enum 有精确类型，不能退化成 i32） */
TEST_F(FfiStructTest, EnumParamReturnAndDataSymbol) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var c: Color = td::clux_color_next(Color::Red);\n"
                 "  if (c != Color::Green) { verdict = 1; }\n"
                 "  if (td::clux_color_next(Color::Green) != Color::Red) { verdict = 2; }\n"
                 "  var g: Color = td::td_color;\n"
                 "  if (g != Color::Green) { verdict = 3; }\n"
                 "  td::td_color = Color::Red;\n"
                 "  if (td::td_color != Color::Red) { verdict = 4; }\n" +
                 kReport);
  int32_t *c = keep->color();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(*c, 1); /* CLUX_COLOR_RED：clux 写进库里的值（TearDown 还原） */
}

/* ⑧ char* 返回（clux 侧声明为 str）：长度与内容都要对。内容用 libc 的 strcmp
   比对（str 不能下标），顺带验证 clux 的 str 实参也能传给 C 的 char* 形参。 */
TEST_F(FfiStructTest, CharPtrReturnAsStr) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "extern lib libc from \"*\";\n"
                 "extern cfunc libc::strcmp(a: str, b: str): i32;\n"
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var g: str = td::clux_greeting();\n"
                 "  if (len(g) != 19u64) { verdict = 1; }\n"
                 "  if (libc::strcmp(g, \"hello from testdata\") != 0) { verdict = 2; }\n" +
                 kReport);
}

/* ⑨ 回调里的结构体：C 按值把 struct 传给 clux 函数，并接收它按值返回的 struct */
TEST_F(FfiStructTest, StructInCallback) {
  expect_verdict(std::string(kTypes) + prelude() +
                 "func double_point(p: Point): Point {\n"
                 "  return .Point{ .x = p.x * 2, .y = p.y * 3 };\n"
                 "}\n"
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var r: i32 = td::clux_call_point_cb(cfunc_ptr(double_point), 5, 7);\n"
                 "  if (r != 1021) { verdict = 1; }\n" +
                 kReport);
}

/* ⑪ 程序自定义类型可以出现在 FFI 签名里（pass1c 回归）：
   - `type` 别名、指向结构体的指针、enum 都通过；
   - 拼错的类型名给出明确诊断（而不是从前的 "missing type"）。 */
TEST_F(FfiStructTest, ProgramTypesInFfiSignatures) {
  /* 别名 + 指向结构体的指针：编译并运行都应成功（用自己的最小前言，避免与
     prelude() 里的同名声明重复） */
  expect_verdict(std::string(kTypes) +
                 "type Pt = Point;\n"
                 "extern lib td from \"" + kTestLib + "\";\n"
                 "extern var td::td_i32: i32;\n"
                 "extern cfunc td::clux_point_ptr_sum(p: ref *Pt): i32;\n"
                 "func main(): i32 {\n"
                 "  var verdict: i32 = 0;\n"
                 "  var p: Point = .Point{ .x = 6, .y = 7 };\n"
                 "  if (td::clux_point_ptr_sum(p.&) != 13) { verdict = 1; }\n" +
                 kReport);
  /* 未知类型：编译失败（类型解析在程序类型激活之后，诊断要可读） */
  std::string bad = std::string(kTypes) + prelude() +
                    "extern cfunc td::clux_point_sum(p: NoSuchType): i32;\n"
                    "func main(): i32 { return 0; }\n";
  EXPECT_NE(run(bad), 0) << bad;
}

/* ⑫ 容器往返：struct FFI 程序经 `.cxb`（emit → run）语义不变 */
TEST_F(FfiStructTest, StructFfiSurvivesContainerRoundTrip) {
  std::string src = std::string(kTypes) + clux_with_printf_prelude(prelude()) +
                    "func main(): i32 {\n"
                    "  var verdict: i32 = 0;\n"
                    "  var made: Point = td::clux_point_make(7, 8);\n"
                    "  if (td::clux_point_sum(made) != 15) { verdict = 1; }\n"
                    "  var b: Big = .Big{ .a = 1, .b = 2, .c = 3.5 };\n"
                    "  if (td::clux_big_sum(b) != 6) { verdict = 2; }\n" +
                    kReport;
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir());
  std::string path = (dir / "clux_ffi_struct_rt.cx").string();
  std::string bin = (dir / "clux_ffi_struct_rt.cxb").string();
  FILE *fp = fopen(path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  fwrite(src.data(), 1, src.size(), fp);
  fclose(fp);

  ASSERT_EQ(driver_build_bin(path.c_str(), bin.c_str()), 0);
  int32_t *v = keep->i32();
  ASSERT_NE(v, nullptr);
  *v = -1;
  ASSERT_EQ(driver_run_bin(bin.c_str()), 0);
  EXPECT_EQ(*v, 0) << "container round-trip verdict=" << *v;

  std::remove(path.c_str());
  std::remove(bin.c_str());
}
