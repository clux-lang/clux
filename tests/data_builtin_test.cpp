/* `data(s)`：从**序列**取首元素指针（C ABI 适配）
 * 见 docs/language-spec-m1.md §5 与 docs/m6-design.md §14.21。
 *
 * 语义：`data(s) -> ref *T`（T = 元素类型；str → u8，即 C 的 `const char *`）
 *   - own/ref/fatal []T → 指向切片首元素（切片自带缓冲区）
 *   - [N]T              → 指向数组值自身的数据块
 *   - str               → 指向 intern 池里的首字节
 * 恒 `ref`（借用、永不拥有），空序列可能得到 NULL（C 侧不得解引用）。
 * 与 `len(s)` 组成 C 的 `(T*, size_t)` 对：`f(data(s), len(s))`。
 *
 * **它是指令不是函数**（`BCODE_DATA`）：内置 cfunc 的实参会被 clone 进被调
 * 作用域，函数形态只能拿到副本地址（slice 缓冲区副本 / 数组副本块，调用一结束
 * 就悬垂）。取"存储身份"必须像 `x.&` 那样在调用点就地完成——这也是本文件
 * 第 ① 组用例（悬垂回归）的由来。
 *
 * 判定方式同 ffi_struct_test.cpp：clux 程序把检查结果写进库里的 `td_i32`，
 * 宿主侧读回（`driver_run_file` 的返回值只表示"编译并运行成功"）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "vm/ffi_lib.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/vm.h"
}

#include "test_common.h"

namespace {

#if defined(_WIN32)
const char *kTestLib = "clux_ffi_testdata.dll";
#else
const char *kTestLib = "libclux_ffi_testdata.so";
#endif

void *db_alloc(size_t size) { return std::malloc(size); }
void db_free(void *p) { std::free(p); }

std::string temp_path(const std::string &tag) {
  static int counter = 0;
  return (std::filesystem::temp_directory_path() /
          (tag + "_" + std::to_string(counter++) + ".cx"))
      .string();
}

/* 长期句柄：句柄归零会卸载 DLL 并重置导出变量（见 §14.17.3） */
struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(db_alloc, db_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class DataBuiltinTest : public ::testing::Test {
 protected:
  static LibKeeper *keep;
  int32_t saved = 0;

  static void SetUpTestSuite() { keep = new LibKeeper(); }
  static void TearDownTestSuite() {
    delete keep;
    keep = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(keep->lib, nullptr);
    int32_t *v = verdict_slot();
    ASSERT_NE(v, nullptr);
    saved = *v;
    *v = -1;
  }

  void TearDown() override {
    if (int32_t *v = verdict_slot()) *v = saved;
  }

  int32_t *verdict_slot() {
    char err[256] = {0};
    return keep->lib
               ? (int32_t *)ffi_lib_sym(keep->lib, "td_i32", err, sizeof err)
               : nullptr;
  }

  /* 源码 + 前言，运行并把判定值写进 td_i32 */
  void expect_verdict(const std::string &body) {
    std::string src =
        "extern lib \"*\" as libc;\n"
        "extern lib \"" +
        std::string(kTestLib) +
        "\" as td;\n"
        "extern var td::td_i32: i32;\n"
        "extern cfunc td::clux_sum_i32(p: ref *i32, n: usize): i32;\n"
        "extern cfunc td::clux_first_i32(p: ref *i32): i32;\n"
        "extern cfunc td::clux_fill_i32(p: ref *i32, n: usize, v: i32): void;\n"
        "extern cfunc td::clux_str_first_byte(s: ref *u8): i32;\n"
        "func main(): i32 {\n"
        "  var verdict: i32 = 0;\n" +
        body +
        "  td::td_i32 = verdict;\n"
        "  return verdict;\n"
        "}\n";
    std::string path = temp_path("clux_data");
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src.data(), 1, src.size(), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    ASSERT_EQ(rc, 0) << src;
    int32_t *v = verdict_slot();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 0) << "clux verdict=" << *v << "\n" << src;
  }

  /* 期望编译失败（诊断用例） */
  void expect_compile_error(const std::string &src) {
    std::string path = temp_path("clux_data_bad");
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src.data(), 1, src.size(), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    EXPECT_NE(rc, 0) << src;
  }
};

LibKeeper *DataBuiltinTest::keep = nullptr;

}  // namespace

/* ① 悬垂回归：函数形态的实现会返回副本缓冲区的地址（调用一结束即失效）。
   指令形态必须给出**调用方**的真实存储——读写都作用于同一块内存。 */
TEST_F(DataBuiltinTest, PointsIntoCallerStorageNotACopy) {
  expect_verdict(
      "  var s: own []i32 = make(i32, 4, 1, 2, 3, 4);\n"
      "  var p: ref *i32 = data(s);\n"
      "  // 同一块内存：C 通过指针写回，clux 侧能看见\n"
      "  td::clux_fill_i32(data(s), len(s), 90);\n"
      "  if (s[0] != 90 || s[3] != 93) { verdict = 1; }\n"
      "  if (p.* != 90) { verdict = 2; }\n");
}

/* ② (T*, size_t) 形态：切片的求和（含 own / ref 切片） */
TEST_F(DataBuiltinTest, SlicePointerWithLenMatchesCAbi) {
  expect_verdict(
      "  var s: own []i32 = make(i32, 4, 10, 20, 30, 40);\n"
      "  if (td::clux_sum_i32(data(s), len(s)) != 100) { verdict = 1; }\n"
      "  // ref 切片（数组的一段）：仍指向原数组内存\n"
      "  var arr = .[5]i32{1, 2, 3, 4, 5};\n"
      "  var r: ref []i32 = arr[1:4];\n"
      "  if (td::clux_sum_i32(data(r), len(r)) != 9) { verdict = 2; }\n"
      "  if (td::clux_first_i32(data(r)) != 2) { verdict = 3; }\n");
}

/* ③ 数组：data([N]T) 指向数组值自身的数据块 */
TEST_F(DataBuiltinTest, ArrayPointer) {
  expect_verdict(
      "  var a = .[3]i32{7, 8, 9};\n"
      "  if (td::clux_sum_i32(data(a), len(a)) != 24) { verdict = 1; }\n"
      "  if (data(a).* != 7) { verdict = 2; }\n"
      "  td::clux_fill_i32(data(a), len(a), 5);\n"
      "  if (a[0] != 5 || a[2] != 7) { verdict = 3; }\n");
}

/* ④ str：data(str) → ref *u8（C 的 const char*） */
TEST_F(DataBuiltinTest, CharPointerFromStr) {
  expect_verdict(
      "  var g: str = \"hello\";\n"
      "  if (td::clux_str_first_byte(data(g)) != 104) { verdict = 1; }\n" /* 'h' */
      "  // 空串给的是**有效的** C 空串指针（不是 NULL）：首字节为 0\n"
      "  var e: str = \"\";\n"
      "  if (td::clux_str_first_byte(data(e)) != 0) { verdict = 2; }\n"
      "  if (len(e) != 0usize) { verdict = 3; }\n");
}

/* ⑤ 结果类型就是 ref *T（可赋给声明类型、可解引用、可继续传给 C） */
TEST_F(DataBuiltinTest, ResultTypeIsExactRefPointer) {
  expect_verdict(
      "  var s: own []i32 = make(i32, 2, 6, 7);\n"
      "  var p: ref *i32 = data(s);\n"     /* own []i32 → ref *i32 */
      "  if (p.* != 6) { verdict = 1; }\n"
      "  var b: own []u8 = make(u8, 2, 1u8, 2u8);\n"
      "  var q: ref *u8 = data(b);\n"      /* 元素类型跟着走 */
      "  if (q.* != 1u8) { verdict = 2; }\n");
}

/* ⑥ 诊断：实参不是序列 / 参数个数不对 */
TEST_F(DataBuiltinTest, Diagnostics) {
  const std::string lib = std::string("extern lib \"") + kTestLib + "\" as td;\n";
  expect_compile_error("func main(): i32 { var p: ref *i32 = data(5); return 0; }\n");
  expect_compile_error("extern lib \"*\" as libc;\n"
                       "func main(): i32 { var g: str = \"x\"; "
                       "var q: opaque = data(g, g); return 0; }\n");
  (void)lib;
}

/* ⑦ 空切片：ptr 可为 NULL，len 为 0（C 侧不得解引用） */
TEST_F(DataBuiltinTest, EmptySliceIsNullButLenIsZero) {
  expect_verdict(
      "  var s: own []i32 = make(i32, 0);\n"
      "  if (len(s) != 0usize) { verdict = 1; }\n"
      "  if (td::clux_sum_i32(data(s), len(s)) != 0) { verdict = 2; }\n");
}
