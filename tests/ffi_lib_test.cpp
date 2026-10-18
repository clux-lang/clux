/* M6 FFI Phase 1（不依赖 libffi 的部分）：动态库加载与符号解析
 *
 * 判据原则：**按内容判定，绝不按后缀判类**。下面的 FakeSuffixDoesNotImplyLibrary
 * 用例就是这条原则的回归：一个名字以 .dll 结尾的文本文件必须加载失败（由操作
 * 系统报错），证明本层没有"看后缀就当动态库"。
 */
#include <gtest/gtest.h>

#include "core/allocator.h"
#include "vm/ffi_lib.h"
#include "vm/vm.h"
#include "vm/type_slice.h"
#include "vm/type_option.h"
#include "vm/type.h"      /* vm_new + 类型句柄（C ABI 判定测试用） */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/* 测试用最小 allocator（与 allocator_test.cpp 同款风格：显式 alloc/free 钩子） */
void *ffi_test_alloc(size_t size) { return std::malloc(size); }
void ffi_test_free(void *p) { std::free(p); }

class FfiLibTest : public ::testing::Test {
 protected:
  void SetUp() override { alloc_ = create_allocator(ffi_test_alloc, ffi_test_free); }
  void TearDown() override { delete_allocator(&alloc_); }
  allocator_t *alloc_ = nullptr;
};

/* 各平台的系统 C 库（用于"能加载 + 能解析符号"的正例） */
#if defined(_WIN32)
const char *kSystemLib = "msvcrt.dll";
#elif defined(__APPLE__)
const char *kSystemLib = "libSystem.B.dylib";
#else
const char *kSystemLib = "libc.so.6";
#endif

/* 数学库：glibc 的 libm 与 libc 分离（sqrt/pow 不在 libc 里），
   Windows/macOS 则由 CRT/System 提供。 */
#if defined(_WIN32)
const char *kMathLib = "msvcrt.dll";
#elif defined(__APPLE__)
const char *kMathLib = "libSystem.B.dylib";
#else
const char *kMathLib = "libm.so.6";
#endif

std::string write_file(const char *name, const std::string &content) {
  std::string path = std::string("build/tmp/ffi_") + name;
  std::ofstream f(path, std::ios::binary);
  f << content;
  f.close();
  return path;
}

TEST_F(FfiLibTest, OpenSystemLibraryAndResolveSymbol) {
  char err[256] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kSystemLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  EXPECT_STREQ(ffi_lib_path(lib), kSystemLib);

  /* malloc 是 C 库必然导出的符号 */
  void *sym = ffi_lib_sym(lib, "malloc", err, sizeof(err));
  EXPECT_NE(sym, nullptr) << "err=" << err;

  ffi_lib_close(lib);
}

TEST_F(FfiLibTest, MissingLibraryReportsError) {
  char err[256] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, "no_such_library_anywhere_xyz", err,
                                sizeof(err));
  EXPECT_EQ(lib, nullptr);
  EXPECT_NE(std::strstr(err, "no_such_library_anywhere_xyz"), nullptr);
}

TEST_F(FfiLibTest, MissingSymbolReportsError) {
  char err[256] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kSystemLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  void *sym = ffi_lib_sym(lib, "clux_definitely_missing_symbol_xyz", err,
                          sizeof(err));
  EXPECT_EQ(sym, nullptr);
  EXPECT_NE(std::strstr(err, "clux_definitely_missing_symbol_xyz"), nullptr);
  ffi_lib_close(lib);
}

/* **核心原则回归**：后缀不表示文件类型——名字像 .dll 的文本文件必须加载失败。 */
TEST_F(FfiLibTest, FakeSuffixDoesNotImplyLibrary) {
  std::string path = write_file("fake.dll", "this is plain text, not a library");
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, path.c_str(), err, sizeof(err));
  EXPECT_EQ(lib, nullptr); /* 由操作系统按内容拒绝 */
  EXPECT_NE(err[0], '\0');
  std::remove(path.c_str());
}

/* ---- 通用库名（generic name，CMake find_library 风格）----
 *
 * `extern lib td from "clux_ffi_testdata";` 里的名字**不带平台后缀**，加载器按
 * 平台展开候选：Windows `clux_ffi_testdata.dll`；Linux `libclux_ffi_testdata.so`
 * （再退化到 `clux_ffi_testdata.so`）；macOS 还有 `.dylib`。
 *
 * 测试 DLL 由 CMake 放在测试可执行文件旁：Windows 的默认搜索顺序包含 exe 目录，
 * 因此能直接命中；POSIX 上是否命中取决于 rpath/LD_LIBRARY_PATH，命中不了就跳过
 * （不把"环境搜索路径"当成语言语义的一部分来断言）。
 */
TEST_F(FfiLibTest, OpensByGenericNameAndAddsPlatformSuffix) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, "clux_ffi_testdata", err, sizeof(err));
  if (!lib) GTEST_SKIP() << "test library not on the loader search path: " << err;

  /* 句柄记住**声明的通用名**（序列化用它，产物才能跨平台），
     而 path 是实际命中的那个候选（诊断用）。
     注意 Windows 的 LoadLibrary 对无扩展名的名字会自动补 `.dll`，所以
     as-is 候选就可能命中——这里只要求 path 属于该通用名展开出的候选。 */
  EXPECT_STREQ(ffi_lib_name(lib), "clux_ffi_testdata");
  /* path 是**实际命中的候选**：可能是相对候选名（系统搜索路径命中），也可能是
     `<exe 目录>/<候选>`（exe 目录回退命中——Linux 上常见）。两者都接受。 */
  const std::string p = ffi_lib_path(lib);
  auto ends_with = [](const std::string &s, const char *suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
  };
  EXPECT_TRUE(ends_with(p, "clux_ffi_testdata") ||
              ends_with(p, "clux_ffi_testdata.dll") ||
              ends_with(p, "libclux_ffi_testdata.so") ||
              ends_with(p, "clux_ffi_testdata.so") ||
              ends_with(p, "libclux_ffi_testdata.dylib") ||
              ends_with(p, "clux_ffi_testdata.dylib"))
      << p;

  /* 名字对了，符号也要能解析（td_i32 是测试库的导出变量） */
  void *sym = ffi_lib_sym(lib, "td_i32", err, sizeof(err));
  EXPECT_NE(sym, nullptr) << err;
  ffi_lib_close(lib);
}

/* 显式文件名/路径仍然可用：第一个候选恒为 as-is（旧产物与绝对路径兼容） */
TEST_F(FfiLibTest, ExplicitFileNameStillLoadsAsIs) {
  char err[512] = {0};
#if defined(_WIN32)
  const char *explicit_name = "clux_ffi_testdata.dll";
#else
  const char *explicit_name = "libclux_ffi_testdata.so";
#endif
  ffi_lib_t *lib = ffi_lib_open(alloc_, explicit_name, err, sizeof(err));
  if (!lib) GTEST_SKIP() << "test library not on the loader search path: " << err;
  EXPECT_STREQ(ffi_lib_name(lib), explicit_name);
  /* 命中可能是 `<exe 目录>/<显式文件名>`（exe 目录回退），只要求以文件名结尾 */
  const std::string p = ffi_lib_path(lib);
  EXPECT_TRUE(p == explicit_name ||
              (p.size() > std::strlen(explicit_name) &&
               p.compare(p.size() - std::strlen(explicit_name),
                         std::strlen(explicit_name), explicit_name) == 0))
      << p;
  ffi_lib_close(lib);
}

/* 失败诊断必须列出**试过的候选名**（否则"名字拼错/不在搜索路径"极难定位） */
TEST_F(FfiLibTest, MissingLibraryErrorListsTriedCandidates) {
  char err[1024] = {0};
  EXPECT_EQ(ffi_lib_open(alloc_, "definitely_no_such_library_xyz", err,
                         sizeof(err)),
            nullptr);
  const std::string msg(err);
  EXPECT_NE(msg.find("tried:"), std::string::npos) << msg;
  EXPECT_NE(msg.find("definitely_no_such_library_xyz.dll"), std::string::npos)
      << msg;
}

/* `"*"` = 当前进程自身：伪库的名字与路径都是 `*` */
/* 候选表是**跨平台并集**：不按编译期 OS 分支，顺序固定且可断言。
 * 这是"同一份源码 / 同一份 .cxb 在 Windows/Linux/macOS 都能跑"的契约实体。 */
TEST_F(FfiLibTest, CandidatesArePlatformAgnosticUnion) {
  char out[FFI_LIB_CANDIDATE_MAX][FFI_LIB_CANDIDATE_LEN];
  const size_t n = ffi_library_candidates("c", out, FFI_LIB_CANDIDATE_MAX);
  ASSERT_EQ(n, 6u);
  EXPECT_STREQ(out[0], "c");          /* as-is：显式路径 / 带后缀 / 裸名字 */
  EXPECT_STREQ(out[1], "c.dll");      /* Windows */
  EXPECT_STREQ(out[2], "libc.so");    /* ELF（GNU 前缀） */
  EXPECT_STREQ(out[3], "c.so");       /* ELF（无前缀） */
  EXPECT_STREQ(out[4], "libc.dylib"); /* Mach-O（GNU 前缀） */
  EXPECT_STREQ(out[5], "c.dylib");    /* Mach-O（无前缀） */

  /* 显式路径仍然排第一（as-is），后续候选不会覆盖它 */
  const size_t m =
      ffi_library_candidates("/usr/lib/libfoo.so", out, FFI_LIB_CANDIDATE_MAX);
  ASSERT_EQ(m, 6u);
  EXPECT_STREQ(out[0], "/usr/lib/libfoo.so");

  /* cap 不是硬编码 6：按需截断 */
  EXPECT_EQ(ffi_library_candidates("c", out, 2), 2u);
  EXPECT_STREQ(out[1], "c.dll");
}

#if defined(_WIN32)
/* **不直接区分操作系统**（用户要求）：Windows 的 LoadLibraryA 对带扩展名的名字
 * 按内容装载——`.so` 命名的 PE 同样能加载。这里把测试 DLL 复制成
 * `lib<名字>.so`，再用**通用名**打开：命中的必然是候选里的 `lib<名字>.so`，
 * 从而证明 Windows 上 .so 也在候选表里、且真的可用（MinGW/MSYS 产物即此形态）。 */
TEST_F(FfiLibTest, WindowsLoadsSoNamedPeThroughCandidateUnion) {
  char exe_path[MAX_PATH] = {0};
  const DWORD len = GetModuleFileNameA(nullptr, exe_path, (DWORD)sizeof exe_path);
  ASSERT_GT(len, 0u) << "GetModuleFileNameA failed: " << GetLastError();
  std::string dir(exe_path);
  const size_t slash = dir.find_last_of("\\/");
  ASSERT_NE(slash, std::string::npos);
  dir = dir.substr(0, slash + 1);

  const std::string src = dir + "clux_ffi_testdata.dll";
  const std::string dst = dir + "libclux_ffi_soalias.so";
  std::error_code ec;
  std::filesystem::copy_file(src, dst,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) GTEST_SKIP() << "cannot stage .so copy: " << ec.message();

  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, "clux_ffi_soalias", err, sizeof(err));
  EXPECT_NE(lib, nullptr) << "err=" << err;
  if (lib) {
    EXPECT_STREQ(ffi_lib_name(lib), "clux_ffi_soalias");
    /* 命中的是候选里的 lib<name>.so（as-is / <name>.dll 都不存在） */
    EXPECT_STREQ(ffi_lib_path(lib), "libclux_ffi_soalias.so");
    void *sym = ffi_lib_sym(lib, "td_i32", err, sizeof(err));
    EXPECT_NE(sym, nullptr) << err;
    ffi_lib_close(lib);
  }
  std::filesystem::remove(dst, ec);
}
#endif

TEST_F(FfiLibTest, ProcessPseudoLibKeepsStarName) {
  char err[128] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, "*", err, sizeof(err));
  ASSERT_NE(lib, nullptr) << err;
  EXPECT_STREQ(ffi_lib_name(lib), "*");
  EXPECT_STREQ(ffi_lib_path(lib), "*");
  ffi_lib_close(lib);
}
TEST_F(FfiLibTest, EmptyPathRejected) {
  char err[128] = {0};
  EXPECT_EQ(ffi_lib_open(alloc_, "", err, sizeof(err)), nullptr);
  EXPECT_NE(err[0], '\0');
}

TEST_F(FfiLibTest, CloseNullIsSafe) { ffi_lib_close(nullptr); }

}  // namespace

/* ---- 最小 ABI 层（v0）：真正调进 C 库 ----
 *
 * 这些用例不是"能解析符号"而是"能调用"：malloc / strlen / free 都是整数·指针
 * ABI，v0 支持。浮点与结构体传参由类型层拒绝（待 libffi）。
 */
TEST_F(FfiLibTest, CallMallocStrlenAndFree) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kSystemLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;

  void *p_malloc = ffi_lib_sym(lib, "malloc", err, sizeof(err));
  void *p_strlen = ffi_lib_sym(lib, "strlen", err, sizeof(err));
  void *p_free = ffi_lib_sym(lib, "free", err, sizeof(err));
  ASSERT_NE(p_malloc, nullptr) << "err=" << err;
  ASSERT_NE(p_strlen, nullptr) << "err=" << err;
  ASSERT_NE(p_free, nullptr) << "err=" << err;

  /* malloc(16) → 非空指针 */
  int64_t args1[1] = {16};
  int64_t buf = 0;
  ASSERT_TRUE(ffi_call_int(p_malloc, 1, args1, &buf, err, sizeof(err))) << err;
  ASSERT_NE(buf, 0);

  /* 写入后 strlen 应读到长度（用 memcpy 而非 strcpy，避免依赖 C 库其他符号） */
  const char *msg = "hello";
  std::memcpy(reinterpret_cast<void *>(static_cast<intptr_t>(buf)), msg, 6);
  int64_t args2[1] = {buf};
  int64_t len = 0;
  ASSERT_TRUE(ffi_call_int(p_strlen, 1, args2, &len, err, sizeof(err))) << err;
  EXPECT_EQ(len, 5);

  /* free(ptr) → void 返回（返回寄存器值忽略） */
  int64_t args3[1] = {buf};
  int64_t ignored = 0;
  EXPECT_TRUE(ffi_call_int(p_free, 1, args3, &ignored, err, sizeof(err))) << err;

  ffi_lib_close(lib);
}

TEST_F(FfiLibTest, CallRejectsNullSymbolAndTooManyArgs) {
  char err[256] = {0};
  int64_t out = 0;
  EXPECT_FALSE(ffi_call_int(nullptr, 0, nullptr, &out, err, sizeof(err)));
  EXPECT_NE(err[0], '\0');

  /* FFI_MAX_ARGS 现在是 16（变参 printf 等需要更多实参）：用上限 +1 触发拒绝 */
  int64_t many[17] = {0};
  void *sym = reinterpret_cast<void *>(static_cast<intptr_t>(1)); /* 不会被调用 */
  EXPECT_FALSE(ffi_call_int(sym, 17, many, &out, err, sizeof(err)));
  EXPECT_NE(std::strstr(err, "exceed"), nullptr);
}

/* ---- libffi 通用签名调用：浮点（最小 ABI 层做不到的部分）----
 *
 * sqrt(9.0) 走 ffi_type_double 描述符：x64 下 double 实参/返回值走 XMM 寄存器，
 * "函数指针按 int64 强转"无法表达，这正是引入 libffi 的意义。
 */
#if defined(CLUX_HAVE_LIBFFI) && defined(CLUX_FFI_TYPED_CALLS)
#include <ffi.h>

TEST_F(FfiLibTest, LibffiCallsFloatFunctionSqrt) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kMathLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  void *p_sqrt = ffi_lib_sym(lib, "sqrt", err, sizeof(err));
  ASSERT_NE(p_sqrt, nullptr) << "err=" << err;

  double in = 9.0;
  double out = 0.0;
  const struct _ffi_type *argt[1] = {&ffi_type_double};
  void *argv[1] = {&in};
  ASSERT_TRUE(ffi_call_typed(p_sqrt, 1, argt, &ffi_type_double, argv, &out, err,
                             sizeof(err))) << err;
  EXPECT_DOUBLE_EQ(out, 3.0);
  ffi_lib_close(lib);
}

TEST_F(FfiLibTest, LibffiCallsTwoDoubleFunctionPow) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kMathLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  void *p_pow = ffi_lib_sym(lib, "pow", err, sizeof(err));
  ASSERT_NE(p_pow, nullptr) << "err=" << err;

  double a = 2.0, b = 10.0, out = 0.0;
  const struct _ffi_type *argt[2] = {&ffi_type_double, &ffi_type_double};
  void *argv[2] = {&a, &b};
  ASSERT_TRUE(ffi_call_typed(p_pow, 2, argt, &ffi_type_double, argv, &out, err,
                             sizeof(err))) << err;
  EXPECT_DOUBLE_EQ(out, 1024.0);
  ffi_lib_close(lib);
}
#endif /* CLUX_HAVE_LIBFFI && CLUX_FFI_TYPED_CALLS */

/* ---- 浮点调用：sqrt / pow（经 ffi_call_f64，不需要 libffi）---- */
TEST_F(FfiLibTest, CallFloatFunctionSqrt) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kMathLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  void *p_sqrt = ffi_lib_sym(lib, "sqrt", err, sizeof(err));
  ASSERT_NE(p_sqrt, nullptr) << "err=" << err;
  double args[1] = {9.0}, out = 0.0;
  ASSERT_TRUE(ffi_call_f64(p_sqrt, 1, args, &out, err, sizeof(err))) << err;
  EXPECT_DOUBLE_EQ(out, 3.0);
  ffi_lib_close(lib);
}

TEST_F(FfiLibTest, CallTwoDoubleFunctionPow) {
  char err[512] = {0};
  ffi_lib_t *lib = ffi_lib_open(alloc_, kMathLib, err, sizeof(err));
  ASSERT_NE(lib, nullptr) << "err=" << err;
  void *p_pow = ffi_lib_sym(lib, "pow", err, sizeof(err));
  ASSERT_NE(p_pow, nullptr) << "err=" << err;
  double args[2] = {2.0, 10.0}, out = 0.0;
  ASSERT_TRUE(ffi_call_f64(p_pow, 2, args, &out, err, sizeof(err))) << err;
  EXPECT_DOUBLE_EQ(out, 1024.0);
  ffi_lib_close(lib);
}

/* ---- §2 C ABI 类型约束判定 ---- */
TEST(FfiCApiTest, AllowsScalarsPointersAndAggregates) {
  allocator_t *a = create_allocator(ffi_test_alloc, ffi_test_free);
  vm_t *vm = vm_new(a);
  ASSERT_NE(vm, nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_i32), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_i64), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_u32), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_f32), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_f64), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_bool), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_str), nullptr);
  EXPECT_EQ(ffi_c_abi_reject_reason(vm->type_opaque), nullptr);
  vm_destroy(&vm);
  delete_allocator(&a);
}

TEST(FfiCApiTest, RejectsCluxSpecificTypes) {
  allocator_t *a = create_allocator(ffi_test_alloc, ffi_test_free);
  vm_t *vm = vm_new(a);
  ASSERT_NE(vm, nullptr);
  /* option 与切片是 clux 专有，无 C 表示 → 必须拒绝并给出原因 */
  const type_t *opt = type_option_intern(vm, vm->type_i32);
  const type_t *slice = type_slice_intern(vm, vm->type_i32);
  if (opt) {
    const char *why = ffi_c_abi_reject_reason(opt);
    EXPECT_NE(why, nullptr);
  }
  if (slice) {
    const char *why = ffi_c_abi_reject_reason(slice);
    EXPECT_NE(why, nullptr);
  }
  EXPECT_NE(ffi_c_abi_reject_reason(nullptr), nullptr);
  vm_destroy(&vm);
  delete_allocator(&a);
}

/* ---- Phase 5：闭包生成可被 C 调用的函数指针 ---- */
#include <ffi.h>
namespace {
void clux_add_cb(void *ret, void **args, void *user) {
  (void)user;
  *(int *)ret = *(int *)args[0] + *(int *)args[1];
}
}  // namespace

TEST_F(FfiLibTest, ClosureProducesCallableFunctionPointer) {
  void *at[2] = {&ffi_type_sint, &ffi_type_sint};
  void *handle = nullptr, *code = nullptr;
  char err[512] = {0};
  bool ok = clux_ffi_closure_create(alloc_, 2, at, &ffi_type_sint, clux_add_cb,
                                    nullptr, &handle, &code, err, sizeof err);
  fprintf(stderr, "[diag] create=%d err=%s handle=%p code=%p\n", (int)ok, err,
          handle, code);
  fflush(stderr);
  ASSERT_TRUE(ok) << "err=" << err;
  int (*fn)(int, int) = (int (*)(int, int))code;
  int r = fn(20, 22);
  fprintf(stderr, "[diag] fn(20,22)=%d\n", r);
  fflush(stderr);
  EXPECT_EQ(r, 42);
  clux_ffi_closure_destroy(alloc_, handle);
}