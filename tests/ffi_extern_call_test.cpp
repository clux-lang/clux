/* M6 FFI：BCODE_EXTERN_CALL 的字节码级真调用测试
 *
 * 验证 `op_extern_call` 真的能把符号解析出来并调用：
 *   把宿主 C 字符串指针作为 i64 压栈 → EXTERN_CALL "msvcrt.dll::strlen" argc=1
 *   ret=i64 → 期望返回 5。
 *
 * 测试自建一个模块并把 `msvcrt.dll` 打开后放进 `module_t.ffi_libs`（别名 crt），
 * 再把 `bc->module_path` 指向该模块——这正是处理器查找库表的路径。
 * 直接调 `exec_drive(vm, bc, 0)`：exec_run 只跑"类型提升区 + 函数注册段"，
 * 不执行顶层语句，故这里用驱动循环从 pc=0 执行自包含指令序列。
 */
#include <gtest/gtest.h>

#include "core/allocator.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "vm/bcode.h"
#include "vm/exec.h"
#include "vm/ffi_lib.h"
#include "vm/module.h"
#include "vm/scope.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <cstdint>
#include <cstdlib>

namespace {

void *ffi_calloc(size_t size) { return std::malloc(size); }
void ffi_cfree(void *p) { std::free(p); }

const char *kMod = "ffi_extern_mod";

/* 各平台的系统 C 库 / 数学库（POSIX 上 sqrt 在 libm，不在 libc） */
#if defined(_WIN32)
const char *kSystemLib = "msvcrt.dll";
const char *kMathLib = "msvcrt.dll";
#elif defined(__APPLE__)
const char *kSystemLib = "libSystem.B.dylib";
const char *kMathLib = "libSystem.B.dylib";
#else
const char *kSystemLib = "libc.so.6";
const char *kMathLib = "libm.so.6";
#endif

class FfiExternCallTest : public ::testing::Test {
 protected:
  allocator_t *alloc = nullptr;
  vm_t *vm = nullptr;
  bytecode_t *bc = nullptr;

  void SetUp() override {
    alloc = create_allocator(ffi_calloc, ffi_cfree);
    vm = vm_new(alloc);
    bc = bcode_new(alloc);

    char err[256] = {0};
    ffi_lib_t *lib = ffi_lib_open(alloc, kSystemLib, err, sizeof(err));
    ASSERT_NE(lib, nullptr) << "err=" << err;
    /* 数学符号（sqrt 等）在 glibc 里属于 libm，与 libc 分开；Windows/macOS 由
       CRT/System 提供。这里挂成独立别名，测试用 "math::sqrt"。 */
    ffi_lib_t *math = ffi_lib_open(alloc, kMathLib, err, sizeof(err));
    ASSERT_NE(math, nullptr) << "err=" << err;

    /* 模块只承载库表：bc/scope/exports 传 NULL（module_destroy 均做空检查） */
    module_t *mod = module_new(alloc, nullptr, nullptr, nullptr, kMod);
    ASSERT_NE(mod, nullptr);
    mod->ffi_libs = strmap_new(alloc, /*owns_value=*/false);
    ASSERT_NE(mod->ffi_libs, nullptr);
    strmap_insert(mod->ffi_libs, alloc, "crt", lib);
    strmap_insert(mod->ffi_libs, alloc, "math", math);
    vm_module_bind(vm, kMod, mod);

    /* 处理器据此（bc->module_path → vm->modules）找到本模块的库表 */
    bc->module_path = kMod;
  }

  void TearDown() override {
    bcode_destroy(&bc);
    vm_destroy(&vm); /* 连带销毁模块 → 关闭动态库 */
    delete_allocator(&alloc);
  }

  value_t *run() { return exec_drive(vm, bc, 0); }
};

/* 正例：真调用 C 库函数 strlen("hello") == 5 */
TEST_F(FfiExternCallTest, CallsStrlenThroughExternCall) {
  const char *s = "hello";
  bcode_write_op(bc, BCODE_PUSH_I64);
  bcode_write_i64(bc, (int64_t)(intptr_t)s);
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("crt::strlen"));
  bcode_write_u32(bc, 1); /* argc */
  bcode_write_u32(bc, 2); /* ret_kind = I64 */
  bcode_write_op(bc, BCODE_HALT);

  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_i64);
  EXPECT_EQ(*(const int64_t *)value_data(top), 5);
}

/* 反例 1：库别名未加载 → 明确诊断 */
TEST_F(FfiExternCallTest, UnknownLibraryIsDiagnosed) {
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("nolib::strlen"));
  bcode_write_u32(bc, 0);
  bcode_write_u32(bc, 2);
  bcode_write_op(bc, BCODE_HALT);

  value_t *r = run();
  EXPECT_NE(r, nullptr); /* error value */
}

/* 反例 2：符号不存在 → 明确诊断 */
TEST_F(FfiExternCallTest, UnknownSymbolIsDiagnosed) {
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("crt::clux_no_such_symbol_xyz"));
  bcode_write_u32(bc, 0);
  bcode_write_u32(bc, 2);
  bcode_write_op(bc, BCODE_HALT);

  value_t *r = run();
  EXPECT_NE(r, nullptr);
}

/* 反例 3：畸形 lib::name → 明确诊断 */
TEST_F(FfiExternCallTest, MalformedQualifiedNameIsDiagnosed) {
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("no_separator_here"));
  bcode_write_u32(bc, 0);
  bcode_write_u32(bc, 2);
  bcode_write_op(bc, BCODE_HALT);

  value_t *r = run();
  EXPECT_NE(r, nullptr);
}

}  // namespace

/* 正例（f64）：EXTERN_CALL "crt::sqrt" argc=1 ret=4(F64) → sqrt(9.0) == 3.0 */
TEST_F(FfiExternCallTest, CallsSqrtThroughExternCall) {
  bcode_write_op(bc, BCODE_PUSH_F64);
  bcode_write_f64(bc, 9.0);
  bcode_write_op(bc, BCODE_EXTERN_CALL);
  bcode_write_str(bc, STRSLICE_LIT("math::sqrt"));
  bcode_write_u32(bc, 1); /* argc */
  bcode_write_u32(bc, 4); /* ret_kind = F64 */
  bcode_write_op(bc, BCODE_HALT);

  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_f64);
  EXPECT_DOUBLE_EQ(*(const double *)value_data(top), 3.0);
}