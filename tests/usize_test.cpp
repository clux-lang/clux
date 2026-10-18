/* `usize`：C 的 `size_t` 在 clux 里的名字（**独立类型**，宽度随架构）
 * 见 docs/language-spec-m1.md §2.1、docs/m4-design.md §5/§11.3、docs/m6-design.md §14.20。
 *
 * 设计要点：`usize` **不是** `u64`/`u32` 的别名，而是自己的 `type_t`
 * （id 18，kind INT、无符号、size/align = 指针宽度）。它承担 C `size_t` 的
 * **语义角色**，但在类型系统里是独立身份：
 *   - `typeof(1usize) != typeof(1u64)`、`usize == u64` 为假（类型计算/错误信息分开）；
 *   - 与**同宽**无符号类型隐式互转（64 位 usize ↔ u64，32 位 usize ↔ u32）——
 *     表示完全相同，无信息损失；变宽时按宽度规则（拓宽隐式、缩窄显式）；
 *   - FFI 里 `usize` = C 的 `size_t`（独立描述符码，宽度按运行平台）。
 *
 * 覆盖：
 *   ① VM 层：`type_lookup(vm, "usize")` 是独立类型，与 u64/u32 **不是**同一个
 *      type_t；宽度 = 指针宽度；id 表可查；`len` 返回它；
 *   ② 源码层：len/sizeof/alignof 都是 usize；`5usize` 后缀；字段/参数/返回；
 *      与同宽无符号类型互转；类型身份与 u64 不同；
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

/* ① VM 层：身份 —— `usize` 是**独立**类型，不是 u64/u32 的别名 */
TEST(Usize, IsDistinctPlatformSizeType) {
  allocator_t *alloc = create_allocator(us_alloc, us_free);
  ASSERT_NE(alloc, nullptr);
  vm_t *vm = vm_new(alloc);
  ASSERT_NE(vm, nullptr);

  const type_t *usize = type_lookup(vm, STRSLICE_LIT("usize"));
  ASSERT_NE(usize, nullptr) << "`usize` is not registered as a builtin type";
  EXPECT_EQ(usize, vm_type_usize(vm));
  EXPECT_EQ(usize->kind, TYPE_KIND_INT);
  EXPECT_STREQ(usize->name.ptr, "usize");

  /* 独立身份：与平台宽度对应的 u64/u32 **不是**同一个 type_t */
  if (kIs64) {
    EXPECT_EQ(usize->size, 8u);
    EXPECT_NE(usize, vm->type_u64);
    EXPECT_NE(usize, vm->type_u32);
  } else {
    EXPECT_EQ(usize->size, 4u);
    EXPECT_NE(usize, vm->type_u32);
    EXPECT_NE(usize, vm->type_u64);
  }
  /* size_t 的定义就是"与指针同宽" */
  EXPECT_EQ(usize->size, sizeof(void *));
  EXPECT_EQ(usize->align, alignof(void *));

  /* id 表可查（LOAD_TYPE <id> 走它），且 id 落在内建段 */
  EXPECT_EQ(vm_type_load(vm, (uint32_t)usize->id), usize);
  EXPECT_LT(usize->id, TYPE_ID_BUILTIN_COUNT);

  /* len 的签名返回 usize（不是 u64） */
  value_t *lenv = scope_lookup(vm->global_scope, STRSLICE_LIT("len"));
  ASSERT_NE(lenv, nullptr);
  EXPECT_EQ(func_type_return(value_type(lenv)), usize);

  /* FFI：usize 有自己的类型码，且能从码还原成 usize（宽度 = 指针宽度） */
  uint32_t kind = 0;
  ASSERT_TRUE(ffi_data_kind_of(usize, vm, &kind));
  EXPECT_EQ(kind, BCODE_FFI_DATA_USIZE);
  size_t sz = 0;
  EXPECT_EQ(ffi_data_type_of(vm, kind, &sz), usize);
  EXPECT_EQ(sz, sizeof(void *));
  EXPECT_NE(ffi_data_ffi_type(kind), nullptr);

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

  std::string make_source(const std::string &body) {
    return "struct Buf { n: usize; cap: usize; }\n"
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
  }

  void expect_verdict(const std::string &body) {
    std::string src = make_source(body);
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
      "  if (len(\"hello\") != 5u64) { verdict = 2; }\n" /* 同宽：可隐式互转 */
      "  if (sizeof(usize) != sizeof(str)) { verdict = 3; }\n"
      "  if (sizeof(usize) != sizeof(opaque)) { verdict = 4; }\n"
      "  if (alignof(usize) != alignof(opaque)) { verdict = 5; }\n"
      "  if (sizeof(Buf) != sizeof(usize) * 2usize) { verdict = 6; }\n"
      "  if (sizeof(u8) != 1usize) { verdict = 7; }\n");
}

TEST_F(UsizeSourceTest, SameWidthImplicitConversions) {
  /* usize 是独立类型，但与**同宽**无符号类型表示相同 → 双向隐式；
     变宽仍按宽度规则（拓宽隐式 / 缩窄显式）。 */
  expect_verdict(
      "  var a: usize = 7u64;\n" /* u64 -> usize ✓（同宽） */
      "  var b: u64 = a;\n"      /* usize -> u64 ✓ */
      "  if (b != 7u64) { verdict = 1; }\n"
      "  var c: usize = len(\"abcd\");\n"
      "  var d: u64 = c + 1usize;\n"
      "  if (d != 5u64) { verdict = 2; }\n"
      "  if (5usize != 5u64) { verdict = 3; }\n"
      /* 窄 → 宽：u8 隐式进 usize */
      "  var small: u8 = 7u8;\n"
      "  var widened: usize = small;\n"
      "  if (widened != 7usize) { verdict = 4; }\n"
      /* 宽 → 窄：显式 cast */
      "  var narrow: u8 = 300usize as u8;\n"
      "  if (narrow != 44u8) { verdict = 5; }\n");
}

TEST_F(UsizeSourceTest, TypeIdentityDiffersFromU64) {
  /* 独立类型的关键证据：类型计算里 `usize == u64` 为假。
     用三元类型计算把结论落成一个可断言的字面量宽度。 */
  expect_verdict(
      "  type NotSame = usize == u64 ? i32 : i64;\n"
      "  type Same64 = usize == usize ? i32 : i64;\n"
      "  var probe: NotSame = 1;\n"
      "  var probe2: Same64 = 1;\n"
      "  if (sizeof(probe) != sizeof(i64)) { verdict = 1; }\n"
      "  if (sizeof(probe2) != sizeof(i32)) { verdict = 2; }\n"
      /* 但宽度相同（同一架构上都是指针宽度） */
      "  if (sizeof(usize) != sizeof(u64)) { verdict = 3; }\n");
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
      "  if (n != 5u64) { verdict = 2; }\n"
      /* 返回值的**类型**就是 usize（sizeof 与 usize 一致） */
      "  if (sizeof(n) != sizeof(usize)) { verdict = 3; }\n");
}

TEST_F(UsizeSourceTest, SizeofReturnsUsizeTypeNotU64) {
  /* sizeof/alignof 折叠出的常量是 usize 字面量：写进 usize 变量、参与
     usize 运算都无需 cast；与 u64 也能直接比较（同宽）。数组边界槽位同样
     接受 usize 常量（`[sizeof(usize)]u8`，由 SemaTest.SizeofAsArrayBound 覆盖）。 */
  expect_verdict(
      "  var a: usize = sizeof(i32);\n"
      "  if (a != 4usize) { verdict = 1; }\n"
      "  if (sizeof(a) != sizeof(usize)) { verdict = 2; }\n"
      "  if (sizeof(a) != 8u64) { verdict = 3; }\n");
}

/* ③ 负例：usize 不是 u64，所以与**有符号**类型不能隐式混合 */
TEST(Usize, SignedMixtureRejectedAtCompileTime) {
  std::string src =
      "extern cfunc libc::strlen(s: str): usize;\n"
      "func main(): i32 {\n"
      "  var bad: i64 = libc::strlen(\"x\");\n"
      "  return 0;\n"
      "}\n";
  std::string path = temp_path("clux_usize_bad");
  FILE *fp = fopen(path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  fwrite(src.data(), 1, src.size(), fp);
  fclose(fp);
  std::string err;
  int rc = clux_run_capture(path, &err);
  std::remove(path.c_str());
  EXPECT_EQ(rc, 1) << err;
  EXPECT_NE(err.find("usize"), std::string::npos)
      << "诊断里应出现 usize（独立类型名）：\n" << err;
}

/* ③ 序列化往返：usize 的类型 id（18）与 FFI usize 码必须能落盘/读回——
   `.cxb` 里 LOAD_TYPE 18 与 EXTERN_CALL 的 ret_kind=15 都要还原成 usize，
   否则运行期会退化成 u64 或直接报 unknown type。 */
TEST_F(UsizeSourceTest, BytecodeRoundTripKeepsUsize) {
  std::string src = make_source(
      "  var n: usize = libc::strlen(\"hello\");\n"
      "  if (n != 5usize) { verdict = 1; }\n"
      "  if (sizeof(n) != sizeof(usize)) { verdict = 2; }\n"
      "  td::td_i32 = verdict;\n");
  std::string path = temp_path("clux_usize_rt");
  FILE *fp = fopen(path.c_str(), "wb");
  ASSERT_NE(fp, nullptr);
  fwrite(src.data(), 1, src.size(), fp);
  fclose(fp);

  const std::string bin = (clux_test_path() / "clux_usize_rt.cxb").string();
  ASSERT_EQ(driver_build_bin(path.c_str(), bin.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);

  int32_t *v = verdict_slot();
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(*v, 0) << "clux verdict=" << *v << "\n" << src;

  std::remove(path.c_str());
  std::remove(bin.c_str());
}