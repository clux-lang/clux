/* `usize`：C 的 `size_t` 在 clux 里的名字（**随架构变化**）
 * 见 docs/language-spec-m1.md §2.1 与 docs/m4-design.md §5/§11.3。
 *
 * 设计要点：`usize` 不是新类型，而是**别名**（与 C 的 typedef 同义）——
 *   64 位平台 = `u64`，32 位平台 = `u32`（`vm_type_usize`，按 `sizeof(void*)`）。
 * 因此它与底层类型完全同一：可互相赋值/比较，FFI 描述符复用同一张表。
 *
 * 覆盖：
 *   ① VM 层：`type_lookup(vm, "usize")` 就是平台的无符号整数类型，`len` 返回它；
 *   ② 源码层：len/sizeof/alignof 都是 usize；`5usize` 字面量后缀；usize 可用于
 *      变量/结构体字段/参数/返回；与 u64 互换；宽度等于指针宽度；
 *   ③ FFI：`usize` 形参/返回就是 C 的 `size_t`（malloc/memset/free/strlen 实测）。
 *
 * 判定方式同 ffi_struct_test.cpp：clux 程序把检查结果写进库里的 `td_i32`，
 * 宿主侧读回断言（`driver_run_file` 的返回值只表示"编译并运行成功"，
 * **不代表 main 的返回值**）。
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
#include "vm/type_func.h"
#include "vm/value.h"
#include "vm/vm.h"
}

#include "test_common.h"

namespace {

#if defined(_WIN32)
const char *kTestLib = "clux_ffi_testdata.dll";
#else
const char *kTestLib = "libclux_ffi_testdata.so";
#endif

void *us_alloc(size_t size) { return std::malloc(size); }
void us_free(void *p) { std::free(p); }

const bool kIs64 = sizeof(void *) == 8;

std::string temp_path(const std::string &tag) {
  static int counter = 0;
  return (clux_test_path() /
          (tag + "_" + std::to_string(counter++) + ".cx"))
      .string();
}

}  // namespace

/* ① VM 层：别名身份 —— `usize` 与平台无符号整数类型是**同一个 type_t** */
TEST(Usize, AliasesPlatformSizeType) {
  allocator_t *alloc = create_allocator(us_alloc, us_free);
  ASSERT_NE(alloc, nullptr);
  vm_t *vm = vm_new(alloc);
  ASSERT_NE(vm, nullptr);

  const type_t *usize = type_lookup(vm, STRSLICE_LIT("usize"));
  ASSERT_NE(usize, nullptr) << "`usize` is not registered as a builtin type";
  EXPECT_EQ(usize, vm_type_usize(vm));
  if (kIs64) {
    EXPECT_EQ(usize, vm->type_u64);
    EXPECT_EQ(usize->size, 8u);
  } else {
    EXPECT_EQ(usize, vm->type_u32);
    EXPECT_EQ(usize->size, 4u);
  }
  /* size_t 的定义就是"与指针同宽" */
  EXPECT_EQ(usize->size, sizeof(void *));
  EXPECT_EQ(usize->align, alignof(void *));

  /* len 的签名返回 usize */
  value_t *lenv = scope_lookup(vm->global_scope, STRSLICE_LIT("len"));
  ASSERT_NE(lenv, nullptr);
  EXPECT_EQ(func_type_return(value_type(lenv)), usize);

  vm_destroy(&vm);
  delete_allocator(&alloc);
}

/* ②③ 源码层 + FFI：用测试库的 td_i32 收判定值 */
namespace {

class UsizeSourceTest : public ::testing::Test {
 protected:
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  int32_t saved_i32 = 0;

  void SetUp() override {
    alloc = create_allocator(us_alloc, us_free);
    ASSERT_NE(alloc, nullptr);
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
    ASSERT_NE(lib, nullptr) << "cannot open " << kTestLib << ": " << err;
    int32_t *v = verdict_slot();
    ASSERT_NE(v, nullptr);
    saved_i32 = *v;
    *v = -1;
  }

  void TearDown() override {
    if (int32_t *v = verdict_slot()) *v = saved_i32;
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }

  int32_t *verdict_slot() {
    char err[256] = {0};
    return lib ? (int32_t *)ffi_lib_sym(lib, "td_i32", err, sizeof err) : nullptr;
  }

  void expect_verdict(const std::string &body) {
    std::string src =
        "struct Buf { n: usize; cap: usize; }\n"
        "func size_of(x: usize): usize { return x; }\n"
        "extern lib \"*\" as libc;\n"
        "extern lib \"" +
        std::string(kTestLib) +
        "\" as td;\n"
        "extern var td::td_i32: i32;\n"
        "extern cfunc libc::malloc(n: usize): opaque;\n"
        "extern cfunc libc::memset(p: opaque, v: i32, n: usize): opaque;\n"
        "extern cfunc libc::free(p: opaque): void;\n"
        "extern cfunc libc::strlen(s: str): usize;\n"
        "func main(): i32 {\n"
        "  var verdict: i32 = 0;\n" +
        body +
        "  td::td_i32 = verdict;\n"
        "  return verdict;\n"
        "}\n";
    std::string path = temp_path("clux_usize");
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
};

}  // namespace

TEST_F(UsizeSourceTest, LenSizeofAlignofReturnUsize) {
  expect_verdict(
      "  var n: usize = len(\"hello\");\n"
      "  if (n != 5usize) { verdict = 1; }\n"
      "  if (len(\"hello\") != 5u64) { verdict = 2; }\n" /* 别名：与 u64 同型 */
      "  if (sizeof(usize) != sizeof(str)) { verdict = 3; }\n"
      "  if (sizeof(usize) != sizeof(opaque)) { verdict = 4; }\n"
      "  if (alignof(usize) != alignof(opaque)) { verdict = 5; }\n"
      "  if (sizeof(Buf) != sizeof(usize) * 2usize) { verdict = 6; }\n"
      "  if (sizeof(u8) != 1usize) { verdict = 7; }\n");
}

TEST_F(UsizeSourceTest, InterchangeableWithUnderlyingIntegerType) {
  expect_verdict(
      "  var a: usize = 7u64;\n" /* u64 -> usize ✓（同一类型） */
      "  var b: u64 = a;\n"      /* usize -> u64 ✓ */
      "  if (b != 7u64) { verdict = 1; }\n"
      "  var c: usize = len(\"abcd\");\n"
      "  var d: u64 = c + 1usize;\n"
      "  if (d != 5u64) { verdict = 2; }\n"
      "  if (5usize != 5u64) { verdict = 3; }\n");
}

TEST_F(UsizeSourceTest, UsizeInFieldsParamsAndReturns) {
  expect_verdict(
      "  var a: usize = 42usize;\n"
      "  var buf: Buf = .Buf{ .n = a, .cap = size_of(a) };\n"
      "  if (buf.n != 42usize || buf.cap != 42usize) { verdict = 1; }\n"
      "  if (buf.cap != a) { verdict = 2; }\n"
      "  buf.cap = buf.cap + 1usize;\n"
      "  if (buf.cap != 43usize) { verdict = 3; }\n");
}

TEST_F(UsizeSourceTest, UsizeIsCSizeTInFfiSignatures) {
  expect_verdict(
      "  // malloc/memset/free 的 size_t 形参、strlen 的 size_t 返回\n"
      "  var p: opaque = libc::malloc(64usize);\n"
      "  _ = libc::memset(p, 65, 64usize);\n"
      "  libc::free(p);\n"
      "  var n: usize = libc::strlen(\"hello\");\n"
      "  if (n != 5usize) { verdict = 1; }\n"
      "  if (n != 5u64) { verdict = 2; }\n");
}
