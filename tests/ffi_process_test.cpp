/* M6 FFI：特殊外部模块 `*`（当前进程自身）
 *
 * libc 这类库在进程启动时就已链接进来，不需要（也不该）用 `extern lib "<文件>"`
 * 再加载一次。`extern lib "*" as <别名>;` 声明的就是"当前进程"这个伪库：
 * 符号解析走 RTLD_DEFAULT 的等价实现（POSIX 直接 `dlsym(RTLD_DEFAULT, …)`；
 * Windows = 主模块 + 遍历已加载模块，见 `ffi_sym_current_process`）。
 *
 *     extern lib "*" as c;
 *     extern func  c::abs(x: i32): i32;      // 与 `extern cfunc` 等价
 *     extern cfunc c::strlen(s: str): u64;
 *     extern var   c::_fmode: i32;
 *
 * 覆盖：字节码级（伪库句柄 + EXTERN_CALL/EXTERN_LOAD）、源码级（读/写/调用）、
 * 容器往返、以及本轮一并修掉的**返回类型精度**与**实参校验**。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "driver/driver.h"
#include "parser/fmt.h"
#include "vm/bcode.h"
#include "vm/exec.h"
#include "vm/ffi_lib.h"
#include "vm/module.h"
#include "vm/scope.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "test_common.h"
}

namespace {

/* 特殊路径 `"*"` = 当前进程自身（所有平台同一个拼写；POSIX 下就是
   `dlsym(RTLD_DEFAULT, …)`，Windows 下是"主模块 + 已加载模块"遍历） */
const char *kHostLib = "*";

void *pl_alloc(size_t size) { return std::malloc(size); }
void pl_free(void *p) { std::free(p); }


std::string read_file(const std::string &path) {
  std::string out;
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) return out;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, got);
  fclose(fp);
  return out;
}

/* 进程里读 `_fmode`（宿主侧，用于写入后复核与还原） */
int32_t host_fmode() {
  char err[256] = {0};
  int32_t v = 0;
  void *sym = ffi_sym_current_process("_fmode", err, sizeof err);
  if (sym) memcpy(&v, sym, sizeof v);
  return v;
}

std::string ffi_format(const std::string &src) {
  allocator_t *a = create_allocator(pl_alloc, pl_free);
  char *out = fmt_format_source(a, src.c_str(), src.size(), nullptr);
  std::string r = out ? out : "";
  if (out) allocator_free(a, (void **)&out);
  delete_allocator(&a);
  return r;
}

}  // namespace

/* ================================================================
 * ① 字节码级：进程伪库句柄（`ffi_lib_open(alloc, "*")`）
 * ================================================================ */

namespace {

class FfiProcessBcodeTest : public ::testing::Test {
 protected:
  allocator_t *alloc = nullptr;
  vm_t *vm = nullptr;
  bytecode_t *bc = nullptr;
  const char *kMod = "ffi_proc_mod";

  void SetUp() override {
    alloc = create_allocator(pl_alloc, pl_free);
    vm = vm_new(alloc);
    bc = bcode_new(alloc);

    /* `extern lib "*" as c;` 的等价物：打开"进程伪库"并放进库表 */
    char err[256] = {0};
    ffi_lib_t *lib = ffi_lib_open(alloc, kHostLib, err, sizeof err);
    ASSERT_NE(lib, nullptr) << "err=" << err;

    module_t *mod = module_new(alloc, nullptr, nullptr, nullptr, kMod);
    ASSERT_NE(mod, nullptr);
    mod->ffi_libs = strmap_new(alloc, /*owns_value=*/false);
    ASSERT_NE(mod->ffi_libs, nullptr);
    strmap_insert(mod->ffi_libs, alloc, "c", lib);
    vm_module_bind(vm, kMod, mod);
    bc->module_path = kMod;
  }

  void TearDown() override {
    bcode_destroy(&bc);
    vm_destroy(&vm); /* 连带销毁模块 → 关闭（伪）句柄 */
    delete_allocator(&alloc);
  }

  value_t *run() { return exec_drive(vm, bc, 0); }
};

}  // namespace

/* 进程里的 strlen("hello") == 5，且**返回类型是 u64**（声明即类型） */
TEST_F(FfiProcessBcodeTest, CallsProcessStrlenWithU64ReturnType) {
  const char *s = "hello";
  bcode_write_op(bc, BCODE_PUSH_I64);
  bcode_write_i64(bc, (int64_t)(intptr_t)s);
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("c::strlen"));
  bcode_write_u32(bc, 1);
  bcode_write_u32(bc, BCODE_FFI_RET_U64); /* 关键：u64（旧编码这里是 i64） */
  bcode_write_op(bc, BCODE_HALT);

  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_u64);
  EXPECT_EQ(*(const uint64_t *)value_data(top), 5u);
}

/* 进程里的 abs(-7) == 7（i32 返回） */
TEST_F(FfiProcessBcodeTest, CallsProcessAbs) {
  bcode_write_op(bc, BCODE_PUSH_I32);
  bcode_write_i32(bc, -7);
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("c::abs"));
  bcode_write_u32(bc, 1);
  bcode_write_u32(bc, BCODE_FFI_RET_I32);
  bcode_write_op(bc, BCODE_HALT);

  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_i32);
  EXPECT_EQ(*(const int32_t *)value_data(top), 7);
}

/* 进程里的**数据符号**（`extern var c::_fmode`）：走同一条进程解析路径 */
TEST_F(FfiProcessBcodeTest, LoadsDataSymbolFromProcess) {
  const int32_t expected = host_fmode();
  bcode_write_op(bc, BCODE_EXTERN_LOAD);
  bcode_write_str(bc, STRSLICE_LIT("c::_fmode"));
  bcode_write_u32(bc, BCODE_FFI_DATA_I32);
  bcode_write_op(bc, BCODE_HALT);

  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_i32);
  EXPECT_EQ(*(const int32_t *)value_data(top), expected);
}

/* 进程里不存在的符号 → error value（诊断含"当前进程"字样） */
TEST_F(FfiProcessBcodeTest, UnknownProcessSymbolIsDiagnosed) {
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("c::clux_no_such_symbol_xyz"));
  bcode_write_u32(bc, 0);
  bcode_write_u32(bc, BCODE_FFI_RET_I32);
  bcode_write_op(bc, BCODE_HALT);
  EXPECT_NE(run(), nullptr);
}

/* ================================================================
 * ② 源码级：`extern lib "*" as c;` 的完整用法
 * ================================================================ */

namespace {

/* 进程库的端到端程序：函数（i32/u64 返回）、数据符号读写、`extern func` 同义写法 */
std::string process_program() {
  return std::string("extern lib \"") + kHostLib + "\" as c;\n" +
         "extern func  c::abs(x: i32): i32;\n" +
         "extern cfunc c::strlen(s: str): u64;\n" +
         "extern var   c::_fmode: i32;\n" +
         "func main(): i32 {\n" +
         "  if (c::abs(-7) != 7) { return 1; }\n" +
         "  if (c::strlen(\"hello\") != 5u64) { return 2; }\n" +
         "  var s: str = \"abcd\";\n" +
         "  if (c::strlen(s) != 4u64) { return 3; }\n" +
         "  var old: i32 = c::_fmode;\n" +
         "  c::_fmode = 32768;\n" +
         "  if (c::_fmode != 32768) { return 4; }\n" +
         "  c::_fmode = old;\n" +
         "  if (c::_fmode != old) { return 5; }\n" +
         "  return 0;\n" +
         "}\n";
}

}  // namespace

TEST(FfiProcessSource, RunsWithoutAnyExternLibFile) {
  std::string path = write_temp_file(process_program(), "proc_run");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 进程伪库同样随容器落盘：`emit → run .cxb`、`.cxb → .cxs → .cxb` 均可用 */
TEST(FfiProcessSource, SurvivesCxbAndCxsRoundTrip) {
  std::string src = write_temp_file(process_program(), "proc_rt");
  std::string b1 = src + ".1.cxb";
  std::string a2 = src + ".2.cxs";
  std::string b2 = src + ".2.cxb";
  ASSERT_EQ(driver_build_bin(src.c_str(), b1.c_str()), 0);
  EXPECT_EQ(driver_run_bin(b1.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(b1.c_str(), a2.c_str()), 0);

  /* `.cxs` 里必须带上 `externlib "*" as c`（伪库在加载侧被重新识别） */
  FILE *fp = fopen(a2.c_str(), "rb");
  ASSERT_NE(fp, nullptr);
  std::string text;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, got);
  fclose(fp);
  EXPECT_NE(text.find("externlib \"*\" as c"), std::string::npos) << text;

  ASSERT_EQ(driver_asm_to_bin(a2.c_str(), b2.c_str()), 0);
  EXPECT_EQ(driver_run_bin(b2.c_str()), 0);
  EXPECT_EQ(read_file(b1), read_file(b2)); /* 字节一致 */

  std::remove(src.c_str());
  std::remove(b1.c_str());
  std::remove(a2.c_str());
  std::remove(b2.c_str());
}

/* `extern func c::f(...)` 与 `extern cfunc c::f(...)` 产生相同语义；
   fmt 统一渲染成规范写法 `extern cfunc`（语义不变的归一化）。 */
TEST(FfiProcessSource, ExternFuncIsSynonymOfExternCfunc) {
  std::string src = std::string("extern lib \"") + kHostLib + "\" as c;\n" +
                    "extern func c::abs(x: i32): i32;\n"
                    "func main(): i32 { return c::abs(-7) - 7; }\n";
  std::string path = write_temp_file(src, "proc_syn");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());

  std::string formatted = ffi_format(src);
  EXPECT_NE(formatted.find("extern cfunc c::abs(x: i32): i32;"),
            std::string::npos)
      << formatted;
  EXPECT_EQ(formatted.find("extern func c::abs"), std::string::npos)
      << formatted; /* 归一化为规范写法 */
}

/* 用 `*` 声明的**数据符号**写入后，宿主侧（同一进程）能读到 —— 前面用例已覆盖
   读；这里确认写：改 `_fmode` 再还原，两次都从宿主侧复核。 */
TEST(FfiProcessSource, DataSymbolWriteIsVisibleInProcess) {
  const int32_t old = host_fmode();
  std::string src = std::string("extern lib \"") + kHostLib + "\" as c;\n" +
                    "extern var c::_fmode: i32;\n"
                    "func main(): i32 { c::_fmode = 16384; return 0; }\n";
  std::string path = write_temp_file(src, "proc_write");
  ASSERT_EQ(driver_run_file(path.c_str()), 0);
  EXPECT_EQ(host_fmode(), 16384);
  std::remove(path.c_str());

  char restore[512];
  snprintf(restore, sizeof restore,
           "extern lib \"%s\" as c;\n"
           "extern var c::_fmode: i32;\n"
           "func main(): i32 { c::_fmode = %d; return 0; }\n",
           kHostLib, (int)old);
  std::string rpath = write_temp_file(restore, "proc_restore");
  EXPECT_EQ(driver_run_file(rpath.c_str()), 0);
  EXPECT_EQ(host_fmode(), old);
  std::remove(rpath.c_str());
}

/* ================================================================
 * ③ 本轮一并修掉的缺陷：返回类型精度 + 实参校验
 * ================================================================ */

/* u64 返回必须造 u64 值：`strlen(s) != 5u64` 这个比较在旧编码下会报
   `implicit cast: not a widening conversion`（i64 vs u64）。 */
TEST(FfiReturnAndArgs, UnsignedReturnTypeMatchesDeclaration) {
  std::string src = std::string("extern lib \"") + kHostLib + "\" as c;\n" +
                    "extern cfunc c::strlen(s: str): u64;\n"
                    "func main(): i32 {\n"
                    "  var n: u64 = c::strlen(\"hello\");\n"
                    "  if (n != 5u64) { return 1; }\n"
                    "  return 0;\n"
                    "}\n";
  std::string path = write_temp_file(src, "proc_u64");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 实参校验：数量不符 / 浮点喂整数形参 / f32 与 f64 混宽度 / 结构体按值 /
   进程里没有的符号 —— 都要在编译期给出明确诊断（类型不符此前完全不校验，
   会静默按 int64 传参）。 */
TEST(FfiReturnAndArgs, ArgumentAndReturnDiagnostics) {
  const std::string lib = std::string("extern lib \"") + kHostLib + "\" as c;\n";
  std::vector<std::pair<std::string, std::string>> cases = {
      /* 数量不符 */
      {lib + "extern cfunc c::abs(x: i32): i32;\n"
             "func main(): i32 { return c::abs(1, 2); }\n",
       "argc"},
      /* 浮点实参喂整数形参（描述符不同：double 不能当 int 传） */
      {lib + "extern cfunc c::abs(x: i32): i32;\n"
             "func main(): i32 { return c::abs(1.5); }\n",
       "float_arg"},
      /* 浮点**混宽度**：f32 实参喂 f64 形参（描述符不同，要显式 as 转换） */
      {lib + "extern cfunc c::sqrt(x: f64): f64;\n"
             "func main(): i32 { return c::sqrt(1.5f32) as i32; }\n",
       "f32_to_f64"},
      /* 结构体按值传参尚未支持（§2 允许，但搬运层要构造 struct 描述符） */
      {"struct P { x: i32; }\n" + lib +
           "extern cfunc c::foo(p: P): i32;\n"
           "func main(): i32 { return 0; }\n",
       "struct_arg"},
      /* 进程里没有这个符号（编译期按进程解析校验） */
      {lib + "extern cfunc c::clux_no_such_symbol_xyz(): i32;\n"
             "func main(): i32 { return 0; }\n",
       "nosym"},
  };
  for (const auto &c : cases) {
    std::string path = write_temp_file(c.first, c.second);
    EXPECT_NE(driver_run_file(path.c_str()), 0) << "case=" << c.second;
    std::remove(path.c_str());
  }
}
