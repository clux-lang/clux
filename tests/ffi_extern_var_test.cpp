/* M6 FFI：`extern var` —— 动态库导出的**全局数据符号**
 *
 * `extern lib lib from "...";` + `extern var lib::name: T;` 声明库里的全局变量：
 * 运行期 dlsym/GetProcAddress 得到**变量地址**（数据符号的"符号地址"就是变量
 * 地址），读写都直接作用于库里的那块内存——没有本地副本，因此它是**左值**
 * （`lib::name = v;` 走 EXTERN_STORE，读取走 EXTERN_LOAD）。
 *
 * 两个层次都测：
 *   ① 字节码级（本文件上半）：手写 EXTERN_LOAD / EXTERN_STORE 指令序列，
 *      经 exec_drive 执行——不依赖前端，直接验证处理器与类型码；
 *   ② 源码级（下半）：写 .cx 源码 → driver_run_file / emit → run .cxb，
 *      覆盖 sema 的限定名解析、左值赋值、复合赋值与四类诊断。
 *
 * 被测目标是 tests/ffi_testdata.c 编出的极小动态库（12 个导出变量，覆盖
 * i8/i16/i32/i64、u8/u16/u32/u64、f32/f64、bool、指针——系统库里没有这些
 * 宽度的可写数据符号）。**写操作在用例里自己还原**：同一进程内 DLL 只加载
 * 一次，导出变量是进程级共享状态。
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

#if defined(_WIN32)
const char *kTestDataLib = "clux_ffi_testdata.dll";
#elif defined(__APPLE__)
const char *kTestDataLib = "libclux_ffi_testdata.dylib";
#else
const char *kTestDataLib = "libclux_ffi_testdata.so";
#endif

void *td_alloc(size_t size) { return std::malloc(size); }
void td_free(void *p) { std::free(p); }


/* 保持测试库**常驻**：`ffi_lib_close` 是 FreeLibrary/dlclose，引用计数归零会
   **卸载模块**——下次 LoadLibrary 拿到的是全新副本，导出变量复位。若每次读取都
   开/关句柄，就观察不到"clux 写进去的值"（这是测试夹具的坑，不是实现的坑：
   真实程序里库表由 module_t 持有，直到模块销毁）。持有一个长期句柄即可。 */
struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(td_alloc, td_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestDataLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

LibKeeper &lib_keeper() {
  static LibKeeper k; /* 进程内单例：DLL 只加载一次、保持驻留 */
  return k;
}

/* 在读测试里直接读库里的变量（宿主侧独立于 clux 的 FFI 通路），
   用来确认"clux 写进去的确实是库里那块内存"。 */
template <typename T>
T read_lib_global(const char *name) {
  T out{};
  LibKeeper &k = lib_keeper();
  if (!k.lib) return out;
  char err[256] = {0};
  void *sym = ffi_lib_sym(k.lib, name, err, sizeof err);
  if (sym) memcpy(&out, sym, sizeof(T));
  return out;
}

}  // namespace

/* ================================================================
 * ① 字节码级：EXTERN_LOAD / EXTERN_STORE 处理器
 * ================================================================ */

namespace {

class FfiExternVarBcodeTest : public ::testing::Test {
 protected:
  allocator_t *alloc = nullptr;
  vm_t *vm = nullptr;
  bytecode_t *bc = nullptr;
  const char *kMod = "ffi_var_mod";

  void SetUp() override {
    alloc = create_allocator(td_alloc, td_free);
    vm = vm_new(alloc);
    bc = bcode_new(alloc);

    char err[256] = {0};
    ffi_lib_t *lib = ffi_lib_open(alloc, kTestDataLib, err, sizeof err);
    ASSERT_NE(lib, nullptr) << "err=" << err;

    module_t *mod = module_new(alloc, nullptr, nullptr, nullptr, kMod);
    ASSERT_NE(mod, nullptr);
    mod->ffi_libs = strmap_new(alloc, /*owns_value=*/false);
    ASSERT_NE(mod->ffi_libs, nullptr);
    strmap_insert(mod->ffi_libs, alloc, "td", lib);
    vm_module_bind(vm, kMod, mod);
    bc->module_path = kMod;
  }

  void TearDown() override {
    bcode_destroy(&bc);
    vm_destroy(&vm); /* 连带销毁模块 → 关闭动态库 */
    delete_allocator(&alloc);
  }

  value_t *run() { return exec_drive(vm, bc, 0); }

  /* 重新开始一段指令序列：新 bc **必须**重设 module_path（处理器据此找库表），
     且 vm->halted 要清掉（上一段以 HALT 结束，否则 exec_drive 立刻返回、新的
     指令一条都不执行——表现为"写操作静默没生效"）。 */
  void restart() {
    bcode_destroy(&bc);
    bc = bcode_new(alloc);
    bc->module_path = kMod;
    vm->halted = false;
  }

  /* EXTERN_LOAD <"td::name"> <kind> → HALT */
  void emit_load(const char *qual, uint32_t kind) {
    bcode_write_op(bc, BCODE_EXTERN_LOAD);
    bcode_write_str(bc, strslice_from_cstr(qual));
    bcode_write_u32(bc, kind);
    bcode_write_op(bc, BCODE_HALT);
  }

  /* PUSH_I32 <v> → EXTERN_STORE <"td::name"> <kind> → HALT */
  void emit_store_i32(const char *qual, uint32_t kind, int32_t v) {
    bcode_write_op(bc, BCODE_PUSH_I32);
    bcode_write_i32(bc, v);
    bcode_write_op(bc, BCODE_EXTERN_STORE);
    bcode_write_str(bc, strslice_from_cstr(qual));
    bcode_write_u32(bc, kind);
    bcode_write_op(bc, BCODE_HALT);
  }
};

}  // namespace

/* 读 i32 数据符号：库里 td_i32 = -123456 */
TEST_F(FfiExternVarBcodeTest, LoadReadsInt32DataSymbol) {
  emit_load("td::td_i32", BCODE_FFI_DATA_I32);
  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_i32);
  EXPECT_EQ(*(const int32_t *)value_data(top), -123456);
}

/* 读 u64：库里 td_u64 是一个**超过 int64 正区间**的值——证明按无符号宽度读、
   且没有在 int64 中转时被符号混淆（0xFFFFFFFFFFFFFFFF 附近的值）。 */
TEST_F(FfiExternVarBcodeTest, LoadReadsUnsigned64DataSymbol) {
  emit_load("td::td_u64", BCODE_FFI_DATA_U64);
  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_u64);
  EXPECT_EQ(*(const uint64_t *)value_data(top), 18000000000000000000ull);
}

/* 读 double：库里 td_f64 = 2.5（浮点走的是同一套按宽度 memcpy，不需要 XMM 约定） */
TEST_F(FfiExternVarBcodeTest, LoadReadsFloat64DataSymbol) {
  emit_load("td::td_f64", BCODE_FFI_DATA_F64);
  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_f64);
  EXPECT_DOUBLE_EQ(*(const double *)value_data(top), 2.5);
}

/* 读指针宽度的数据符号 → opaque 值，且内容等于宿主自己 dlsym 到的地址 */
TEST_F(FfiExternVarBcodeTest, LoadReadsPointerAsOpaque) {
  emit_load("td::td_ptr", BCODE_FFI_DATA_PTR);
  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_opaque);

  /* 宿主用同一套 API 解析 td_ptr 与 td_i32：库里 td_ptr 指向 td_i32，
     因此 VM 读到的指针应等于 td_i32 的地址。 */
  char err[256] = {0};
  allocator_t *a2 = create_allocator(td_alloc, td_free);
  ffi_lib_t *lib = ffi_lib_open(a2, kTestDataLib, err, sizeof err);
  ASSERT_NE(lib, nullptr) << err;
  void *addr_of_i32 = ffi_lib_sym(lib, "td_i32", err, sizeof err);
  ASSERT_NE(addr_of_i32, nullptr) << err;
  ffi_lib_close(lib);
  delete_allocator(&a2);

  const void *got = *(const void *const *)value_data(top);
  EXPECT_EQ(got, addr_of_i32);
}

/* 写 + 读回：EXTERN_STORE 真的写进了库里的内存（宿主侧独立复核），随后还原 */
TEST_F(FfiExternVarBcodeTest, StoreWritesDataSymbolInLibraryMemory) {
  const int32_t original = read_lib_global<int32_t>("td_i32");

  emit_store_i32("td::td_i32", BCODE_FFI_DATA_I32, 424242);
  EXPECT_EQ(run(), nullptr);
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), 424242);

  /* 还原（同一进程内 DLL 共享）：再走一遍存指令，写回原值 */
  restart();
  emit_store_i32("td::td_i32", BCODE_FFI_DATA_I32, original);
  EXPECT_EQ(run(), nullptr);
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), original);
}

/* bool 数据符号：读出来的值必须是 **bool 类型**（不是 u8），否则
   `flag != true` 会在类型层报错——这正是独立 BOOL 类型码存在的理由。 */
TEST_F(FfiExternVarBcodeTest, LoadReadsBoolWithBoolType) {
  emit_load("td::td_bool", BCODE_FFI_DATA_BOOL);
  EXPECT_EQ(run(), nullptr);
  value_t *top = exec_stack_peek(vm, 0);
  ASSERT_NE(top, nullptr);
  ASSERT_EQ(value_type(top), vm->type_bool);
  EXPECT_TRUE(*(const bool *)value_data(top));
}

/* 反例：库别名未加载 / 符号不存在 / 畸形限定名 / 类型码非法 */
TEST_F(FfiExternVarBcodeTest, ErrorsAreDiagnosed) {
  const char *bad_quals[] = {"nolib::td_i32", "td::no_such_symbol_xyz",
                             "no_separator"};
  for (const char *q : bad_quals) {
    restart();
    emit_load(q, BCODE_FFI_DATA_I32);
    value_t *r = run();
    EXPECT_NE(r, nullptr) << "qual=" << q; /* error value */
  }
  restart();
  emit_load("td::td_i32", 999u); /* 未知类型码 */
  EXPECT_NE(run(), nullptr);
}

/* ================================================================
 * ② 源码级：`extern var lib::name: T;` 的读写与诊断
 * ================================================================ */

namespace {

/* 覆盖全部可编组宽度的读：每个值都与库里初值比对，返回 0 才算通过。
   返回码即第一个失败的序号（便于定位是哪个宽度错了）。 */
std::string all_widths_read_program() {
  return std::string("extern lib td from \"") + kTestDataLib + "\";\n" +
         "extern var td::td_i8: i8;\n"
         "extern var td::td_i16: i16;\n"
         "extern var td::td_i32: i32;\n"
         "extern var td::td_i64: i64;\n"
         "extern var td::td_u8: u8;\n"
         "extern var td::td_u16: u16;\n"
         "extern var td::td_u32: u32;\n"
         "extern var td::td_u64: u64;\n"
         "extern var td::td_f32: f32;\n"
         "extern var td::td_f64: f64;\n"
         "extern var td::td_bool: bool;\n"
         "func main(): i32 {\n"
         "  if (td::td_i8 != -7i8) { return 1; }\n"
         "  if (td::td_i16 != -3000i16) { return 2; }\n"
         "  if (td::td_i32 != -123456) { return 3; }\n"
         "  if (td::td_i64 != -5000000000i64) { return 4; }\n"
         "  if (td::td_u8 != 200u8) { return 5; }\n"
         "  if (td::td_u16 != 60000u16) { return 6; }\n"
         "  if (td::td_u32 != 4000000000u32) { return 7; }\n"
         "  if (td::td_u64 != 18000000000000000000u64) { return 8; }\n"
         "  if (td::td_f32 != 1.25f32) { return 9; }\n"
         "  if (td::td_f64 != 2.5) { return 10; }\n"
         "  if (td::td_bool != true) { return 11; }\n"
         "  return 0;\n"
         "}\n";
}

}  // namespace

TEST(FfiExternVarSource, ReadsAllScalarWidths) {
  std::string path = write_temp_file(all_widths_read_program(), "var_read");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 写 + 读回（clux 自己读回一份，宿主再读一份 → 双向确认），最后还原初值 */
TEST(FfiExternVarSource, WritesAllScalarWidthsAndRestores) {
  const int32_t i32_orig = read_lib_global<int32_t>("td_i32");
  const double f64_orig = read_lib_global<double>("td_f64");
  const uint8_t u8_orig = read_lib_global<uint8_t>("td_u8");
  const bool bool_orig = read_lib_global<bool>("td_bool");

  std::string src = std::string("extern lib td from \"") + kTestDataLib + "\";\n" +
                    "extern var td::td_i32: i32;\n"
                    "extern var td::td_u8: u8;\n"
                    "extern var td::td_f64: f64;\n"
                    "extern var td::td_bool: bool;\n"
                    "func main(): i32 {\n"
                    "  td::td_i32 = 424242;\n"
                    "  if (td::td_i32 != 424242) { return 1; }\n"
                    "  td::td_u8 = 7u8;\n"
                    "  if (td::td_u8 != 7u8) { return 2; }\n"
                    "  td::td_f64 = 0.5;\n"
                    "  if (td::td_f64 != 0.5) { return 3; }\n"
                    "  td::td_bool = false;\n"
                    "  if (td::td_bool != false) { return 4; }\n"
                    "  return 0;\n"
                    "}\n";
  std::string path = write_temp_file(src, "var_write");
  ASSERT_EQ(driver_run_file(path.c_str()), 0);
  /* 宿主侧独立读库内存：确认写的是**库里的那块**（不是本地副本） */
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), 424242);
  EXPECT_EQ(read_lib_global<uint8_t>("td_u8"), 7);
  EXPECT_DOUBLE_EQ(read_lib_global<double>("td_f64"), 0.5);
  EXPECT_FALSE(read_lib_global<bool>("td_bool"));
  std::remove(path.c_str());

  /* 还原：直接经 clux 的写入路径（顺带再验一次 store） */
  char restore[1024];
  snprintf(restore, sizeof restore,
           "extern lib td from \"%s\";\n"
           "extern var td::td_i32: i32;\n"
           "extern var td::td_u8: u8;\n"
           "extern var td::td_f64: f64;\n"
           "extern var td::td_bool: bool;\n"
           "func main(): i32 {\n"
           "  td::td_i32 = %d;\n"
           "  td::td_u8 = %uu8;\n"
           "  td::td_f64 = %.17g;\n"
           "  td::td_bool = %s;\n"
           "  return 0;\n"
           "}\n",
           kTestDataLib, (int)i32_orig, (unsigned)u8_orig, f64_orig,
           bool_orig ? "true" : "false");
  std::string rpath = write_temp_file(restore, "var_restore");
  EXPECT_EQ(driver_run_file(rpath.c_str()), 0);
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), i32_orig);
  EXPECT_EQ(read_lib_global<uint8_t>("td_u8"), u8_orig);
  EXPECT_DOUBLE_EQ(read_lib_global<double>("td_f64"), f64_orig);
  EXPECT_EQ(read_lib_global<bool>("td_bool"), bool_orig);
  std::remove(rpath.c_str());
}

/* 复合赋值：`+= / -= / *=` 走 EXTERN_LOAD → op → EXTERN_STORE，语义与普通变量一致 */
TEST(FfiExternVarSource, CompoundAssignmentReadsThenStores) {
  const int32_t orig = read_lib_global<int32_t>("td_i32");
  std::string src = std::string("extern lib td from \"") + kTestDataLib + "\";\n" +
                    "extern var td::td_i32: i32;\n"
                    "extern var td::td_f64: f64;\n"
                    "func main(): i32 {\n"
                    "  td::td_i32 = 100;\n"
                    "  td::td_i32 += 5;\n"
                    "  if (td::td_i32 != 105) { return 1; }\n"
                    "  td::td_i32 -= 6;\n"
                    "  if (td::td_i32 != 99) { return 2; }\n"
                    "  td::td_i32 *= 2;\n"
                    "  if (td::td_i32 != 198) { return 3; }\n"
                    "  td::td_f64 = 2.0;\n"
                    "  td::td_f64 += 0.5;\n"
                    "  if (td::td_f64 != 2.5) { return 4; }\n"
                    "  return 0;\n"
                    "}\n";
  std::string path = write_temp_file(src, "var_compound");
  ASSERT_EQ(driver_run_file(path.c_str()), 0);
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), 198);
  std::remove(path.c_str());

  /* 还原（经 clux 写回） */
  char restore[512];
  snprintf(restore, sizeof restore,
           "extern lib td from \"%s\";\n"
           "extern var td::td_i32: i32;\n"
           "extern var td::td_f64: f64;\n"
           "func main(): i32 { td::td_i32 = %d; td::td_f64 = 2.5; return 0; }\n",
           kTestDataLib, (int)orig);
  std::string rpath = write_temp_file(restore, "var_restore2");
  EXPECT_EQ(driver_run_file(rpath.c_str()), 0);
  EXPECT_EQ(read_lib_global<int32_t>("td_i32"), orig);
  EXPECT_DOUBLE_EQ(read_lib_global<double>("td_f64"), 2.5);
  std::remove(rpath.c_str());
}

/* 指针（opaque）：按指针宽度读出再写回同一个变量——值不变，
   证明 EXTERN_LOAD/STORE 的 PTR 宽度路径可用（clux 目前不能解引用 opaque）。 */
TEST(FfiExternVarSource, OpaquePointerRoundTripsThroughLibraryMemory) {
  void *orig = read_lib_global<void *>("td_ptr");
  ASSERT_NE(orig, nullptr);
  std::string src = std::string("extern lib td from \"") + kTestDataLib + "\";\n" +
                    "extern var td::td_ptr: opaque;\n"
                    "func main(): i32 {\n"
                    "  var p: opaque = td::td_ptr;\n"
                    "  td::td_ptr = p;\n"
                    "  return 0;\n"
                    "}\n";
  std::string path = write_temp_file(src, "var_ptr");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  EXPECT_EQ(read_lib_global<void *>("td_ptr"), orig);
  std::remove(path.c_str());
}

/* 容器：extern var 程序经 `.cxb` / `.cxs` 往返仍可运行（v3 容器带库表，
   且 EXTERN_LOAD/STORE 的 STR+U32 操作数在反汇编里稳定） */
TEST(FfiExternVarSource, RunsThroughCxbAndCxsRoundTrip) {
  std::string src = write_temp_file(all_widths_read_program(), "var_rt");
  std::string b1 = src + ".1.cxb";
  std::string a2 = src + ".2.cxs";
  std::string b2 = src + ".2.cxb";
  ASSERT_EQ(driver_build_bin(src.c_str(), b1.c_str()), 0);
  EXPECT_EQ(driver_run_bin(b1.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(b1.c_str(), a2.c_str()), 0);

  /* 反汇编应原样渲染两个操作数（限定名 + 类型码） */
  FILE *fp = fopen(a2.c_str(), "rb");
  ASSERT_NE(fp, nullptr);
  std::string text;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, got);
  fclose(fp);
  EXPECT_NE(text.find("EXTERN_LOAD \"td::td_i64\" 3"), std::string::npos)
      << text;
  EXPECT_NE(text.find("EXTERN_LOAD \"td::td_f64\" 9"), std::string::npos)
      << text;

  ASSERT_EQ(driver_asm_to_bin(a2.c_str(), b2.c_str()), 0);
  EXPECT_EQ(driver_run_bin(b2.c_str()), 0);

  std::remove(src.c_str());
  std::remove(b1.c_str());
  std::remove(a2.c_str());
  std::remove(b2.c_str());
}

/* ---- 诊断：四类错误都必须给出**可执行**的提示，且不静默放行 ---- */
TEST(FfiExternVarSource, Diagnostics) {
  const std::string lib = kTestDataLib;
  const std::string decl_lib =
      "extern lib td from \"" + lib + "\";\n";
  /* （源码, 标签）。源码先存进 std::string 再取 c_str()——不能对临时串取指针。 */
  std::vector<std::pair<std::string, std::string>> cases = {
      /* 未声明的库别名 */
      {"extern var nolib::x: i32;\nfunc main(): i32 { return 0; }\n", "nolib"},
      /* 类型不在可编组子集（option 没有 C 表示；str/struct/enum 现在都支持了） */
      {decl_lib + "extern var td::td_i32: ?i32;\n"
                  "func main(): i32 { return 0; }\n",
       "badtype"},
      /* 库里没有这个数据符号（编译期 dlsym 校验） */
      {decl_lib + "extern var td::no_such_symbol_xyz: i32;\n"
                  "func main(): i32 { return 0; }\n",
       "nosym"},
      /* 引用未声明的符号 */
      {decl_lib + "func main(): i32 { return td::nope; }\n", "undef"},
      /* 给 extern cfunc 赋值 */
      {decl_lib + "extern cfunc td::td_i32(x: i32): i32;\n"
                  "func main(): i32 { td::td_i32 = 1; return 0; }\n",
       "assignfn"},
      /* extern var 不允许初始化器 */
      {decl_lib + "extern var td::td_i32: i32 = 5;\n"
                  "func main(): i32 { return 0; }\n",
       "init"},
  };
  for (const auto &c : cases) {
    std::string path = write_temp_file(c.first, c.second);
    EXPECT_NE(driver_run_file(path.c_str()), 0) << "case=" << c.second;
    std::remove(path.c_str());
  }
}
