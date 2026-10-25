# 只保留 C 后端：移除 VM 执行器与 libffi（迁移设计）

状态：**设计/待实施**（用户已选范围 A：删除 VM 执行器 + libffi，保留 sema/ctfe/C
后端共用的类型、值、作用域基础设施；先文档、后代码）。

目标：`clux` 只以 **C 后端**作为执行后端：

- `clux run` / `clux test` 默认编译 C、运行生成的可执行文件；
- 不再需要 `libffi`（生产构建与测试都不链接）；
- 保留并强化 `--emit-c`（含 `--emit-obj`、`--build-dir`）用于**调试/验证**，
  并把它接到 `run` / `test` 的调试路径上；
- sema、CTFE、类型系统继续工作（它们复用 `src/vm` 的 `type_t/value_t/scope_t`
  基础设施，**不在本次删除范围**）。

非目标：删除 `src/vm/type_*.c`、`value.c`、`scope.c`、`vtable.c` 等共享层
（选项 B，需要重写 sema/CTFE 的类型/值模型，工作量与风险都远大于本迁移）。

---

## 1. 现状盘点（以当前 HEAD 为准）

### 1.1 两条执行路径

| 路径 | 入口 | 依赖 | 产物 |
|---|---|---|---|
| VM（字节码解释器） | `driver_run_file*` / `driver_test_*` / `driver_run_bin` / `driver_run_asm` | `src/compiler`（3668 行）→ `src/vm/bcode*` + `exec*` + `function.c` + `vm.c` | 进程内执行 |
| C（宿主编译器） | `cbackend_build_program`（`clux build`） | `src/cbackend`（sema AST → C IR → C 源码 → clang） | `.c/.h/.o/.exe` |

C 后端有**自己的前端流水线**（加载/解析/sema + import），不经过
`driver/compile_module`，因此删除 VM 执行路径不会破坏 C 构建。

### 1.2 CLI 现状

- `clux build <file> [--emit-c[=P]] [--emit-obj[=P]] [--build-dir] [--output]`：
  已经是 C 后端，`--emit-c` 已可用于调试（编译失败时保留生成的 C）。
- `clux run`：`driver_run_file_exit` → **VM**；接受 `.cx` / `.cxb`（字节码）。
- `clux test`：默认 **VM**；`--backend=c` 已实现（改写成 C 测试运行器，逐个
  用例独立子进程）。
- `clux eval`：直接 `vm_new` + 字节码求值，**纯 VM**。
- `clux bc emit|asm|disasm`：字节码工具，**纯 VM/compiler**。

### 1.3 测试现状

- 测试文件 105 个、约 40k 行；**36 个文件、约 430 处**直接调用
  `driver_run_file*` / `driver_test_*`（其中 `driver_run_file*` 约 427 处），
  即进程内 VM 执行。
- `clux_expect_ok` / `clux_expect_reject` 共 **414 处**（34 个文件），其中
  "无内存泄漏"断言依赖 VM allocator（`delete_allocator` 打印 `memory leak`）。
- 纯 VM/bcode 测试（约 10 个文件，数百个用例）：
  `bcode_*_test`、`exec_test`、`vm_test`、`value_dump_test`、
  `asm_examples_test`、`array_bcode_test`、`driver_module_bcode_test`。
- VM FFI 测试（约 8 个文件）：
  `ffi_lib_test`、`ffi_extern_call_test`、`ffi_extern_var_test`、
  `ffi_struct_test`、`ffi_process_test`、`ffi_callback_e2e_test`、
  `ffi_assert_test`、`driver_ffi_own`。
- C 后端已有等价覆盖：`tests/c_backend_test.cpp`（含 FFI/callback/import/
  closure/export ABI）、`tests/c_backend_diff_test.cpp`。

### 1.4 libffi 依赖面

- CMake：Windows 用 `third_party/libffi-port`（win64 MASM），POSIX 用系统
  libffi；统一成 `FFI::ffi`，**只被 `clux_vm` 链接**；`CLUX_FFI_TYPED_CALLS`
  定义给测试。
- 代码：
  - `src/vm/ffi_lib.c`（900 行）：`dlopen/dlsym`、libffi `ffi_prep_cif/_var/
    ffi_call`，以及**与执行无关的类型分类** `ffi_data_kind_of` /
    `ffi_data_type_of`（sema 多处使用）。
  - `src/vm/ffi_cfunc.c`（236 行）：`cfunc_ptr` 的 libffi closure（VM 回调）。
  - `src/vm/exec_call.c`：VM extern 调用/typed call。
- C 后端 FFI（`c_lower_ffi.c`）是**纯代码生成**：生成 extern 原型并直接调用
  真实 C 符号，不链接 libffi。

### 1.5 需要保留的共享基础设施

`sema` / `ctfe` / `cbackend` 共同依赖（删除 VM 执行器后仍需保留）：

- `src/vm/type*.c`（类型池/布局/所有权/签名/字面量类型）、`value.c`、
  `scope.c`、`vtable.c`、`str_pool.c`、`type_ownership.c`；
- `function.c` / `builtin_len.c`（sema 按名查 `len`、`__deep_clone`、`data`
  等内建 fid；CTFE 可能构造函数引用）；
- `vm.c` 的**上下文/类型注册/global scope**部分（去掉 exec/bcode 注册）；
- `module.c`（`module_t`/import 元数据，driver 与 cbackend 共用）；
- `bcode.h` 中的 **FFI 返回类型码**（sema 用来构造调用点类型；可抽成独立
  `ffi_codes.h`，不必保留整个 bcode 层）。

> 结论：`src/vm` 约 16.3k 行中，执行器/bcode/FFI/VM 内建约 7–8k 行可删或
> 大幅裁剪；类型/值/作用域约 8k 行保留。

---

## 2. 目标结构

```
src/core         allocator/arena/vec/...（不变）
src/parser       AST/词法（不变）
src/desugar      装饰器/前脱糖（不变）
src/diag         诊断（不变）
src/module       import/package 元数据（不变）
src/sema         类型检查/shadow（保留；只依赖共享类型/值层）
src/ctfe         comptime 求值（保留；只依赖共享类型/值层）
src/cbackend     C IR/writer/host compiler（唯一执行后端）
src/driver       C 构建管线 + 测试运行器（VM 入口删除）
src/cmd          build/run/test/format/version（bc/eval 删除或隐藏）
```

- `clux build`：不变（C 后端），保留 `--emit-c/--emit-obj/--build-dir`。
- `clux run`：编译到临时 build dir → 运行 exe；`--emit-c[=P]`、`--keep-c`、
  `--build-dir` 透传；退出码 = main 返回值。
- `clux test`：默认 `--backend=c`；`--emit-c[=P]` 保留测试运行器 C 源。
- `clux bc` / `clux eval` / `.cxb` 输入：删除（不保留兼容层）。
- 库：`clux_vm` 裁剪为共享类型/值/作用域库（可后续更名为 `clux_types`）；
  `clux_compiler`、`clux_ctfe` 的 bcode/compiler 依赖清理；`FFI::ffi` 从链接
  中移除。

---

## 3. 分阶段实施（每阶段独立提交、可回滚）

### Phase 0：开关与基线

- CMake 增加 `CLUX_ENABLE_VM`（默认 ON）与 `CLUX_ENABLE_LIBFFI`（默认 ON），
  把 `src/vm` 的 exec/bcode/ffi 源文件、`clux_compiler`、FFI::ffi 链接、
  `clux_ffi_testdata` 都挂到开关下。
- 记录基线：全量测试通过集合、examples、二进制大小、`clux run`/`test`
  耗时、链接 libffi 情况。
- 验收：`-DCLUX_ENABLE_VM=OFF` 能配置（可暂不构建完整测试）。

### Phase 1：CLI 默认 C 后端（VM 仍可回退）

- `clux run` 改为 C 后端：`cbackend_build_program` + 临时 build dir + 运行；
  `--emit-c[=P]` / `--keep-c` / `--build-dir` / `--cc` 透传。
- `clux test` 默认 `--backend=c`；`--backend=vm` 保留为显式回退（deprecated）。
- `clux eval`、`clux bc`、`.cxb` 输入标注 deprecated（仍可用，Phase 3 删）。
- 验收：examples 全部 `clux run` 通过；`clux test --backend=c` 通过；
  `--emit-c` 在 run/test 下产出可检查的 C；VM 回退仍可用。

### Phase 2：测试运行器切换与泄漏检测

这是最大的一步：约 430 处进程内 VM 调用。

- 新 `driver_run_c(path, &exit_code)`：调用 C 后端编译到临时 exe 并 spawn，
  收集 stderr/退出码；`driver_run_file*` 的实现切到它（API 名称暂时不变，
  减少测试改动）。
- **泄漏断言**：在生成的 C 里加可选调试分配器（`CLUX_LEAK_CHECK=1` 时
  `clux_new/clux_free` 记账，进程退出打印 `memory leak ...`），保持
  `clux_expect_ok` 的"无泄漏"断言能力；若不接受该方案，则删除/降级这些
  断言（需确认）。
- 诊断断言分层：
  - 编译期诊断（sema/parser）仍进程内，原样保留；
  - 运行期错误文本（`exec: ...` 等 VM 消息）改为 C 运行器的输出/退出码，
    逐条迁移或删除；
  - `.cxb`/asm 相关断言删除。
- 删除/迁移纯 VM 测试（见 1.3），把 C 后端已有覆盖作为正式回归；VM 测试在
  `CLUX_ENABLE_VM=ON` 下保留一个发布周期。
- 验收：`CLUX_ENABLE_VM=OFF` 下 `clux_test` 全绿；examples 全绿；
  运行时间可接受（预计 400+ 次 clang 调用，约 1–2 分钟量级，需实测）。

### Phase 3：删除 VM 执行器与 compiler

删除（或移入 `legacy/` 一个发布周期）：

- `src/vm/exec*.c`、`bcode*.c`、`bcode_function.c`、`builtin_mock/panic/assert`；
- `src/vm/ffi_cfunc.c`、`ffi_lib.c` 的执行部分（见 Phase 4 拆分）；
- `src/vm/value_dump.c`（若仅 eval/test 用）；
- `src/compiler/`、`clux_compiler` target；
- `src/driver` 的 `driver_run_bin/asm`、`driver_build_bin/asm`、
  `driver_bin_to_asm/asm_to_bin`、`compile_module`（VM 字节码路径）；
- `src/cmd/eval.c`、`src/cmd/bc.c`、`driver_detect_input` 的 cxb/cxs 分支。

保留并整理：

- `bcode.h` 的 FFI 返回类型码 → 新 `include/core/ffi_codes.h`（sema 使用）；
- `vm.c` 裁剪为 `vm_new/destroy + 类型注册 + global scope`（去掉 exec/bcode/
  内建注册里 VM-only 的部分）；
- `function.c`/`builtin_len.c` 按 sema/CTFE 实际引用审计后保留最小集。

验收：`clux_vm` 不再包含/引用 exec/bcode/compiler；`clux` 二进制不再含
`driver_run_file` 的 VM 路径；`CLUX_ENABLE_VM=OFF` 构建通过。

### Phase 4：移除 libffi

- 把 `ffi_lib.c` 中**与执行无关**的部分抽到 `src/core/ffi_abi.c`
  （`ffi_data_kind_of/ffi_data_type_of`、类型码、ABI 可接受性判定），
  `include/vm/ffi_lib.h` 拆为 `include/core/ffi_abi.h` + VM-only 部分删除。
- 删除 `src/vm/ffi_lib.c`、`ffi_cfunc.c`、`exec_call.c` 的 extern 调用路径。
- CMake：删除 `third_party/libffi-port` 的 add_subdirectory/链接、`FFI::ffi`、
  `CLUX_HAVE_LIBFFI`、`CLUX_FFI_TYPED_CALLS`、`clux_ffi_testdata`
  （若其仅服务 VM FFI 测试）；POSIX 的 `find_library(ffi)` 删除。
- 验收：Windows/POSIX 构建均**不链接 libffi**（`dumpbin /dependents` /
  `ldd` 检查）；C 后端 FFI/callback/extern var 测试全绿。

### Phase 5：收尾

- `clux_vm` 更名/拆分为 `clux_types`（可选，纯机械）；删除死头文件
  （`bcode*.h`、`exec*.h`、`ffi_lib.h` 等）。
- CLI help/README/spec 更新；`docs/` 增加"C 后端调试指南"（`--emit-c`、
  `--build-dir`、`--cc`、`-I/-L/-l/-D`、保留 C/obj 的用法）。
- CI：矩阵含 `CLUX_ENABLE_VM=OFF`；lint 检查无 libffi 链接；测试超时阈值。
- 验收：全新 build 目录配置/构建/测试；examples；`--emit-c` 手工调试流程；
  二进制启动/体积对比基线。

---

## 4. 测试迁移清单（按文件类别）

| 类别 | 文件（示例） | 处理 |
|---|---|---|
| C 后端回归 | `c_backend_test.cpp`、`c_backend_diff_test.cpp` | 保留，作为主回归 |
| 闭包/所有权/泛型/结构等语义 | `closure_*`、`own_*`、`generic_*`、`struct_*`、`driver_*`（约 36 文件） | `driver_run_file` 切到 C 运行器；迁移泄漏/运行期诊断断言 |
| 纯 bcode/asm/exec | `bcode_*`、`asm_examples`、`exec_test`、`vm_test`、`value_dump`、`array_bcode`、`driver_module_bcode` | 删除（`CLUX_ENABLE_VM=ON` 下暂留一个周期） |
| VM FFI | `ffi_*`、`driver_ffi_own`、`driver_m6` | 删除；C 后端 FFI/callback 用例覆盖 |
| comptime/ctfe | `comptime_*`、`driver_comptime`、`sema_test` | 保留 CTFE 编译期断言；运行期执行走 C |
| allocator/vec/strmap 等 core | 不涉及 | 原样 |

新增测试：

- `CBuild.RunEmitsC`（`clux run --emit-c` 产出 C 且可编译）；
- `CBuild.LeakCheckReports`（调试分配器正/负例）；
- C 运行器差分：同一源码 C 运行 vs 已迁移的期望退出码；
- `NoLibffi`：CI 链接检查（脚本或 CMake 断言）。

---

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 约 430 处 VM 调用改走宿主 clang：慢/依赖环境 | 测试时长、CI 稳定性 | Phase 0 基准；并行 ctest；缓存 build dir；必要时只在语义测试用 C，纯类型测试保持进程内 sema |
| 泄漏断言依赖 VM allocator | `clux_expect_ok` 大量失败 | 生成 C 的调试分配器（Phase 2）或显式降级断言（需确认） |
| 运行期诊断文本差异 | 断言失败 | 分层迁移：编译期诊断保留；运行期消息逐条更新 |
| C 后端依赖 clang/gcc（生成 C 含 `$` 标识符扩展） | 平台/工具链限制 | 文档明确 host compiler 要求；评估去掉 `$` 或仅支持 clang/gcc |
| FFI 行为差异（extern var/callback/process） | 运行时回归 | C 后端已有 FFI 测试；Phase 4 前加差分用例；保留 `-l/-L/-I` 透传 |
| 删除公共 API（driver_run_bin 等） | 外部/测试引用 | 一个发布周期内保留 deprecated 符号（返回错误）；随后删除 |
| 回滚复杂 | 大改不可逆 | 每阶段独立提交；`CLUX_ENABLE_VM` 开关保留到 Phase 3 完成；Phase 4 前 tag |

---

## 6. 验收标准（整体）

1. `cmake -S . -B build`（无 libffi 依赖）→ `clux` 构建成功；
2. `clux run examples/...` 全部通过，退出码与 C 可执行一致；
3. `clux test` 默认 C 后端，全量测试通过（允许的 skip 列表需清单化）；
4. `clux build --emit-c[=P]` 与 `clux run/test --emit-c` 均能产出可读 C；
5. 链接检查：Windows `dumpbin /dependents clux.exe`、Linux `ldd` 无 libffi；
6. 闭包/ABI 回归：`CBackend.BuildClosure*`、`BuildExportClosure*`、
   `CBackendDiff.*`、`ClosureValueOwn.*` 全绿；
7. 文档：README/CLI help/`docs/c-only-migration.md` 与实现一致。

---

## 7. 已确认决策（2026-10-25）

1. `clux eval` 与 `clux bc` **直接删除**（不保留兼容层）。
2. 纯 VM/bcode 测试**直接删除**；用例后续依据白皮书重新设计实现。
3. 泄漏检测：长期做成产品级侵入式性能工具（后续规划）；**当前阶段只实现
   一个泄漏检测全局分配器**——在生成 C 的 `main` 中创建/销毁，
   `clux_new`/`clux_free` 统一走该分配器（见下）。
4. 宿主编译器只承诺 **gcc/clang**（生成 C 使用 `$` 标识符等扩展）；Windows
   用 clang-cl 的 MSVC 兼容模式也不在本项目关心范围，不做 MSVC 兼容。
5. `.cxb` 字节码格式**完全删除**（含读取/反汇编/执行）。

### 已实现的泄漏检测全局分配器（Phase 2 前置）

- 生成 C 中增加共享运行时：`clux_rt_state`（入口单元定义，依赖模块 extern），
  `clux_rt_alloc/free/init/shutdown`；`clux_new`/`clux_make`/`clux_free`
  全部走它。
- `main` 进入时 `clux_rt_init()`、退出时 `clux_rt_shutdown()`；退出时若有
  未释放块，向 stderr 打印 `memory leak detected: ...`（保留原 VM allocator
  的诊断文本，便于测试断言）。
- 入口单元的运行时仅在"整个程序存在堆分配"时发射（两遍 lowering 统计），
  避免无分配程序被迫包含 `<stdlib.h>` 与用户 FFI 声明（如 `div`）冲突。
- 已加 gtest：`CBackend.LeakDetectorReportsAndCleanProgramSilent`；跨模块
  分配/释放/泄漏上报用 `build/tmp/rt_proj` 手工验证。

---

## 8. 参考：现有可用的 C 调试能力

```powershell
# 只生成 C（不调用 cc）
clux build file.cx --emit-c
clux build file.cx --emit-c=out.c

# 生成对象 / 指定构建目录 / 指定编译器
clux build file.cx --emit-obj
clux build file.cx --build-dir=build\debug --cc=clang
```

生成失败时 C 后端会保留 `.c` 并打印路径（现有行为）；Phase 1 会把同一套
选项接到 `clux run` / `clux test`，并增加 `--keep-c` 便于断点/阅读生成代码。
