/* M6 FFI Phase 5 端到端：**clux 源码里定义的函数**被 **C 侧**当回调使用
 *
 * 前置阶段只证明了两个方向各自成立：
 *   - Phase 3（外→内）：clux 源码 `crt::abs(-7)` 能调到 C 库函数；
 *   - Phase 4/5 基础设施：`clux_ffi_closure_create` 能把一个 **C handler**
 *     包成可调用的函数指针（FfiLibTest.ClosureProducesCallableFunctionPointer）。
 *
 * 本文件补齐最后一环：把 closure 的 handler 接到 **VM 里的 clux 函数值**上，
 * 让 C 代码通过一个普通 C 函数指针调用 clux 函数体：
 *
 *     clux 源码 ──compile_module──> module_t.global_scope 里的 func value
 *            └─ clux_ffi_closure_create(handler=VM 转发) ─> C 函数指针
 *                                                          ↑
 *                        C 侧（本 TU 的 C 函数 / libc qsort）直接调用
 *
 * 三个用例：
 *   1. C 函数按 C ABI 调用 clux `add`（整数参数/返回值编组）；
 *   2. **libc `qsort`** 用 clux 定义的比较函数排序（真实第三方 C 消费者）；
 *   3. clux 回调内部**再回调进 C**（`crt::abs`）——Phase 3 与 Phase 5 复合。
 *
 * 测试自己组装"编译并持有 VM"的流水线：`driver_run_file` 跑完即销毁 VM，
 * `driver_compile_to_bytecode` 是 driver 内的 static，而 `compile_module`
 * 是公开入口（include/driver/compile_module.h），因此这里按 driver.c 的同一
 * 顺序手工组装：create_allocator → arena → diag → vm → module_path_normalize
 * → compile_module。模块注册进 vm->modules，函数值归模块 global_scope。
 */
#include <gtest/gtest.h>

#include <ffi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "driver/compile_module.h"
#include "module/module_path.h"
#include "vm/exec.h"
#include "vm/ffi_lib.h"
#include "vm/module.h"
#include "vm/scope.h"
#include "vm/value.h"
#include "vm/vm.h"
}

namespace {

void *cb_alloc(size_t size) { return std::malloc(size); }
void cb_free(void *p) { std::free(p); }

/* ---- 编译 clux 源码并**保留 VM**（全部公开 API）---- */
struct CompiledClux {
  allocator_t *alloc = nullptr;
  arena_t *arena = nullptr;
  diag_buf_t *diag = nullptr;
  vm_t *vm = nullptr;
  module_t *mod = nullptr;
  std::string path;
  bool owns_file = false;

  ~CompiledClux() { dispose(); }

  bool compile(const std::string &src, const char *tag) {
    static int counter = 0;
    std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("clux_ffi_cb_" + std::string(tag) + "_" + std::to_string(counter++) +
         ".cx");
    path = p.string();
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    fwrite(src.data(), 1, src.size(), fp);
    fclose(fp);
    owns_file = true;

    alloc = create_allocator(cb_alloc, cb_free);
    if (!alloc) return false;
    arena = arena_new_default(alloc);
    diag = diag_buf_new(alloc);
    vm = vm_new(alloc);
    if (!arena || !diag || !vm) return false;

    vec_t *path_stack = vec_new(alloc, /*owns_element=*/false);
    char *entry = module_path_normalize(alloc, path.c_str());
    if (!path_stack || !entry) return false;
    /* 与 driver 的 run/build 同一核心：lex→parse→sema→compile→注册段执行，
       产出 module_t（global_scope 里是函数值）并注册进 vm->modules。 */
    mod = compile_module(vm, alloc, diag, arena, entry, path_stack);
    allocator_free(alloc, (void **)&entry);
    vec_free(alloc, &path_stack);
    if (!mod) diag_print_all(diag);
    return mod != nullptr;
  }

  /* 模块全局作用域里按名取函数值（借用：归模块所有） */
  value_t *lookup(const char *name) {
    return mod ? scope_lookup(mod->global_scope, strslice_from_cstr(name))
               : nullptr;
  }

  void dispose() {
    if (owns_file && !path.empty()) std::remove(path.c_str());
    owns_file = false;
    if (diag) diag_buf_destroy(&diag);
    if (vm) vm_destroy(&vm); /* 连带销毁模块（关闭 extern lib 句柄） */
    if (arena && alloc) arena_destroy(alloc, &arena);
    if (alloc) delete_allocator(&alloc);
  }
};

/* ---- 值编组：未 track 的 i32 值（调用方显式释放，同 vm_test 的 raw 系列）---- */
value_t *make_i32_untracked(vm_t *vm, int32_t v) {
  void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
  return value_alloc_shell(vm->alloc, vm->type_i32, data);
}

void free_untracked(vm_t *vm, value_t *v) {
  if (!v) return;
  value_dispose(vm, v);
  allocator_free(vm->alloc, (void **)&v);
}

int64_t read_sint(const value_t *v) {
  switch (value_type(v)->size) {
    case 1: return (int64_t)*(const int8_t *)value_data(v);
    case 2: return (int64_t)*(const int16_t *)value_data(v);
    case 4: return (int64_t)*(const int32_t *)value_data(v);
    default: return *(const int64_t *)value_data(v);
  }
}

/* ---- 回调上下文：VM + 目标 clux 函数值 ---- */
struct CallbackCtx {
  vm_t *vm = nullptr;
  value_t *fn = nullptr; /* 借用：归模块 global_scope */
  bool saw_error = false;
  int calls = 0;
};

/* 在 VM 里调用 clux 函数 f(i32, i32) -> i32，返回结果（失败置 saw_error）。 */
bool call_clux_binary_i32(CallbackCtx *ctx, int32_t x, int32_t y,
                          int32_t *out) {
  value_t *a0 = make_i32_untracked(ctx->vm, x);
  value_t *a1 = make_i32_untracked(ctx->vm, y);
  value_t *args[2] = {a0, a1};
  value_t *r = value_call(ctx->vm, ctx->fn, args, 2);
  bool ok = r && !value_is_error(ctx->vm, r) && value_type(r) == ctx->vm->type_i32;
  if (ok) {
    *out = (int32_t)read_sint(r);
  } else {
    ctx->saw_error = true;
  }
  /* 实参是本测试的未 track 值，调用返回后即可释放（value_call 已 clone 到
     被调函数作用域，该作用域在返回前已销毁）。返回值由调用方作用域 track，
     随 vm_destroy 释放，不在此处置。 */
  free_untracked(ctx->vm, a0);
  free_untracked(ctx->vm, a1);
  ctx->calls++;
  return ok;
}

/* libffi handler：**C 调用约定**收 int32 参数 → 转成 clux 值 → 调 VM 函数。
   `args[i]` 按 libffi 约定指向该实参的值（int32 参数 → 指向 int32）。 */
void clux_i32_i32_handler(void *ret, void **args, void *user) {
  CallbackCtx *ctx = (CallbackCtx *)user;
  int32_t out = 0;
  if (!call_clux_binary_i32(ctx, *(const int32_t *)args[0],
                            *(const int32_t *)args[1], &out)) {
    out = INT32_MIN; /* 失败哨兵：断言会先看到 saw_error */
  }
  *(int32_t *)ret = out;
}

/* libffi handler（指针版）：qsort 传的是 `const void *` 元素地址，按 int 解引用
   后交给同一个 clux 函数。C ABI 层面签名是 (ptr, ptr) -> i32。 */
void clux_cmp_ptr_handler(void *ret, void **args, void *user) {
  CallbackCtx *ctx = (CallbackCtx *)user;
  int32_t x = *(const int32_t *)(*(void **)args[0]);
  int32_t y = *(const int32_t *)(*(void **)args[1]);
  int32_t out = 0;
  if (!call_clux_binary_i32(ctx, x, y, &out)) out = INT32_MIN;
  *(int32_t *)ret = out;
}

/* 纯 C 侧消费者：按 C ABI 调用函数指针（编译期生成的调用序列，与第三方
   C 代码一致——不是测试里手工强转调用）。 */
int32_t c_apply_binary(int32_t (*fn)(int32_t, int32_t), int32_t a, int32_t b) {
  return fn(a, b);
}

const char *kSystemLib =
#if defined(_WIN32)
    "msvcrt.dll";
#elif defined(__APPLE__)
    "libSystem.B.dylib";
#else
    "libc.so.6";
#endif

}  // namespace

class FfiCallbackE2E : public ::testing::Test {
 protected:
  CompiledClux unit;
  CallbackCtx ctx;
  void *handle = nullptr;
  void *code = nullptr;
  char cerr[512] = {0};

  void TearDown() override {
    if (handle) clux_ffi_closure_destroy(unit.alloc, handle);
    handle = nullptr;
    code = nullptr;
  }

  /* 把模块里的 clux 函数 f(i32,i32)->i32 包成 C 函数指针 */
  bool wrap_binary(const char *fn_name, ffi_closure_handler_t handler,
                   void **arg_types) {
    value_t *fn = unit.lookup(fn_name);
    EXPECT_NE(fn, nullptr) << "clux 函数 '" << fn_name << "' 未注册";
    if (!fn) return false;
    ctx.vm = unit.vm;
    ctx.fn = fn;
    return clux_ffi_closure_create(unit.alloc, 2, arg_types, &ffi_type_sint,
                                   handler, &ctx, &handle, &code, cerr,
                                   sizeof cerr);
  }
};

/* 用例 1：C 函数按 C ABI 调用 clux `add` */
TEST_F(FfiCallbackE2E, CFunctionCallsCluxFunctionThroughClosure) {
  ASSERT_TRUE(unit.compile("func add(a: i32, b: i32): i32 { return a + b; }\n",
                          "add"));
  void *at[2] = {&ffi_type_sint, &ffi_type_sint};
  ASSERT_TRUE(wrap_binary("add", clux_i32_i32_handler, at)) << "err=" << cerr;

  int32_t r = c_apply_binary((int32_t (*)(int32_t, int32_t))code, 20, 22);
  EXPECT_EQ(r, 42);
  EXPECT_FALSE(ctx.saw_error);
  EXPECT_EQ(ctx.calls, 1);

  /* 多次调用：VM 状态可重入（每次调用走完整 func_vcall 现场保存/恢复） */
  EXPECT_EQ(c_apply_binary((int32_t (*)(int32_t, int32_t))code, -5, 7), 2);
  EXPECT_FALSE(ctx.saw_error);
  EXPECT_EQ(ctx.calls, 2);
}

/* 用例 2：**libc qsort** 用 clux 定义的比较函数排序（真实第三方 C 消费者） */
TEST_F(FfiCallbackE2E, CluxComparatorDrivesCQsort) {
  ASSERT_TRUE(unit.compile("func cmp(a: i32, b: i32): i32 { return a - b; }\n",
                          "cmp"));
  /* qsort 的比较函数签名是 (const void *, const void *) -> int */
  void *at[2] = {&ffi_type_pointer, &ffi_type_pointer};
  ASSERT_TRUE(wrap_binary("cmp", clux_cmp_ptr_handler, at)) << "err=" << cerr;

  int data[7] = {5, 3, 9, 1, 7, 2, 8};
  auto cmp = (int (*)(const void *, const void *))code;
  std::qsort(data, 7, sizeof(int), cmp);

  const int want[7] = {1, 2, 3, 5, 7, 8, 9};
  for (int i = 0; i < 7; i++) EXPECT_EQ(data[i], want[i]) << "index " << i;
  EXPECT_FALSE(ctx.saw_error);
  /* 7 个元素：qsort 至少调用 n-1 次比较（libc 实现可能更多，故用 >=） */
  EXPECT_GE(ctx.calls, 6);
}

/* 用例 3：clux 回调内部**再回调进 C**（`crt::abs`）——Phase 3 × Phase 5 */
TEST_F(FfiCallbackE2E, CluxCallbackItselfCallsBackIntoC) {
  std::string src = std::string("extern lib \"") + kSystemLib +
                    "\" as crt;\n"
                    "extern cfunc crt::abs(x: i32): i32;\n"
                    "func abs_add(a: i32, b: i32): i32 {\n"
                    "  return crt::abs(a) + crt::abs(b);\n"
                    "}\n";
  ASSERT_TRUE(unit.compile(src, "absadd"));
  void *at[2] = {&ffi_type_sint, &ffi_type_sint};
  ASSERT_TRUE(wrap_binary("abs_add", clux_i32_i32_handler, at)) << "err=" << cerr;

  EXPECT_EQ(c_apply_binary((int32_t (*)(int32_t, int32_t))code, -20, -22), 42);
  EXPECT_FALSE(ctx.saw_error);
}

/* ================================================================
 * 语言级（M6 Phase 5 收口）：**C 函数指针由 clux 源码产出**
 *
 * 上面的用例由宿主（本 TU）调用 `clux_ffi_closure_create` 建闭包；这里改成
 * 内建 `cfunc_ptr(f)`：源码里写 `cfunc cmp(...) { ... }` 再 `cfunc_ptr(cmp)`
 * 就得到可交给 C 的函数指针——宿主只负责**调用**它，不再参与闭包搭建。
 * ================================================================ */

/* 从 clux 函数里取出 opaque 值（即 C 函数指针） */
static void *call_for_code_ptr(CompiledClux &unit, const char *fn_name,
                               bool *is_error) {
  *is_error = false;
  value_t *fn = unit.lookup(fn_name);
  EXPECT_NE(fn, nullptr) << "clux 函数 '" << fn_name << "' 未注册";
  if (!fn) {
    *is_error = true;
    return nullptr;
  }
  value_t *r = value_call(unit.vm, fn, nullptr, 0);
  if (!r || value_is_error(unit.vm, r) || value_type(r) != unit.vm->type_opaque) {
    *is_error = true;
    return nullptr;
  }
  return *(void **)value_data(r);
}

TEST_F(FfiCallbackE2E, CfuncPtrBuiltinYieldsCFunctionPointerFromSource) {
  ASSERT_TRUE(unit.compile(
      "cfunc add(a: i32, b: i32): i32 { return a + b; }\n"
      "func get_cb(): opaque { return cfunc_ptr(add); }\n",
      "srcptr"));
  bool err = false;
  void *code_from_clux = call_for_code_ptr(unit, "get_cb", &err);
  ASSERT_FALSE(err);
  ASSERT_NE(code_from_clux, nullptr);

  /* C 侧按 C ABI 调用由 **clux 源码**产出的函数指针 */
  int32_t got =
      c_apply_binary((int32_t (*)(int32_t, int32_t))code_from_clux, 20, 22);
  EXPECT_EQ(got, 42);
  EXPECT_EQ(c_apply_binary((int32_t (*)(int32_t, int32_t))code_from_clux, -8, 3),
            -5);

  /* 重复 `cfunc_ptr(f)` 复用同一 trampoline（不重复占可执行内存） */
  bool err2 = false;
  void *again = call_for_code_ptr(unit, "get_cb", &err2);
  EXPECT_FALSE(err2);
  EXPECT_EQ(again, code_from_clux);
}

/* 语言级 × 真实 C 消费者：clux 源码产出的闭包交给 **libc qsort** 反复调用。
   C ABI 是 (opaque, opaque) -> i32（qsort 传元素地址），因此 clux 侧参数声明为
   opaque；比较恒返回 0（保持原序）。真正被验证的是**调用真的发生了**：clux
   闭包体内自增全局计数，C 侧跑完 qsort 后从 VM 读回计数——这同时证明
   "C 库 → libffi trampoline → 解释器 → 回写 C" 的整条链，以及回调内对
   模块全局变量（root_scope）的写入在重入下有效。 */
TEST_F(FfiCallbackE2E, CluxSourceComparatorSortsThroughCQsort) {
  ASSERT_TRUE(unit.compile(
      "var CALLS: i32 = 0;\n"
      "cfunc raw_cmp(a: opaque, b: opaque): i32 {\n"
      "  CALLS = CALLS + 1;\n"
      "  return 0;\n"
      "}\n"
      "func get_cb(): opaque { return cfunc_ptr(raw_cmp); }\n"
      "func calls(): i32 { return CALLS; }\n",
      "srcqsort"));
  bool err = false;
  void *code_from_clux = call_for_code_ptr(unit, "get_cb", &err);
  ASSERT_FALSE(err);
  ASSERT_NE(code_from_clux, nullptr);

  int data[5] = {4, 2, 5, 1, 3};
  std::qsort(data, 5, sizeof(int),
             (int (*)(const void *, const void *))code_from_clux);

  /* 比较恒返回 0 → 元素集合不变。**不断言顺序**：C 标准不要求 qsort 稳定，
     等值元素的重排是实现自由（MSVC 的快排就会重排）。 */
  int seen[6] = {0};
  for (int i = 0; i < 5; i++) {
    ASSERT_GE(data[i], 1) << "index " << i;
    ASSERT_LE(data[i], 5) << "index " << i;
    seen[data[i]]++;
  }
  for (int v = 1; v <= 5; v++) EXPECT_EQ(seen[v], 1) << "value " << v;

  /* C 侧确实调用了 clux 闭包：计数从 VM 里读回（n 个元素至少 n-1 次比较） */
  value_t *calls = unit.lookup("calls");
  ASSERT_NE(calls, nullptr);
  value_t *r = value_call(unit.vm, calls, nullptr, 0);
  ASSERT_NE(r, nullptr);
  ASSERT_FALSE(value_is_error(unit.vm, r));
  EXPECT_GE((int64_t)read_sint(r), 4);
}

/* 反例：非 C ABI 的签名（option）在 cfunc_ptr 处**明确报错**，不猜着传参 */
TEST_F(FfiCallbackE2E, CfuncPtrRejectsNonCApiSignature) {
  ASSERT_TRUE(unit.compile(
      "cfunc bad(x: ?i32): i32 { return 0; }\n"
      "func get_bad(): opaque { return cfunc_ptr(bad); }\n",
      "badcabi"));
  value_t *fn = unit.lookup("get_bad");
  ASSERT_NE(fn, nullptr);
  value_t *r = value_call(unit.vm, fn, nullptr, 0);
  ASSERT_NE(r, nullptr);
  EXPECT_TRUE(value_is_error(unit.vm, r));
}

/* ================================================================
 * 纯 clux 端到端：**不写任何宿主代码**，libc qsort 用 clux 比较函数排好序
 *
 * 一条链全在源码里：clux 定义 `cfunc` → `cfunc_ptr` 取 C 函数指针 →
 * 经 `extern cfunc crt::qsort` 把数组地址与函数指针（都是 opaque）交给 C 库 →
 * C 库回调 clux 函数体（内部 `(a as ref *i32).*` 解引用比较）→ 数组升序。
 * `main` 自检（顺序 + 首尾值 + 回调计数），返回 0 才算通过。
 * ================================================================ */
TEST_F(FfiCallbackE2E, PureCluxProgramSortsWithCQsortCallback) {
  std::string src =
      std::string("extern lib \"") + kSystemLib +
      "\" as crt;\n"
      "extern cfunc crt::qsort(base: opaque, n: u64, size: u64, cmp: opaque): void;\n"
      "var CALLS: i32 = 0;\n"
      "cfunc cmp(a: opaque, b: opaque): i32 {\n"
      "  CALLS = CALLS + 1;\n"
      "  var x: i32 = (a as ref *i32).*;\n"
      "  var y: i32 = (b as ref *i32).*;\n"
      "  if (x < y) { return -1; }\n"
      "  if (x > y) { return 1; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr = .[5]i32{4, 2, 5, 1, 3};\n"
      "  crt::qsort(arr.&, 5, 4, cfunc_ptr(cmp));\n"
      "  if (arr[0] > arr[1]) { return 1; }\n"
      "  if (arr[1] > arr[2]) { return 1; }\n"
      "  if (arr[2] > arr[3]) { return 1; }\n"
      "  if (arr[3] > arr[4]) { return 1; }\n"
      "  if (arr[0] != 1 || arr[4] != 5) { return 2; }\n"
      "  if (CALLS < 4) { return 3; }\n"
      "  return 0;\n"
      "}\n";
  ASSERT_TRUE(unit.compile(src, "pureqsort"));
  value_t *main_fn = unit.lookup("main");
  ASSERT_NE(main_fn, nullptr);
  value_t *r = value_call(unit.vm, main_fn, nullptr, 0);
  ASSERT_NE(r, nullptr);
  ASSERT_FALSE(value_is_error(unit.vm, r));
  EXPECT_EQ((int64_t)read_sint(r), 0)
      << "clux 自检返回值（1=未排序 2=首尾错 3=回调没被调用）";
}
