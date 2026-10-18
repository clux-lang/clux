# clux 里程碑规划

> **状态基准**：M1–M5 均已实现并端到端可运行。本文档记录各里程碑的交付内容、
> 设计文档入口、以及**明确尚未实现的部分**（避免"文档说完成、代码没有"）。
> 验证方式见文末"当前验证状态"。

## M1: 最小可执行子集 ✅ 已完成

**目标**：实现"裁剪后的 C"子集的解释器

**语言特性**：基础类型(i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/bool/void)、var 变量声明(含推断)、func 函数定义(a:type 风格)、if/else、while、for、break/continue/return、算术/比较/逻辑/位/赋值运算、true/false 字面量、printf 硬编码、C 风格注释

**不支持**：指针、所有权、struct/union/enum、数组、字符串、模块、错误处理、预编译指令

**交付标准**：`clux run hello.cx` 能正确执行含变量、运算、控制流、函数调用的程序 ✅ 8 个 examples 端到端通过

**任务**：T1 骨架 → T2 Lexer → T3 AST → T4 Parser → T5 类型系统 → T6 语义分析 → T7 解释器 → T8 端到端集成 ✅ 全部完成

**设计文档**：[language-spec-m1.md](language-spec-m1.md)、[architecture-m1.md](architecture-m1.md)

---

## M2: 与 C 表达力齐平（指针除外）✅ 已完成

**目标**：补全语言功能达到 C 的表达力（指针除外），保持 clux 自己的语法风格

**语言特性**：struct、enum（严格分离）、tag union、cunion、静态数组 `[N]T`、元组 `<T1,T2>`、switch（if 语法糖）、do-while、`foreach`（for-each 前脱糖 → 索引 `for`，见 `src/desugar/`）、type 别名 + 类型计算、sizeof/alignof/typeof、函数类型与函数字面量、位运算复合赋值、三元表达式、元组↔数组互转、optional `?T` + nil 重构、comptime var/func、CTFE、const/volatile 真实类型、fill 值包、鸭子类型协议、字节码工具链

**交付标准**：全部单元测试通过 + 新 examples 端到端通过 ✅

**设计文档**：[m2-design.md](m2-design.md)

**任务**：
- Phase 0: Lexer + AST 基础（新关键字/符号 + 新 AST 节点 + `parse_type_expr`）✅
- Phase 2: VM 类型系统扩展（复合类型 + interning + vtable + `is_own` 借用字段）✅
- Phase 3: Sema 扩展（`resolve_type` 升级 + 类型计算 + 鸭子类型检查）✅
- Phase 4: 构造 + 访问（`.<type>{...}` + 字段访问 + enum variant + 新字节码）✅
- Phase 5: 控制流 + 表达式补全（switch desugar + do-while + 三元 + 位运算复合赋值）✅
- Phase 5b: `foreach`（2026-10）——**前脱糖层**（`src/desugar/`，parser → sema 之间的纯 AST 变换）展开成索引 `for`；`tests/desugar_test.cpp` 断言 AST 形状，`tests/foreach_test.cpp` 端到端 ✅
- Phase 6: 集成 + 测试（examples + 全测试通过 + 文档更新）✅
- 工具链（非主线）：字节码 `.cxb` 与汇编 `.cxs`，统一由 `bc` 子命令提供（`bc emit` 源码→字节码 / `bc asm` 汇编 / `bc disasm` 反汇编，`-o` 指定输出）；`run` 按内容判定（有 `CXBC` 头当字节码，否则源码）；`build` 专责未来的机器码二进制（未实现）✅ 已完成（2026-09-12）

**明确未实现**：`.?`（try）仅词法预留（`operator '.?' (try) is not implemented yet`）；struct 方法；struct/tuple 下标访问（当前仅数组/切片）；switch 模式解构绑定（当前仅值匹配）。

---

## M3: 指针与所有权系统 ✅ 已完成（§3–§11 + §13.2）

**目标**：轻量单所有权 + 指针系统（无全局 borrow checker、函数边界黑盒、运行期零检查）

> **2026-10-16 更新**：RC 系 `share`/`weak` 已移除（§12），待后续重新设计。

**语言特性**：`own *T` / `ref *T` / `fatal *T`、作用域标注 `'<a,b>`、`move` / `clone`、`new T{...}`、后置 `.&` / `.*`、`opaque`（≈ C `void*`）、可空指针 `?*T`、闭包捕获规则

**交付标准**：全部单元测试通过 ✅（[tests/driver_m3_test.cpp](../tests/driver_m3_test.cpp)）

**设计文档**：[m3-design.md](m3-design.md)

**明确未实现（后续里程碑）**：
- **销毁点的编译期生成**（§13.3–§13.4）：当前销毁由 VM 运行时完成（`scope_destroy` 遍历 owned → `ptr_free_owned_recursive` 递归释放 + vtable dispose）；"compiler 层生成销毁点、多后端统一"待 M7 C 后端 / M12 原生后端落地。
- **FFI 落地**：`extern func malloc/free` 等语法已定（§8.4），具体 C 互操作归 M6。
- **同步原语**：竞争写由开发者负责，属标准库范围（M9）。

---

## M4: 切片与字符串 ✅ 已完成（2026-10-09）

**目标**：引入切片（动态长度胖指针）与运行时字符串

**语言特性**：`own []T` / `ref []T` / `fatal []T` 胖指针（`{ptr,len}`）、`make(T,N,init...)` 宏、`len` 内建、`[a:b]` 切片语法（恒产 `ref []T`）、`str` → `ref [] const u8`、`const []T` / `[] const T` 区分、所有权交互（move/clone/借用、禁止全局 `own []T`）、运行期边界检查

**交付标准**：全部单元测试通过 ✅（[tests/driver_m4_test.cpp](../tests/driver_m4_test.cpp)）

**设计文档**：[m4-design.md](m4-design.md)（注：该文档 §4 的"待实现"表述已过期，代码已落地）

**明确未实现（后续里程碑）**：字符串标准库（拼接/查找/替换等）；UTF-8 字符级操作（`[]u8` 是字节序列）。

---

## M5: 模块系统 ✅ 已完成（按 v2 设计落地）

**目标**：跨文件代码组织（参考 TypeScript，更简化）

**语言特性（已实现）**：
- `export` 前缀修饰全局实体（func / var / type / enum / struct / union / cunion）进入模块导出表
- `import <alias> from "<path>"` 导入模块为命名空间，`::` 访问导出成员（`::` 复用 enum variant 访问，sema 按左值分派）
- **相对路径解析**：`./x` / `../y` 相对**被导入文件所在目录**解析；`\`→`/` 归一、
  折叠 `.`/`..`、省略扩展名自动补 `.cx`、规范路径作为模块 key 去重；文件不存在报
  `module not found: './x'`
- **代理导出** `export * from "./x"`：全量转发依赖模块导出符号（支持链式），
  同名冲突报 `export conflict: symbol '...' already exported`
- **非相对路径统一拒绝**：**不按文件后缀判断导入类型**——`.so`/`.dll`/`.dylib`
  只是文件名，不表示文件类型；非 `./`/`../` 开头一律报
  `import path must be relative (...) package and dynamic-library imports are not supported in M5`
- 每模块独立走完整流水线（lex → parse → sema → compile），产物为 `module_t{bc, global_scope, exports, canonical}`
- `vm->modules` 以规范路径为 key；`bcode_function_t` 自带所属模块的 `bc`，跨模块调用切换 `root_scope` + `bc`
- 运行期成员可见性以 **`module->exports`** 为唯一来源（`op_get_member` 查导出表，
  与 sema 的 `::` 解析一致——代理导出因此对运行期同样可见）
- 循环依赖检测（路径栈）
- 全局 comptime var 类型必须是 `const T`（§3.4）

**交付标准**：import/export/代理导出端到端通过 ✅
（[tests/driver_import_test.cpp](../tests/driver_import_test.cpp) 14 个用例、
示例 [examples/modules/](../examples/modules/)）

**设计文档**：[m5-design-v2.md](m5-design-v2.md)（**已落地的设计**）；[m5-design.md](m5-design.md)（v1 原始设计，部分内容已被 v2 与本轮修正取代）

**实现位置**：`src/driver/module_path.c`（路径分类/归一化/解析/存在性）、
`src/driver/driver.c` 的 `compile_module`（递归编译 + 导出表构建与代理合并）、
`src/sema/sema.c` 的 `pass0_imports`、`src/parser/parse_import.c`
（`parse_import` / `parse_reexport`）、`src/vm/exec.c` 的 `op_import` / `op_get_member`。

**约定**：example 目录**顶层 `.cx` 是入口程序**，其导入的模块放在该目录的
子目录中（如 `examples/modules/lib/*.cx`）——模块文件没有 `main`，不作为独立示例运行。

---

## M6: FFI 系统（待实现，设计已完成）

**依赖**：模块系统（M5 ✅）

**语言特性（设计）**：`import libc from "libc.so.6"` 动态库导入；`extern cfunc libc::malloc(size: u64): fatal opaque;` 声明（`dlsym` 查找）；`cfunc name(...) { ... }` 定义（C ABI，可作回调传给 C）；libffi 驱动动态调用与 closure；C 兼容类型约束（禁止 `own *T` / option / tag union / slice / 闭包）

**设计文档**：[m6-design.md](m6-design.md)

**当前状态**：`src/` 无任何 FFI 代码，`CMakeLists.txt` 未引入 libffi，lexer 无 `cfunc` / `extern` 关键字。M5 已实现模块相对路径导入与代理导出，并统一拒绝非相对路径——M6 接续该位置时**必须重新决定动态库的绑定语法**：不得以 `.so`/`.dll`/`.dylib` 后缀判定"这是动态库"（后缀不表示文件类型），需用显式语法表达绑定意图，详见 [m6-design.md](m6-design.md) §1.1。

---

## 后续里程碑（待详细设计）

- **M7**: 转译 C 后端（`clux build` 的机器码目标之一；亦承接 M3 §13.3 销毁点编译期生成）
- **M8**: 错误处理机制
- **M9**: 标准库 v1（含字符串库、同步原语）
- **M12**: 原生后端（与 M7 共享编译期所有权分析）

---

## 当前验证状态

在 Windows / clang 22.1.8 / Ninja / Debug 下实测（2026-10 复验，含本轮代码修复）：

| 项目 | 结果 |
|------|------|
| 注册测试数（`ctest -N` / `--gtest_list_tests`） | 1570 |
| `clux_test` 实际通过 | **1570 / 1570** |
| `examples/*/*.cx` 入口示例端到端 | 35 / 35 |
| 入口示例退出时泄漏报告 | 0 |
| `examples/asm/*.cxs` → `bc asm` → `run` | 5 / 5 |
| `.cxs → .cxb → .cxs → .cxb` 字节一致性 | 5 / 5 |
| `clux format` 幂等（examples 全量 .cx） | 37 / 37 |

> **示例扫描范围**：`examples/<name>/*.cx` 是入口程序（各目录一个），
> `<name>/lib/*.cx` 等子目录中的是它导入的模块（无 `main`，不单独运行）。

> ⚠️ **运行测试的环境要求（本机实测）**：若干测试通过
> `std::filesystem::temp_directory_path()` / `tmpnam` 写临时文件，而本机
> **子进程被拒绝写入 `%TEMP%` 根目录**（`fopen` 返回 `errno 13` Permission denied；
> 已在独立最小 C 程序中复现，pwsh 自身可写、其子进程不可写）。
> 未重定向时这些测试会失败甚至触发 UCRT 断言中止，导致**测试套件提前退出**
> （表现为"只有零星失败"，实际大量测试从未执行）。
> 运行前把 TMP/TEMP 指向可写目录即可全绿：
>
> ```powershell
> New-Item -ItemType Directory -Force -Path build\tmp | Out-Null
> $env:TMP = (Resolve-Path build\tmp).Path; $env:TEMP = $env:TMP   # 或任意可写目录
> .\build\clux_test.exe
> ```

### 本轮修复的缺陷（原"已知缺陷"清单）

以下缺陷均已修复并附回归测试；保留记录以便追溯：

1. **`clux format` 删除顶层 `union` 声明**（数据损坏级）→ 已修：`render_stmt`
   补 `AST_UNION_DEF` 分支。回归：`FmtRegression.UnionDeclPreserved`。
2. **`clux format` 丢失 `import` 语句与 `export` 前缀**（同类静默删除）→ 已修：
   补 `AST_IMPORT` 渲染分支；`render_program` 还原 `is_exported` 前缀。
   回归：`FmtRegression.ImportStmtPreserved` / `ExportPrefixPreserved` /
   `CunionDeclPreserved`。
3. **`clux format` 单遍不幂等**（34 个 examples 中 10 个）→ 已修两处根因：
   (a) `render_program` 的分隔换行未检查 `at_line_start`，上一顶层项以独立行注释
   结尾时会凭空多出空行；(b) 括号决策的绑定力表**漏登记 `is`**，且查表失败时
   `lp/rp` 保持未初始化（UB，读垃圾值）。回归：
   `FmtRegression.IdempotentAfterStandaloneComment` / `IsOperatorParensStable` /
   `IdempotentAcrossDeclKinds`；34/34 examples 三遍格式化结果一致。
4. **诊断重复打印**（每个编译错误输出两遍）→ 已修：移除 `compile_module` 内的
   三处 `diag_print_all`，统一由最外层入口打印一次（`sema_analyze` 以
   `!diag_has_error` 收尾，故不会出现"依赖失败却报成功"的静默情形）。
5. **聚合值赋值泄漏 + `var b = a` 退化为别名**（结构/数组/元组）→ 已修：引入
   `value_is_fresh`（新建且未绑定到名字）作为 DEFINE/STORE "接管"路径的前置条件。
   回归：`ExecTest.AggregateCopyInitIsNotAlias` / `AggregateStoreFreshTempNoLeak` /
   `AggregateStoreFromVariableNoLeak`、`Driver.RunFileStructCopyInitIsNotAlias` /
   `RunFileAggregateAssignChain`。
6. **连续两次聚合赋值的访问违例崩溃**（`b = a; a = b;`，`0xC0000005`，进程挂起）——
   属**既有缺陷**，此前因测试套件提前退出而未被发现；同一处 `value_is_fresh` 修复
   顺带解决。回归：既有测试 `Driver.RunFileStructLayoutCompatibleAssignPasses`
   （修复前必崩）现已通过。
7. **37 条编译告警**（`-Wall -Wextra -Wpedantic`）→ 已清零。其中
   `-Wincompatible-pointer-types-discards-qualifiers` 是**真实的 const 违约**
   （`value_dump_*` 声明 `const vm_t *`，内部却创建借用值并 track 进 scope），
   已改为非 const 签名；其余为聚合初始化 / 死代码 / 复合字面量。分类明细见
   [architecture-m1.md](architecture-m1.md) 的"编译警告"节。

8. **M5 收尾项**（相对路径 import、`export * from` 代理导出、非相对路径诊断、
   模块示例）→ 已全部实现；期间发现并修复 **`op_get_member` 查 `global_scope`**
   与 sema/设计（查 `exports`）不一致的问题——它是代理导出"sema 通过、运行期
   找不到成员"的根因。回归：[tests/driver_import_test.cpp](../tests/driver_import_test.cpp)
   14 个用例、示例 [examples/modules/](../examples/modules/)。

### 质量专项：sema/driver 缺陷修复（本轮）

代码质量评估（4 路深读 + 实跑复核）后按优先级修复的缺陷。每条都有可复现最小
用例（归档在 [repro/sema-defects/](../repro/sema-defects/README.md)）与 gtest 回归。

9. **program 类型 id 跨模块重叠**（M5 功能性 bug）：`sema_type_register` 曾按
   `TYPE_ID_PROGRAM_BASE + 模块内下标` 分配 id，而运行期 `vm->types_by_id` 是**全
   VM 单表**、模块字节码用 `LOAD_TYPE <id>` 直接索引 ⇒ 两模块 id 空间重叠，后编译
   模块的 `DEFINE_TYPE` 覆盖先者槽位。症状：**被导入模块内的 struct 构造运行期报
   `construct: unsupported type`**（数组不受影响——走 `PUSH_ARRAY` 内联重建）。
   修复：id 改为全 VM 全局唯一（`type_id_alloc` + `vm.type_id_next`），
   `sema_type_t` 增 `index`，hoist 的 `done[]` 改按登记下标索引。
   回归：`DriverImport.StructConstructInsideDependencyModule` /
   `ProgramTypeIdsUniqueAcrossModules`。
10. **编译失败路径泄漏整个 sema 作用域树**：`driver_compile_to_bytecode` 三条失败
   路径调 `driver_compiled_dispose` 时 `out->scope_tree` 尚未赋值，而
   `sema_destroy` 明确不释放作用域树 ⇒ 实测 `bc emit` 一个 sema 错误文件会打印
   **42 条 allocator leak**（`sema_scope_t`/`sema_symbol_t`/…）。修复：三条路径各补
   `sema_scope_destroy`。回归：`Driver.CompileErrorPathDoesNotLeakSemaScopeTree`。
11. **常量折叠在 9 处槽位丢失**：`sema_expr` 用可写节点槽（`ast_node_t **`）做折叠
   写回，9 处调用传的是局部变量副本且不回写 ⇒ comptime 符号引用漏到运行期 →
   `run: exec: undefined variable`（union 字段构造、`make` 元素位）。
   修复：补回写（含 `fields_last` 修正）+ 链上指针迭代。回归：
   `DriverM4.MakeElementsFoldComptimeVar` / `Driver.RunFileUnionFieldComptimeVarFolds`。
12. **匿名构造字段二次求值**：推断分支漏 `return`，落入具名分支对同一批字段表达式
   再求值一遍 ⇒ 含 `move()` 的字段被误报 `used after move`（具名构造正常）。
   修复：补 `return`。回归：`Driver.AnonConstructFieldEvaluatedOnce`。
13. **`sema_expr` 的 `default:` 静默吞掉未处理节点**：改为内部错误诊断 + 补
   `AST_FILL` 分支。**该改动立刻暴露两个既有漏洞**：(a) for 的步进槽放赋值语句时
   `AST_ASSIGN` 从未被类型检查（`j = "x"` 也能编译）；(b) 4 种类型表达式
   （元组/签名/可选/指针）在表达式位置没有分支，`var x = <i32,i32>;` 之类合法写法
   被静默吞掉。两者均已修。回归：`SemaTest.ForUpdateAssignmentTypeMismatchRejected`
   / `ForUpdateValidAssignmentPasses` / `FillInStructConstructorRejected` /
   `TypeExpressionsAreValuesInExpressionPosition` / `Driver.RunFileTypeExpressionsAsValuesPass`。
14. **`anon_ct` 目标类型栈 push/pop 不对称**（12 处"有条件压栈 + 无条件弹栈"，
   类型 NULL 或栈满时误减外层上下文）→ 新增配对接口
   `sema_anon_push`/`sema_anon_pop`，16 处调用点统一。回归：
   `SemaTest.AnonCtPushPopTokenIsSymmetric`。
15. **死状态清理**：`vm->comptime`（16 写 0 读，8 对 save/restore 纯仪式）、
   `sema->reexports`（2 写 0 读）、`sema->func_has_return`（0 读）、
   `sema_func_t::name`、`sema_scope_t::frame`、`compiler_t::current_scope`
   全部删除。**注意**：评估报告称 `flow_snap.after` 只写，实测在用
   （`stmt.c:1547/1634/1664`），未删。
16. **comptime var 支持所有类型 + 所有常量位置**（用户确认"理论上 comptime var
   应该支持所有类型"后实现）：
   - **根因**：常量槽位由 CTFE 求值，而 CTFE 的标识符解析只查 VM 作用域链；
     comptime var 不生成运行期字节码，其常量此前从未落在这条链上 ⇒
     `[N]i32` 一直报 `array length must be a compile-time constant`。
     修复：`sema_eval_comptime_var` 求值成功后把**常量真实值**绑定进 VM 作用域
     （全局 → `vm->root_scope`，与全局变量的 shadow value 同处；局部 → 当前块作用域）。
   - **求值顺序**：新增 `pass_comptime_early`（`pass1b` 之前"试跑"全局 comptime
     var，成功才提交、失败丢弃诊断留给 `pass_globals`）解决 `type T = [N]i32`
     与签名 `[N+1]i32`；`walk_block` 提升预扫里按源序求值局部 comptime var 解决
     局部 `type T = [C]i32`（且 comptime var 不再参与 `prior_vars` 遮蔽预检）；
     `stmt_build` 对"含非字面量数组边界 + 作用域内有未求值 comptime var"的槽位
     推迟到 3b（否则 3a 硬诊断触发快速失败闸门，3b 不运行）。
   - **类型**：编码/折叠扩展 struct/tuple（复用标准构造节点 `AST_CONSTRUCT`，
     未引入设计稿里的 `AST_CT_CONST`）；CTFE 补聚合值构造
     （`ctfe_make_aggregate`）。至此支持整型/浮点/布尔/字符串/枚举/数组/
     struct/tuple/函数引用。
   - **求值顺序无关（不动点轮次，2026-10 补齐）**：全局编译期实体之间有真实
     依赖环——`comptime var N = add(1,2); type T = [N]i32; func f(a: T)`：
     N 需要 add 的签名（`pass2`）、T 需要 N（`pass1b` 阶段）、f 需要 T。
     单趟顺序无解，因此 `pass1b_types` / `pass2_types` / `pass_globals` 都做成
     **可反复调用**（各自跳过已就绪符号），外面套"试跑轮"循环直到不再有新实体
     就绪（`sema_ready_count` 度量进展；每轮诊断进临时缓冲丢弃），最后用真实
     诊断跑一遍报出真正无解的错误。**常量位置不再因为书写顺序挑食**
     （type 别名写在 comptime var 之前也行）。
     副带修正：`pass2_types` 不再用"签名已解析"在入口短路（否则 §5 返回 ref
     标注校验会在正式轮被跳过）、槽位未解析成功时不建半有效签名；
     `ctfe_call_value` 对**程序函数引用**（sema 折叠出的 `AST_FUNC_REF`，
     `cfunc == NULL`）按 AST 解释执行，使"普通函数出现在常量槽位"
     （m2-design §9 决策点 2）真正可用。
   - **同时修**：`A = 2` 赋值 comptime var 的拒绝（常量真实值现在也在 VM 作用域
     里，不能再靠"查不到"识别，改为查符号表 `is_comptime`）。
   - 回归：`tests/driver_comptime_test.cpp`（14 个用例：全局/局部 ×
     边界/别名/签名/表达式/make/sizeof × 全部类型 × 晚期求值/逆序书写）。

**仍未完成**：M6 FFI（未开始，其动态库绑定语法需按"不以后缀判类"重新定稿）、
`clux build`（M7/M12 机器码后端）、M8 错误处理、M9 标准库。
17. **sema 结构重构（第一轮）**：拆 `sema_expr` + 作用域对位自检
   - `src/sema/expr.c` 的 `sema_expr` 原是 **2114 行**单函数（34 个 case，最大
     case 588 行）。按 AST kind 抽出 **27 个静态处理器**
     （`ex_new` / `ex_construct` / `ex_ident` / `ex_call` / `ex_index` /
     `ex_member` / `ex_slice` / `ex_make` / `ex_move` / `ex_scope_ref` /
     `ex_unary` / `ex_unwrap` / `ex_ternary` / `ex_func_ref` / `ex_sizeof_alignof` /
     `ex_type_expr` / …），`sema_expr` 只剩 **114 行分派表**。
     纯搬移（每个 case 体逐字未改），1563 测试全绿。
   - 新增求值基础设施（拆分的润滑剂）：`val_void(sema)`（void shadow 占位，
     替换 156 处内联 `value_make_shadow(...type_void)`）、`is_poison(sema,v)`
     （错误 / void / NULL 统一判定，替换 26 处双条件）、`EX_GUARD` 宏。
   - **3a/3b 作用域树对位自检**：新增 `check_scope_consumed`（`stmt.c`），在
     `walk_block` 消费结束时校验"消费的子作用域数 == 3a 建树数"，失配即报
     `internal: scope tree desync (consumed N of M child scopes)`。
     此前所有消费点统一写 `child ? child : scope`，失配**静默降级**——表现为
     符号永不激活、`type` 遮蔽语义反转、借用计数泄漏等极难定位的错误。
     不可达尾部（确定 return 之后）豁免：3a 仍按结构建树而 3b 提前 break。
     自检已用"临时删除一处 `(*idx)++`"验证确实触发。

18. **sema_t 状态收敛（第一轮）**：把散落字段收进显式职责子上下文
   - `sema_fn_ctx_t fn`：**当前函数 walk 上下文**（`return_type` / `def` /
     `walking_comptime` / `local_func_base` / `local_func_param_scope` /
     `in_call_callee` / `loop_depth`）。原先这 7 个字段平铺在 `sema_t` 上，
     彼此关系（哪些一起设置、哪些一起恢复）只能靠注释推断
   - `sema_expr_ctx_t ex`：表达式求值上下文（`anon_ct[16]` / `anon_ct_depth`）
   - `mod`：M5 模块上下文（`path_stack` / `module_path`）
   - 共 **100 处**访问点机械重命名（`sema->func_def` → `sema->fn.def` 等），
     行为逐字不变；错漏由编译器立刻捕获（字段名不存在）
   - **顺带消除两处"逐字段保存/恢复"的脆弱写法**：`sema_walk_function`
     （原先结尾把 `return_type`/`def`/捕获上下文清 NULL、只恢复
     `walking_comptime`）与 `sema_check_func_literal`（原先手工备份 5 个字段）
     改为**整体保存/恢复 `sema_fn_ctx_t`**——上下文将来新增字段不会再被漏掉。
     保存点必须在函数**入口最先**：`sema_walk_function` 在写入
     `local_func_base` 之前就要取快照，否则会把刚写入的新值当成旧值恢复。
   - 下一步（未做）：`sema_t` 的环境字段（vm/diag/tokens/arena/global_scope）
     与队列字段（funcs/types/func_id_next）仍可继续分组，并进一步把
     `sema_fn_ctx_t` 的"进入/离开"包装成配对接口。

19. **分层修复 + driver 流水线合并**
   - **`module_path` 抽到中立层**：`src/driver/module_path.c` + `include/driver/
     module_path.h` → `src/module/module_path.c` + `include/module/module_path.h`
     （新增 `clux_module` 静态库，driver 与 sema 都可依赖，它自身不依赖二者）。
   - **import 解析改为 driver 注入的回调**：sema 原本直接调用 driver 的
     `compile_module()` 并做路径规范化（`sema → driver` 反向依赖）。现在
     `sema_t` 只持有 `{sema_import_resolver_t resolve; void *user;}`，遇
     `AST_IMPORT`/`AST_REEXPORT` 回调到 driver；路径分类/规范化、存在性检查、
     递归编译（缓存 + 循环依赖检测）与全部诊断都在 driver 侧
     （`driver_resolve_import` + `driver_import_ctx_t`）。`sema_create` 因此
     去掉了 `path_stack` 参数，`sema->mod` 整体消失。
     **验证**：`src/sema/**` 不再 include 任何 `driver/`、`module/` 头；把
     `clux_module` 从 `clux_sema` 的链接列表移除后仍能构建 ✓。
   - **解开 `sema ↔ ctfe` 头文件循环**：`include/ctfe/ctfe.h` 原先
     `#include "sema/sema.h"`，而 sema 的 `comptime.c`/`expr.c`/`sema.c` 又要
     include 它调用 `ctfe_eval` —— 互相包含。现在 ctfe.h 只用**前向声明**
     （`struct sema_t; struct _sema_scope_t;` + 对应 typedef），ctfe 源文件
     各自 include `sema/sema.h`。依赖方向单向化：ctfe → sema（头文件无环）。
     说明：`sema → ctfe` 的**函数调用**关系保留（CTFE 是 sema 使用的编译期
     求值服务），有意义的是**头文件不再成环**、编译顺序不再互相牵制。
   - **driver 两条流水线合并为一条**：`driver_compile_to_bytecode`
     （`bc emit` / `build -asm` / `build -bin` 的路径）原来自带一套内联
     lex→parse→sema→compile，不支持模块导入、不做入口路径规范化、无循环依赖
     检测。现在它直接走 `compile_module` 同一核心，`driver_compiled_t` 只持有
     `vm` + `module`（bc/global_scope/exports 归 module 所有，`vm_destroy` 统一
     释放，不再单独 `bcode_destroy`/`sema_scope_destroy`）。
     **行为提升**：`clux bc emit` 现在能编译带 `import` 的程序（此前相对路径
     以进程工作目录为基准，嵌套目录下直接报 module not found）。
     顺带修一处泄漏：入口路径与 `path_stack` 由调用方显式释放（本项目分配器
     在 `delete_allocator` 时把存活分配报为泄漏）。
   - **已知缺口已修（见第 21 项）**：`.cxb` 容器此后承载整张模块图。
21. **`.cxb` 承载模块图（M5 字节码层闭环）**
   - **问题**：容器 v1 只存**入口模块**的字节码，依赖模块不在其中 →
     `run x.cxb` 在 `IMPORT` 指令处报 `module not found`。M5 的模块语义因此
     在字节码层断裂（源码路径正常、字节码路径失败），而 `.cxb` 本就是 M5 的
     产物形态之一。
   - **容器 v2（`include/vm/bcode_graph.h` + `src/vm/bcode_graph.c`）**：模块表
     （`module_count` + `entry_index`），每模块携带 **canonical 路径 + 字符串表
     + 字节码 + 导出表 `(name, owner_index)`**。模块顺序 = `vm->modules` 插入
     顺序 = **依赖优先**；入口模块的 path 写空串（运行期只有 `IMPORT` 按路径查表，
     而入口不可能被 import——那构成循环依赖，编译期拒绝）。导出表必须随容器
     携带：名单来自 AST 的 `is_exported`（不在字节码里），`owner_index` 记录
     "值真正所在的模块"——`export * from` 代理导出的值借用依赖模块的
     `global_scope`，加载方据此原样重建。v1 单模块容器继续可读（兼容分支）。
   - **文本形态（`src/vm/bcode_graph_text.c`）**：多模块 `.cxs` 在每段代码前插入
     `MODULE [path]` 与 `export name owner` 指令；**单模块文本与旧格式逐字一致**
     （不写指令行），`examples/asm` 的 5 个手写样本与既有往返测试不受影响。
     emit 侧渲染与 `.cxb → .cxs` 渲染**共用同一实现**（前者经"序列化→反序列化"
     复用后者），因此两侧文本必然一致。
   - **加载路径（driver）**：`driver_execute_graph` 按表序逐模块"建 scope →
     执行注册段 → 重建导出表 → 绑定 vm->modules"，再在入口模块里找 `main`
     调用；`run .cxb`、`run .cxs`（多模块汇编）、`bc disasm`、`bc asm` 全部切到
     模块图（`driver_execute_bytecode` 单模块执行器随之删除）。
   - **端到端验证**（3 模块程序：main → api 代理导出 → math 定义）：
     `bc emit` 产出 696 字节容器（含 3 模块）→ `run .cxb` 退出 0（修复前
     `module not found`）→ `bc disasm` 文本含 `MODULE "./…/math.cx"` 与
     `export add 0`/`export PI 0`（代理导出正确归属 math.cx）→ `bc asm` 往返
     **字节一致** → `run` 往返产物仍退出 0。
   - 回归：`tests/driver_module_bcode_test.cpp`（5 个用例：emit 后字节码路径与
     源码路径一致、往返字节一致、单模块文本无指令行、**真实 v1 容器兼容**、
     循环依赖编译期拒绝）。全量测试 1563 → **1568**。
   - 文档：`docs/bytecode-mnemonics.md` §5 重写（v2 模块表 / v1 兼容读 /
     `.cxs` 模块指令与往返不变式）。
22. **路径语义修正：容器只存源码路径，规范化在加载/执行时重做**
   - **用户澄清**："`.cxs`/`.cxb` 中的路径应该是源码中的路径，路径规范化在 vm 层
     需要重新做一次。"——原先容器/文本里存的是**编译期规范化后的宿主路径**
     （如 `build/tmp/mg/math.cx`），编译期发射的 `IMPORT` 操作数也是 canonical：
     产物把宿主目录布局烧了进去。
   - **改动**：
     1. **编译期发射源码路径**：`IMPORT <源码路径>`（`"./math"`）——取
        `module_t::import_path`（导入它的那条语句里的原文），不再用 canonical。
     2. **运行期重新规范化（VM 层）**：`op_import` 以**当前正在执行的模块**
        （新增 `bytecode_t::module_path`，借用 `module_t::canonical`）为基准调用
        `module_path_resolve` 再查 `vm->modules`。为此 `clux_vm` 现在依赖中立的
        `clux_module` 层（后者只依赖 core，无环）。
     3. **容器只存源码路径 + 导入者下标**：每模块 `import_path`（源码原文）+
        `importer_index`（`0xFFFFFFFF` = 入口），**规范路径不落盘**。
     4. **加载时按导入链重算**：新增 `bcode_graph_resolve_paths(alloc, g, entry_root)`
        ——入口取 `.cxb`/`.cxs` 文件自身的规范化路径，其余模块
        `canonical = module_path_resolve(import_path, 导入者的 canonical)`（不动点
        迭代；不可达即报格式错）。加载器用推导出的键绑定模块并设
        `bc->module_path`，与运行期 op_import **同函数同链** → 两侧 key 必然吻合。
     5. **文本形态**：`MODULE "./math" 1`（源码路径 + 导入者下标），入口仍是裸
        `MODULE`；单模块文本依旧无任何指令行。
   - **收益（已验证）**：`bc emit` 出的 `.cxb` 复制到**另一个目录并改名**（源码不在
     场）仍能 `run` 成功（exit 0）；`bc disasm` 文本成了人类可读的
     `MODULE "./math" 1`。
   - 回归：全量 **1568** 通过；多模块 `emit → disasm → asm` 往返**字节一致**且往返
     产物可执行；`examples/asm` 5 个样本往返字节一致；35 个示例的
     `emit → run .cxb` 全通过；0 告警、0 泄漏。

20. **AST 遍历共享 + `sema_fn_ctx` 配对接口**
   - **新增 `parser/ast_walk.{h,c}`**：把"哪些字段是子节点"这一 AST 结构事实
     收敛为一份实现（`ast_visit_children` 描述直接子节点含兄弟链、
     `ast_walk_preorder` 前序遍历并支持提前终止）。范围限定**表达式与类型
     表达式**——语句层遍历有各自的控制流语义（compiler 的 fid 预扫描、
     sema 的 3b walk），不强行共享。
   - **sema 的三套手写 walker 全部改为"共享遍历 + 谓词"**：
     `type_rhs_uses_var`（76 行）、`type_has_ct_op`（19 行）、
     `type_has_nonliteral_array_bound`（22 行）→ 各自 ~10 行。三者的覆盖面原本
     **互不相同**（前者认 `AST_BINARY`/`AST_CALL`/`AST_CONSTRUCT` 但不认
     `AST_OPTION`；后两者相反；三者都没覆盖 `AST_PTR`/`AST_SLICE_TYPE`/
     `AST_TUPLE`），同一类型槽位在不同查询下会得到不同结论。收敛后不存在
     这种分歧（覆盖面是三者并集，遮蔽检测因此能覆盖 `type T = ?[x]i32` 这类
     此前漏检、要到 CTFE 才以另一种消息报出的情形）。
   - **踩坑记录（设计错误，已修）**：最初把两个谓词合并进**一次固定终止条件**
     的遍历——`[sizeof(a)]i32` 先命中"非字面量边界"而 `STOP`，永远看不到
     `sizeof`，导致 `type_has_ct_op` 误报 false、槽位被 3a 提前解析失败
     （`SizeofAsArrayBound` / `RunFileSizeofAlignofTypeofPasses` 两个测试立刻
     抓到）。改为**请求驱动**的 `want/found` 位掩码：每个谓词都得到完整遍历，
     仅在所求性质命中时终止。
   - **`sema_fn_ctx` 配对接口**：`sema_fn_ctx_enter/leave`（sema.h inline）
     把"进入时整体快照、离开时整体恢复"写成显式配对，两处嵌套 walk
     （`sema_walk_function` / `sema_check_func_literal`）改用之。

**未做的结构性重构**（评估结论）：`sema_t` 环境字段（vm/diag/tokens/arena/
global_scope）与队列字段的进一步分组（访问点数百处、收益有限）；
compiler 的 `prescan_expr`/`prescan_stmt` 因含语句层语义与 fid 动作，未并入
共享遍历（需要时可扩展 `ast_walk` 到语句层再统一）。

> **构建新鲜度提醒**：`build/` 内的可执行文件不会自动随源码更新。曾出现
> "二进制落后源码一整轮里程碑（1444 测试、不认 `import`）却按文档宣称全绿"的情况。
> 任何验证前先 `cmake --build build`（**不要** clean）。
>
> 另注：`Copy-Item` 会保留源文件时间戳，用备份覆盖源码后 ninja 可能认为
> "no work to do" 而不重编——必要时先 `(Get-Item <file>).LastWriteTime = Get-Date`。
23. **VM 所有权重构：消除"孤立于 scope 的 value"**
   - **用户判断**："你之前修复内存泄漏用的 fresh 都是不对的，不应该有孤立于 scope 的
     value，也就是 untrack 只能用于测试；正常代码中 value 的生命周期应该完全由
     scope 管理，在 scope 退出时统一释放。"
   - **改动前的双轨**：`scope_define` 一律 `value_clone`，于是为了避免 own 复合
     类型的重复深拷贝，另开了 `is_fresh` 标记 + `scope_define_owned` +
     调用方 `scope_untrack` 的"接管"轨道（exec.c 的 op_define/op_store 各一处）；
     引擎对象（内建类型值 ×2、opaque、内建函数值 ×3）则用
     `value_make_untracked` 造出**无归属** value，再手工 `value_dispose` + free。
   - **改动**：
     1. `scope_define` 统一为"**未绑定到名字、且已属本作用域**的临时值 → 零拷贝
        采用（adopt）；已命名值（`var b = a` 读到的是 a 的值本体）或借用视图 →
        深拷贝"。变量表只是 name→value 别名，值本体始终在 `scope->owned` 里。
     2. 删除 exec.c 的 fresh/接管分支与**唯一一处 `scope_untrack`**：op_define 统一
        走 `scope_define`，op_store 统一走 `value_assign`（就地深赋值，先释放 dst
        内嵌 own 块）。
     3. 引擎对象改用 `value_make`（自动 track；内建函数值登记进 global scope，随
        VM 存活），**删除全部手工 dispose/free**（7 处 → 0）。
     4. `value_make_untracked` / `scope_untrack` / `scope_define_owned` 在 src 中
        不再有调用者（仅测试夹具使用）；`value_make` 内部仍用前者作为分配原语。
        `is_fresh` 保留但其含义收敛为**纯绑定语义**标记（"该 value 尚未代表某个
        变量"），与生命周期无关。
   - **验证**：1568/1568 测试通过（含 R4 own/数组/闭包/聚合别名
     `AggregateCopyInitIsNotAlias`）；示例 35/35、**0 泄漏**（分配器自检）；
     format 幂等 37/37；0 告警。新增探针（源码级）：`[2]i32`/`Cell`/`[2]Cell` 的
     `var b = a` 后改 b 不影响 a ✓；`c = a`（数组-结构体赋值）后改 c 不影响 a ✓
     （这正是被删除的 op_store 接管分支所覆盖的用例）；comptime var 复合常量 ✓。
   - **收尾清理（同批完成）**：
     1. `is_fresh` → **`is_bound`** 命名与极性收敛（`value_is_bound()` /
        `value_mark_bound()`）：字段语义明确为"该 value 是否已代表某个变量绑定"，
        与生命周期彻底解耦。全部调用点（value.c/scope.c/value.h/测试注释）已更新，
        无 `is_fresh` 残留。
     2. **删除 `scope_define_owned`**（实现 + 头声明）：它已与 `scope_define`
        完全等价且 src/测试均无调用者。
     3. **移除 `src/sema/expr.c` 最后一处手工 `value_dispose`**（upgrade 内建特判）：
        原代码 dispose 一个已登记进 scope 的 shadow 值——只释放 data 而值壳仍留在
        `owned` 列表里（shadow 的 data 为 NULL 才没炸），属"脱离 scope 的生命周期
        操作"。现在只创建替换用的 shadow，原值交由作用域退出统一释放。
     **至此 src 中不存在任何脱离 scope 的 value 生命周期操作**（`value_make_untracked`
     仅作 `value_make` 的分配原语、`scope_untrack` 仅测试夹具使用）。
   - 最终验证：0 告警、**1568/1568** 测试、示例 35/35（0 泄漏）、format 幂等
     37/37、`.cxs` 往返字节一致 5/5、35 个示例 `emit → run .cxb` 全通过。
24. **所有权与读取语义彻底分离：删除绑定标记（`is_bound`）**
   - **动机**：第 23 项之后，"采用 vs 拷贝"仍依赖一个附加标记（原 `is_fresh`、
     第 23 项收尾时改名 `is_bound`）——因为 `op_push`（变量读取）压入的是**变量
     自己的值对象**，`scope_define` 无法仅凭借用与否区分"临时值"与"变量值"。
   - **改动**：
     1. **`op_push` 改为压入借用视图**（`value_make_borrowed(vm, type, data)`）：
        data 指向变量值的业务内存，is_own=false。所有权始终留在变量所属作用域，
        读取只是"看"它一眼；消费点各自 materialize（DEFINE 经 `scope_define`
        深拷贝、赋值经 `value_assign` 就地深拷、RET/传参经 clone）。
        shadow 与无 data 的值直接返回自身（无业务内存可视图）。
     2. **采用判定只剩一个条件**：`scope_define` 见到非借用且属于本作用域的值 →
        只可能是表达式刚创建的临时值 → 零拷贝采用；见到借用视图或其他作用域的
        值 → materialize 深拷贝。**`is_bound` 字段与 `value_is_bound()` /
        `value_mark_bound()` 全部删除**（value.c/value.h/scope.c，零残留）。
   - **为什么安全**（逐点核对）：`move(p)` 压入的仍是变量的**非借用**值 → 被采用
     时所有权仍在同一作用域（owned 不变，仅多一个名字指向它，`strmap_free` 不释放
     值）✓；`var b = a` 压入视图 → 深拷贝 ✓ 无别名；`x = y` 视图 → `value_assign`
     深赋值 ✓；`RET` 视图 → clone 出帧 ✓；`t.*` / `t[i]` / 成员读取本就返回借用
     视图（与本次改动同构）✓；shadow 值路径不变 ✓。
   - **验证**：0 告警、**1568/1568** 测试、示例 35/35（**0 泄漏**）、format 幂等
     37/37、`.cxs` 往返字节一致 5/5、35 个示例 `emit → run .cxb` 全通过。
     至此 **src 中不存在任何脱离作用域的 value 生命周期操作，也不存在任何
     "值是否已绑定"的附加状态**——作用域是唯一的生命周期管理者，借用与否是唯一
     的拷贝判据。
25. **RET 路径审视（结论：RET = copy 正确）+ 修一处 fatal 消费漏判**
   - **审视结论（与用户理解一致）**：返回值一律是**拷贝**——
     `op_ret` 只压 INTERRUPT_RETURN 哨兵，真正的移交在 `func_vcall` 步骤 7：
     临时切到调用方作用域后 `value_clone(result)`，把返回值 materialize 成
     调用方 owned 列表里的独立值（借用视图也在这一步被 materialize ✓）。
     指针是同一语义的特例：拷贝的是指针本身（`fatal *T` 返回即把所有权传给
     调用方）；"不得返回 own 指针"由 sema §4/R1 拦截，因此不存在两个所有者。
     逐项探针（均 exit 0、无泄漏）：返回 struct / 数组 / 枚举 / 局部变量
     （`return p;` 其中 p 为局部聚合）/ fatal 切片 / fatal 指针。
   - **修一处真缺陷**：`walk_return` 里"return fatal 参数 → 标记已消费"的代码
     原先写在 `: ref *T` 的分支**内部**，因此只有 ref 返回才生效；fatal 指针/
     切片返回走不到那段 —— `func id(p: fatal []i32): fatal []i32 { return p; }`
     被误报 `fatal parameter 'p' is not consumed`，而诊断文本自己写的是
     "take ownership with **move() or return it**"。
     修复：把标记提到与返回类型无关的位置，判据与 `sema_walk_function` 末尾的
     未消费检查（`PTR_FATAL || SLICE_FATAL`）统一。
   - **未放宽规则**：`func peek(p: fatal []i32): i32 { return p[0]; }`（只读元素、
     未消费 p）仍然被正确拒绝 ✓。
   - 回归：`tests/driver_m3_test.cpp` 新增
     `FatalSliceParamReturnedIsConsumed`（端到端：返回 fatal 切片后可继续使用）
     与 `FatalSliceParamNotConsumedStillRejected`（负面用例）；全量测试
     1568 → **1570**。
   - 最终验证：0 告警、1570/1570 测试、示例 35/35（0 泄漏）、format 幂等 37/37、
     `.cxs` 往返字节一致 5/5、35 个示例 `emit → run .cxb` 全通过。
26. **收尾两项清理：分配原语改名 + fatal 消费点收敛**
   - **`value_make_untracked` → `value_alloc_shell`**（32 处：src 3、头 2、测试 27）：
     名字直述其职——"分配一个**尚未登记进任何作用域**的 value 壳"。文档明确
     限定**仅供两处**：`value_make` 内部作为分配原语（分配后立即 `scope_track`），
     以及测试夹具手工构造值；正常代码不得直接调用（VM 所有权模型下不存在
     孤立于作用域的 value）。
   - **§4/R1 fatal 消费点收敛为唯一判据入口**（`include/sema/sema.h`）：
     `sema_type_is_fatal(t)` / `sema_mark_fatal_consumed(sym)` /
     `sema_fatal_consumed(sym)`。三处判定（`expr.c` 的 AST_MOVE 接管、
     `stmt.c` 的 walk_return、`sema_walk_function` 末尾的未消费检查）全部改走
     这组接口——判据从三段各自手写 `PTR_FATAL || SLICE_FATAL` 收敛为一处定义
     （第 25 项那个"漏切片分支"的缺陷正是这种重复的产物）。
   - 验证：0 告警、**1570/1570** 测试（含第 25 项新增的 fatal 切片正/负用例）、
     示例 35/35（0 泄漏）、format 幂等 37/37、35 个示例 `emit → run .cxb` 全通过。
27. **CTFE 审查（泛型单态化地基）**
   - 新增文档 `docs/ctfe-review.md`：对 `src/ctfe/**`（ctfe.c 141 / ctfe_call.c 131 /
     ctfe_expr.c 709 行 26 case / ctfe_stmt.c 304 行 14 case）做了逐项审查。
   - **结论：无需改动即可与 2026-10 的所有权重构共存，且已具备单态化的核心能力。**
     核对证据（详见文档 §1/§2）：
     - 所有权：CTFE 中 `value_alloc_shell` / `value_dispose` / `scope_untrack` /
       手工 free **零出现**（本来就走 value_make*/scope_define/value_clone）✓
     - 作用域纪律：`AST_BLOCK` 建/pop scope、for 头变量与 body 同 scope、
       每条 error 分支先 pop 再 return、调用帧显式管理 `current_scope` ✓
     - 返回值移交 = **copy 且顺序正确**：`ctfe_call.c` 先 clone 进 caller scope 再
       pop callee（与 VM 侧 `func_vcall` 步骤 7 语义一致）；嵌套块经
       `ctfe_hoist_return` 逐层上移；错误值经 `ctfe_hoist_error` 上移 ✓
     - 单态化地基：类型值作为一等值**可传参、可返回**（`ctfe_expr.c:84-94`），
       `const`/`volatile`/`typeof`/`sizeof` 均产出类型值 ✓
     - DoS：budget 每表达式/语句递减 + 循环单独检查、max_depth=128 ✓
   - **单态化动工前的准备清单（§3，按优先级）**：
     A. `ctfe_expr.c`/`ctfe_stmt.c` 按 AST kind 抽处理器（照 sema_expr 第 17 项的做法）
        ——泛型会新增 case，现在做是纯搬移、之后做代价高；
     B. 实例化缓存/去重表 + mangling（建议挂在 sema，CTFE 只求值类型实参）；
     C. 实例化层面的递归深度/预算保护（现有 budget/depth 只管求值）；
     D. 实例的 AST 表示定稿（建议：类型槽位替换后的 ast_func_def_t 副本，不新增节点）；
     E. 诊断可定位性（报错需指出泛型定义点 + 实参类型，而非纯字符串）。
28. **M6 FFI 动工：语法定稿 + Phase 2 前端落地**
   - **语法定稿**（用户确认方案 A）：`extern lib "<路径>" as <名>;`——判据是**关键字**，
     路径只是 dlopen 参数，**绝不按 `.so/.dll/.dylib` 后缀判类**；`import` 语义保持
     M5 不变（文档 §1.1 里"import 动态库"的旧形式已标记废弃）。
   - **Phase 2 已实现**：lexer 新增 `extern`/`lib`/`cfunc` 关键字；新 AST 节点
     `AST_EXTERN_LIB`（`include/parser/ast_extern_lib.h`）；`parse_extern_lib`
     （`parse_import.c`）+ `parse_program` 的 `extern` 分支；`fmt` 渲染分支
     （否则会像曾经的 import 一样被静默删除）；sema 对未实现的运行期给出明确
     诊断 `extern lib 'x' is not implemented yet (M6 FFI: dlopen/dlsym path pending)`
     ——**不静默放行**。
   - **发现并修掉一个真实崩溃**：`pass1_names` 的跳过列表只含 `AST_IMPORT`/
     `AST_REEXPORT`，新节点落到通用声明路径 → 访问非法字段 → 0xC0000005。补齐
     跳过点后，含 `extern lib`（且同时有 export/func/var/struct）的程序只报上述
     诊断、不再崩溃。
   - 回归：`tests/driver_module_bcode_test.cpp` 新增 3 个用例（解析后必须报未实现、
     缺 `as` 子句报语法错、`extern` 后非 `lib` 报语法错）。全量 **1570 → 1573**。
   - 验证：0 告警、1573/1573 测试、`clux format` 对该声明幂等且不丢声明（CLI 复核）。
   - **Phase 1 阻塞项**：`third_party/` 目前只有 gtest/icu，**没有 libffi**——
     需要 `git submodule add`（需网络）或改用内置最小 ABI 层；在此之前运行期
     `dlopen/dlsym`、`extern cfunc`、`cfunc` 定义、trampoline/closure 均无法落地。
29. **M6 Phase 2b：cfunc 定义前端落地**
   - **AST**：`ast_func_def_t` 新增 4 个 FFI 字段——`is_cfunc` / `is_extern_cfunc` /
     `is_variadic` / `lib_name`（复用带标志的 func_def，不新增节点种类）。
   - **parser**：`parse_func_like` 关键字参数化（`func` / M6 `cfunc`），新增
     `parse_cfunc_def`（与 func 同构，仅置 `is_cfunc`）+ `parse_program` 的
     `cfunc` 顶层分支。捕获列表对 cfunc 不解析（出现即语法错误，符合"禁止闭包"）。
   - **fmt**：声明级关键字按 `is_cfunc` 渲染为 `cfunc`——否则 `clux format` 会把它
     静默改写成 `func`（语义漂移：C ABI 导出 → 普通函数），与 import/reexport
     漏分支同类问题。
   - **过程教训（两次同类踩坑并修正）**：① `parse_func_like` 原型在
     `ast_func_def.h`、还有 `parse_expr.c` 的函数类型调用点，加参数必须同时改
     （否则 `conflicting types` + `too many arguments`）；② 多次基于 CRLF 的
     字符串替换静默失败（`check_keyword(p,"func")` → `kw` 未生效，以
     `-Wunused-parameter` 暴露）——**改用正则/`edit` 工具**，并以编译告警为信号。
   - 验证（0 告警、1573/1573 测试、示例 35/35 不退化）：以
     `cfunc cb(a: i32, b: u64): i32 { return a; }` 为受检样本——解析 + 类型检查
     通过（实参 `cb(1, 2)` 正确报 i32→u64 不能隐式转换）、`clux format` 输出
     `cfunc cb(a: i32, b: u64): i32 {`（**保留 cfunc 关键字**）且幂等、无体形式
     `cfunc f(): i32;` 报 `expected '{' for function body`（无体=extern 声明，
     属未实现的 Phase 2b 后半）。对照实验：同一签名写成 `func` 与 `cfunc` 行为
     一致（`p: opaque` 这类参数写法两者都报 `expected ')'`——是探针语法问题，
     非 cfunc 差异）。
   - **仍未做（下一步）**：`extern cfunc <库>::<名>(...): 返回;` 需要①
     `parse_func_like` 支持"预解析的限定名"或独立参数/返回解析；② `::` 成员
     解析对 FFI 库别名的分支（新符号种类或限定名注册）；③ §2 类型约束校验
     （禁 option/tag union/slice/闭包、`...` 仅 extern cfunc）与运行期未实现的
     明确诊断。**Phase 1 仍阻塞于 libffi 缺失**（需 submodule / vendor / 最小 ABI 层三选一）。
30. **M6 Phase 1（部分）：动态库加载与符号解析——不需要 libffi 的一半先落地**
   - **关键澄清**：libffi 只服务于**动态调用**（Phase 4）与**回调/闭包**（Phase 5）；
     `dlopen`/`dlsym`（Windows：`LoadLibraryA`/`GetProcAddress`）本身**不依赖 libffi**
     ——因此"库加载 + 符号解析 + 句柄生命周期"可以先独立完成并测试，Phase 1 不再
     整体阻塞于 libffi 缺失。
   - **新增 `include/vm/ffi_lib.h` + `src/vm/ffi_lib.c`**（OS 层薄封装）：`ffi_lib_open` /
     `ffi_lib_sym` / `ffi_lib_close` / `ffi_lib_path`；Windows 走
     `LoadLibraryA`/`GetProcAddress`/`FreeLibrary`，POSIX 走
     `dlopen(RTLD_NOW|RTLD_LOCAL)`/`dlsym`/`dlclose`；错误写入调用方缓冲（含 OS
     错误码 / `dlerror` 文本）。CMake 为 POSIX 加 `${CMAKE_DL_LIBS}`。内存全部经
     `allocator_t`（具名 class：`clux.ffi.lib` / `clux.ffi.path`），泄漏检测可覆盖。
   - **`module_t` 新增 `struct ffi_lib_t *ffi`**：`module_new` 置 NULL，
     `module_destroy` 关闭句柄（`extern lib` 声明的库将挂在此处）。
   - **原则回归测试（用户核心要求）**：`FfiLibTest.FakeSuffixDoesNotImplyLibrary`
     ——名字以 `.dll` 结尾的**纯文本文件必须加载失败**（由操作系统按内容拒绝），
     证明本层没有"看后缀就当动态库"。另有：系统库加载 + 符号解析正例、缺库/缺符号
     报错（错误文本含路径/符号名）、空路径拒绝、`close(NULL)` 安全。
   - 验证：0 告警、**1579/1579** 测试（新增 6 个）、示例 35/35（0 泄漏）、format
     幂等 37/37。
   - **剩余**：① `extern cfunc <库>::<名>` 前端（限定名解析 + `::` 分支 + §2 类型约束
     + 明确诊断）；② 把 `extern lib` 接到 driver（加载库、失败即诊断、句柄入
     `module_t.ffi`）；③ libffi（Phase 4 动态调用 / Phase 5 回调）——仍需 submodule /
     vendor / 最小 ABI 层三选一。
31. **M6：extern lib 真正加载动态库（driver 接入 + 句柄生命周期）**
   - **driver 接线**：`compile_module` 在**lexer pool 释放之前**扫描顶层 AST 的
     `AST_EXTERN_LIB`（path/alias 是 pool 的借出视图，晚了就悬垂）→ 调
     `ffi_lib_open` → 句柄存入 `module_t.ffi`；模块销毁时 `ffi_lib_close`，
     失败路径（诊断/`module_new` 失败）也关闭，无泄漏。
   - **诊断归属调整**：sema 不再对 `extern lib` 报"未实现"，改为**跳过**——加载与
     诊断由 driver 负责（文案直接来自操作系统：`LoadLibrary error 126` /
     `dlerror` 文本）。因此 `extern lib` 只声明不使用时是**无害的**（加载即完事，
     不再误报失败）。
   - **限制（明确诊断）**：暂只支持**每个模块一个** `extern lib`，第二个报
     `only one 'extern lib' per module is supported yet (M6 Phase 3: library table)`。
   - **测试校准**：原用例断言"含 `extern lib` 必失败"在能加载系统库的平台上已
     不成立 → 改为平台感知三例：真系统库加载成功（exit 0，无泄漏）、缺库报错
     （含 OS 文案）、第二个库报错。
   - **验证**：0 告警、**1581/1581**（M6 相关 13 例）、示例 35/35（0 泄漏）、
     format 幂等 37/37；端到端演示：`extern lib "msvcrt.dll" as crt;` 运行成功；
     `nope_xyz` 报 `LoadLibrary error 126`；**名字为 `fake.dll` 的纯文本文件加载
     失败**（按内容判类，非后缀）。
   - **剩余**：① `extern cfunc <库>::<名>` 前端与符号解析；② 库表（多库/别名→句柄
     映射）；③ libffi 或最小 ABI 层（Phase 4 动态调用 / Phase 5 回调）。
32. **M6 Phase 2b：`extern cfunc <库>::<名>(参数): 返回;` 前端落地**
   - **解析**：`parse_func_like` 扩展为 `parse_func_like_ex(p, kind, kw, pre_name, pre_lib)`
     ——`kw == NULL` 表示"关键字已由调用方消费"，`pre_name`/`pre_lib` 非空表示
     "限定名已解析"（extern cfunc 场景）。**保留 3 参数兼容包装**，因此
     `parse_expr.c`（函数类型）与 `parse_func_def` 的调用点**零改动**。
     新增 `parse_extern_cfunc`：`cfunc` → `<lib>` → `::` → `<name>` → 复用全部
     签名解析；`pre_lib` 非空 → 体位置改为吃 `;`，`body = NULL`，
     并置 `is_extern_cfunc` / `lib_name`。
   - **约束**：捕获列表对 cfunc 非法（`cfunc cannot capture`）；`::` 缺失报语法错；
     §2 类型约束与符号解析属 Phase 3/4。
   - **sema**：`extern cfunc`（无体）**明确诊断**
     `extern cfunc is not implemented yet (M6 Phase 3/4: dlsym + call): lib::name`
     ——不让无体定义走函数注册路径（那会越界）。
   - **fmt**：声明渲染三态（`func` / `cfunc` / **`extern cfunc`**）+ `lib::` 前缀 +
     无体时输出 `;`（不再渲染函数体）。实测 `clux format` 输出
     `extern cfunc crt::malloc(size: u64): opaque;` **且幂等**——若漏掉这一步，
     format 会把声明改写成 `cfunc malloc(...)`（静默丢库名）＝数据损坏。
   - **验证**：0 告警、**1584/1584**（新增 3 例：解析后须有明确诊断、缺 `::` 报错、
     捕获列表被拒）、示例 35/35、format 幂等。
   - **教训（新增一条通用规则）**：把某字段改成"合法为 NULL"（此处 `body`）时，
     必须审计其**下游的 NULL 检查**——本次 `body = NULL` 后紧跟的旧守卫
     `if (!body) return "...expected '{' for function body"` 立刻把它当错误，
     症状是"extern 声明明明有分支却仍报缺体"。
   - **仍未做**：① **可变参数 `...`**——lexer 目前把 `.` 当单 token（`'<`/`::`
     才是合成 token），故 `format: str, ...` 仍报 `expected type after '.'`；
     解析器里的 `...` 分支已就位，待 lexer 合成 `...` 即生效；
     ② §2 类型约束校验；③ 库表（别名→句柄）与 `dlsym` 取符号；④ libffi/最小 ABI 层。
33. **M6：lexer 合成 `...`（C 可变参数）**
   - **问题**：`extern cfunc crt::printf(format: str, ...): i32;` 中的 `...` 被词法读成
     三个独立 `.`，报 `expected type after '.' in typed literal`；第 32 项里 parser 的
     `...` 分支因此永不触发（我先前误判为"分支未生效"，实为**词法层没有这个 token**）。
   - **改动**：`lexer_read_symbol` 在**双字符表之前**加三字符识别——`c1=='.' && c2=='.'`
     时消费第二个点，若第三个也是点则消费（产出 3 字符 SYMBOL）；两点但第三点不是
     点则产出 `..` token 交给 parser 报错（lexer 不制造"找不到字符"类误报）。
     与既有 `'<`/`::` 的"复合 SYMBOL token"设计一致。
   - **效果（实测）**：
     - `extern cfunc crt::printf(format: str, ...): i32;` 解析通过 → 报预期的
       `extern cfunc is not implemented yet (M6 Phase 3/4: dlsym + call): crt::printf`；
     - `clux format` 输出**原样** `extern cfunc crt::printf(format: str, ...): i32;`
       （`...` 与 `lib::` 都保留）且幂等；
     - 普通 `.` 代码（结构体字面量 `.P{ .x = 1 }`）不受影响（回归通过）。
   - **测试**：新增 2 例——可变参数 extern cfunc 解析后报未实现诊断（若 `...` 未被词法
     识别，该用例会因语法错而失败）、普通 func 里的 `...` 被拒。
   - 验证：0 告警、**1586/1586**、示例 35/35、format 幂等。
34. **M6：库表（别名 → 句柄）+ 真正的符号解析**
   - `module_t.ffi`（单句柄）→ **`ffi_libs` 库表**（`strmap_t`，别名 → `ffi_lib_t*`，
     懒创建；`module_destroy` 遍历关闭全部）。driver 因此**不再限制"每模块一个
     extern lib"**，重复别名单独诊断。
   - **符号解析落地**：driver 对每个 `extern cfunc <库>::<名>` 调 `ffi_lib_sym`
     （dlsym/GetProcAddress），三种结局各自明确诊断——
     ① 未声明库：`extern cfunc nolib::f uses undeclared library 'nolib' (add ...)`；
     ② 符号不存在：`ffi: cannot resolve symbol 'x' in 'msvcrt.dll' (GetProcAddress error 127)`；
     ③ 解析成功但调用未实现：`extern cfunc crt::malloc resolved, but dynamic calls are
     not implemented yet (M6 Phase 4/5)`。
   - **诊断归属统一**：`extern lib` / `extern cfunc` 一律 **driver 独占**；sema 与
     compiler 都**静默跳过**（`prescan_func` 早退），避免无体定义走函数注册路径
     （否则报 `internal: missing function id`）。
   - **顺序陷阱（值得记）**：解析检查原先放在 sema 之后，而被 sema 的 fatal 诊断
     挡在前面 → **实际不可达**（症状：写了检查却从不触发）。职责改为 driver 独占后
     自然可达。
   - 验证：0 告警、1588/1588（测试按新语义校准：两库合法 / 重复别名 / 缺符号）。

35. **M6 Phase 1/4 起步：最小 ABI 层（真调用 C 函数）**
   - libffi 尚未接入（third_party 无此依赖，用户未选定引入方式）→ 按上一轮说明先做
     **内置最小 ABI 层 v0**：`ffi_call_int(sym, argc, argv, out, err, ecap)`
     ——按**整数/指针 ABI**调用，参数与返回值都是 int64/指针宽度，最多 8 个参数，
     以"按参数个数分派的函数指针强转"实现（x64 MSVC/SysV 下整型/指针传参顺序与 C
     一致）。**浮点与结构体传参在当时明确不支持**（x64 需 XMM 寄存器约定；后续由 libffi 精确描述符补齐，见第 61/63 项），
     不猜着传参。
   - **测试是真调用，不是"能解析符号"**：`malloc(16)` → 写入 `"hello"` →
     `strlen` 返回 **5** → `free` 释放；另有 null 符号与超参数个数的拒绝用例。
   - 验证：0 告警、**1590/1590**、示例 35/35（0 泄漏）。
   - **下一步**：把该原语接到语言层——`lib::fn` 的 sema 符号注册与 `::` 解析分支、
     compiler 生成调用（新 opcode 或复用 CALL 的 FFI 分支）、VM 侧按签名打包
     `int64` 实参并调 `ffi_call_int`；随后才是 §2 类型约束与 cfunc 回调（Phase 5，
     需要闭包式 trampoline，届时 libffi 或手写 asm 二选一）。
36. **M6：FFI 调用点明确诊断（含混错误 → 可执行指引）**
   - **问题**：源码里写 `crt::malloc(16)` 时，因 `extern lib` 别名**不是符号**，
     `::` 解析掉进枚举 variant 路径，报 **`unknown enum type in enum reference`** ——
     完全看不出问题在哪（用户会以为是枚举写法错）。
   - **改动**：
     1. `sema_symbol_kind_t` 新增 **`SEMA_SYM_EXTERN_LIB`**；
     2. `pass1_names` 为每个 `extern lib` **注册别名符号**（照 import 别名样板，
        重复别名报 `duplicate name 'x' for extern lib alias`）；
     3. `expr.c` 的 `::` 分支在模块分支之前插入 FFI 分支：`sym->kind ==
        SEMA_SYM_EXTERN_LIB` → 诊断
        `FFI call 'crt::malloc' is not implemented yet (M6 Phase 4/5: dynamic call via
        the minimal ABI layer)`。
   - **效果**：错误从"看不懂的枚举错误"变成**指明缺哪一步**的指引（并且不静默放行）。
   - 验证：0 告警、**1591/1591**（新增 1 例）、示例 35/35（0 泄漏）、format 幂等 37/37。
   - **仍未做**：`lib::fn` 的语言层接线（sema 建函数对象/分配 fid → compiler 生成调用
     → VM 打包 int64 实参调 `ffi_call_int`）；§2 类型约束校验；cfunc 回调（Phase 5，
     需 trampoline，libffi 或手写 asm 二选一）。
37. **M6：最后一公里规格定稿（避免返工）+ 现状核对**
   - 经 9 轮推进，Phase 2/2b 前端、Phase 1 的加载/符号解析、最小 ABI 层（可**真调用**
     C 函数）均已完成并有测试（见 `docs/m6-design.md` §14.1 的逐项证据表）。
   - 本轮把"最后一公里"（让 `crt::malloc(16)` 在源码里直接可用）的**实现规格**写进
     `docs/m6-design.md` §14.2：sema 建函数对象/分配 fid + §2 类型约束 → compiler 发射
     `BCODE_EXTERN_CALL <str "lib::name"> <u8 argc> <u8 ret_kind>` → VM 从当前模块库表
     取句柄、按签名打包 `int64[]` 调 `ffi_call_int`、按 `ret_kind` 归一化返回值；
     含字节码往返（`.cxs`/`.cxb`）与四类测试的清单。
   - 并把 **libffi 决策点**（§14.3）显式记录：**回调（Phase 5）无法用函数指针强转绕过**
     —— 必须 libffi 或手写 asm；浮点/结构体传参在最小 ABI 层下不可用。该决策影响
     §14.2 第 1 步（是否在 sema 拒绝浮点），因此先定选型再动三层接线，可避免返工。
   - 验证：0 告警、**1591/1591**、示例 35/35（0 泄漏）。
38. **M6：libffi 以 submodule 引入 + `ffi_call` 崩溃诊断（逐条实测）**
   - **submodule 改造**：`third_party/libffi` 由普通 clone 改为 **git submodule**（v3.8.0，
     `12ffd1f9`），`.gitmodules` 与 gitlink 已暂存（未提交）；原目录先移到 `.bak`，验证
     构建通过并确认绝对路径后才删除。拉取：`git submodule update --init --depth 1
     third_party/libffi`。
   - **树外端口**（`third_party/libffi-port/`）：CMake + `fficonfig.h`（Windows x64 静态）+
     `ffi_cfi.h`（空 CFI 宏）+ `preprocess_asm.cmake`（预处理 `win64.S` 并剥 `cfi_*`/`.seh_*`，
     因 clang 的 COFF 汇编器不接受它们）→ 编出 `ffi.lib` 并链进 `clux_vm`。
   - **三个构建坑**（详见 m6-design §14.4）：静态宏是 `FFI_STATIC_BUILD`（3.8 起
     `FFI_BUILDING` 失效）；x64 清单不含 `ffi64.c`（Unix 后端）；`ffi.h` 需 `ffitarget.h`。
   - **崩溃诊断链**（9 步实测，未改仓库代码）：`ffi_prep_cif` 正确（`bytes=32`、
     `flags=12=SINT64`、ABI=WIN64）；`ffi_call` 连**本地 C 函数 + 0 参**都崩 ⇒ 与参数打包、
     DLL 指针无关；剥元数据、补 `X86_64`/`_CET_ENDBR` 均无效；`llvm-nm` 证实链的是
     `ffiw64.c` 的正确实现；`llvm-objdump` 证实汇编序言与上游一致；**SEH `__except` 未触发**
     ⇒ `%rsp` 被写坏 ⇒ C 侧传给汇编的 `stack`（`rcx`）是垃圾。
   - **下一步落点**：反汇编 `ffi_call_int` 在 `call ffi_call_win64` 之前的指令，核对
     `ffiw64.c:160-196` 的 `stack`/`frame` 计算。
   - 验证：0 告警、**1591/1591**、示例 35/35（0 泄漏）；调用路径仍走已验证的最小 ABI 实现，
     浮点用例挂 `CLUX_FFI_TYPED_CALLS` 待启用。
39. **M6：浮点 FFI 调用打通（类型化调用层）+ 两个假设排除**
   - **新增 `ffi_call_f64`**（`include/vm/ffi_lib.h` + `src/vm/ffi_lib.c`）：按参数个数分派
     **类型化**函数指针（`double(double,...)`），x64 下由编译器生成 XMM0-3/XMM0 的正确约定，
     **不需要 libffi 的动态汇编**。实测 `sqrt(9.0)=3.0`、`pow(2,10)=1024.0` ✓。
     至此调用层覆盖**整数/指针/浮点**（`FfiLibTest` 10 例全过）。
   - **libffi win64 汇编路径仍未修好**，本轮又排除两个假设：`_CET_ENDBR` 在生成汇编里
     根本不存在（入口干净 ✓）、调用方加 `-fcf-protection=branch` 仍崩 ✗；另外确认
     `ffi_call_plan_invoke` 只是 `ffi_call` 的包装（prep_cif.c:411），**无法绕开汇编** ✗。
     上游正统 Windows 路径需要 `msvcc.sh` + MASM（`ml64`），本机无此工具链。
     ⇒ 决策：**语言层 FFI 先用类型化调用层实现**，libffi 仅在需要结构体/变参/回调时再处理。
   - 端口新增 `-Wno-language-extension-token`（消掉 libffi `ffitarget.h` 用 `__int64`
     造成的告警，属上游代码风格）。
   - 验证：0 告警、全量测试与示例不退化。
40. **M6：`BCODE_EXTERN_CALL` 操作码 + VM 处理器落地**
   - 操作码**追加在枚举末尾**（`bcode.h`；中途插入会改编号 → 静默误读旧容器，注释里写明约束）；
     指令表登记 `EXTERN_CALL`（`STR + U32 argc + U32 ret_kind`），`.cxs` 往返 5/5 字节一致 ✓。
   - VM 处理器 `op_extern_call`：当前模块库表取句柄 → `ffi_lib_sym` → 按 argc 弹实参
     （int64 宽度）→ `ffi_call_int` → 按 `ret_kind`（VOID/I32/I64/PTR）归一化压栈；
     缺库/缺符号/畸形 `lib::name`/参数过多均有明确诊断；`F64` 返回暂明确报"需类型化调用层"。
   - 验证：0 告警、**1593/1593**、示例 35/35（0 泄漏）。
   - **下一步**：字节码级真调用测试（`strlen` → 5）+ sema/compiler 前端接线（`crt::strlen(s)` 源码可调）。
41. **M6：`EXTERN_CALL` 字节码级真调用验证通过**
   - 新增 `tests/ffi_extern_call_test.cpp`（4 例）：自建模块并把 `msvcrt.dll` 放进
     `module_t.ffi_libs`（别名 `crt`），`bc->module_path` 指向该模块，用
     `exec_drive(vm, bc, 0)` 执行自包含指令序列（`exec_run` 不执行顶层语句，只跑
     类型提升区 + 函数注册段——这一点决定了测试的写法）。
   - **正例**：`PUSH_I64 <宿主 C 字符串指针>` → `EXTERN_CALL "crt::strlen" argc=1 ret=i64`
     → **返回 5** ✓（真的调进了 C 库）。
   - **反例 3 例**：库别名未加载、符号不存在、`lib::name` 畸形 → 各自明确诊断 ✓。
   - 验证：0 告警、**1597/1597**（1593 + 4）、示例 35/35（0 泄漏）。
   - **下一步**：① `ret_kind = F64` 接 `ffi_call_f64`；② sema/compiler 前端接线，使
     `crt::strlen(s)`/`crt::malloc(16)` 在 clux 源码里直接可调。
42. **M6：`EXTERN_CALL` 的 F64 返回接线 + 顺序 bug 修正 + 前端接线方案定稿**
   - **F64 返回已接**：处理器里 `ret_kind = F64` 走 `ffi_call_f64`（v0 规则：F64 返回
     ⇒ 全 double 签名；实参须为 f32/f64，整数实参明确报错而不猜着转换）。
   - 新增源码无关的字节码用例：`PUSH_F64 9.0 → EXTERN_CALL "crt::sqrt" argc=1 ret=F64`
     → **3.0** ✓。`FfiExternCallTest` 现 5/5（`strlen`→5、`sqrt`→3.0 + 3 类反例）。
   - **修掉一个真 bug**：第一版把 F64 分支插在"弹实参循环"**之后** → 整数路径已把栈弹空
     → F64 分支拿到无效值，症状是 `sqrt(9.0)` 报 `bad f64 argument`（不是崩溃、不是错值）。
     教训：同一处理器里两条互斥取参路径必须各自完整、且只有一条被执行。
   - **文档**：`docs/m6-design.md` §14.6 的过时"下一步"已修正，并新增 **§14.7 前端接线方案**
     （带文件/行号锚点：sema 的 `pass1_names` 静默跳过点 → 建 `SEMA_SYM_FUNC` 限定名符号 +
     解析签名；`expr.c` 的 `::` 分支从"报未实现"改为查符号；compiler 调用点改发
     `EXTERN_CALL` 并映射 `ret_kind`；源码级测试与三条踩坑提醒）。
   - 验证：0 告警、**1598/1598**。
43. **M6：§2 C ABI 类型约束判定原语落地**
   - 新增 `ffi_c_abi_reject_reason(const type_t *)`（`include/vm/ffi_lib.h` + `src/vm/ffi_lib.c`）：
     `NULL` = 可用于 C ABI，否则返回人类可读原因。允许整数/浮点/bool/str/opaque/enum/
     struct/cunion/指针（weak·fatal·ref·share）/void（仅返回位置）；拒绝 option、tag union
     （提示改用 cunion）、切片（胖指针）、元组、闭包、类型值、const/volatile 包装。
   - 单测 2 例（真实 VM 类型句柄 + `type_option_intern`/`type_slice_intern` 构造反例）✓。
   - **接入方式**：与 §14.7 前端接线同批——建 `extern cfunc` 符号时对每个已解析的
     参数/返回类型调用本函数，非 NULL 即以该原因报错（`void` 仅允许返回位置）。
   - 验证：0 告警、**1600/1600**。
44. **M6：Phase 5（回调）可行性结论——与调用路径同根受阻**
   - 实测：`ffi_prep_closure_loc` 依赖汇编符号 `ffi_closure_win64`（`ffiw64.c:295` 写
     trampoline）与 `ffi_closure_win64_alt`（`ffiw64.c:288`，`ffi_tramp_set_parms`），
     二者均由 `src/x86/win64.S` 提供（生成物第 130/147 行）。
   - 结论：**回调与 `ffi_call` 崩溃同一根因**；回调需运行期生成可执行 trampoline，C 层面
     无等价物 ⇒ **无法用类型化函数指针绕过**。可行路线只有：① 解决 win64 汇编构建
     （上游正统：`msvcc.sh` + MASM `ml64`，需 VS Build Tools，本机没有）；② 先修好当前
     端口（诊断已收敛到"汇编入口与 C 侧传参都正确，但运行期 `rcx`（新栈区）是垃圾"）。
   - **对里程碑的影响**：语言层调用（§14.7）不依赖回调，可先行；Phase 5 保持待解决状态。
   - 验证：0 告警、**1600/1600**（本轮为文档与调查，未改代码）。
45. **M6：前端接线锚点核实完毕（可直接照改）**
   - 三处代码锚点已核实并写入 `docs/m6-design.md` §14.7 补遗二：
     ① `src/sema/sema.c:779`（extern cfunc 静默跳过点）→ 建 `SEMA_SYM_FUNC` 限定名符号 +
     签名（`type_func_sig`）+ §2 校验（`ffi_c_abi_reject_reason`），样板见 `sema.c:1179`；
     ② `src/sema/expr.c` 的 `SEMA_SYM_EXTERN_LIB` 分支 → 改为 `value_make_shadow(sym->type)`；
     ③ `src/compiler/compile_expr.c:289-300` + `:447` → callee 为 extern cfunc 时改发
     `EXTERN_CALL`（`STR lib::name` + `argc` + `ret_kind` 由返回类型指针比较得出）。
   - 另记：`compile.c:235` 的全局函数名绑定（`LOAD_FUNCTION`+`DEFINE`）**不得**覆盖 extern cfunc
     （无 fid）；当前 `prescan_func` 早退已规避，接线后需复核一次。
   - 验证：0 告警、**1600/1600**（本轮为调查与文档，未改代码）。
46. **M6：§14.7 步骤 ① 完成——sema 注册 extern cfunc 符号 + §2 签名校验**
   - `src/sema/sema.c` 的 `pass1_names`：`extern cfunc` 由"静默跳过"改为
     **注册限定名函数符号** `"<lib>::<name>"`（`SEMA_SYM_FUNC` + `type_func_sig` 签名，
     **不分配 fid**：无函数体、不进运行时函数队列），并对每个参数与返回类型调用
     `ffi_c_abi_reject_reason` 做 **§2 C ABI 校验**（§14.8 的原语由此接入）。
   - **实测诊断**：`extern cfunc crt::g(x: i32): ?i32;` →
     `extern cfunc crt::g: return type: option has no C representation` ✓；
     `extern cfunc crt::f(s: fatal []i32): i32;` → 切片（胖指针）被拒 ✓；
     合法签名（`crt::malloc(size: u64): opaque`）不受影响，仍只报 driver 的
     "resolved, but dynamic calls are not implemented yet" ✓。
   - 新增 2 个 driver 用例（切片参数、option 返回）✓。
   - 验证：0 告警、**1602/1602**（1600 + 2）、示例 35/35（0 泄漏）。
   - **下一步**：§14.7 步骤 ②（`::` 分支返回该符号的 shadow）与步骤 ③（compiler 发射
     `EXTERN_CALL`）——两者需同批完成，之后 `crt::strlen(s)` 即可源码直调。
47. **M6：compiler 侧接线判定已核实（§14.7 补遗三）**
   - 读 `src/compiler/compile_expr.c` 确认：`AST_CALL`（`:439-451`）的 callee 若是
     `AST_SCOPE_REF`（`<库>::<名>`）**就一定是 FFI 调用**——枚举 variant 不可能被调用，
     故该判据充分且廉价，**不需要在 compiler 里另建 extern cfunc 表**；命中后不编译
     callee，只编译实参并发 `EXTERN_CALL`。
   - `ret_kind` 用 `c_resolve_type(c, <return_expr>)`（同文件 `:466` 有用法）解析返回类型，
     再与 `c->vm->type_void/type_i32/type_i64/type_u64/type_f32/type_f64/type_opaque`
     指针比较映射为 0/1/2/2/4/4/3（指针类型 → 3）；类型来源按 `lib::name` 在
     `AST_FUNC_DEF(is_extern_cfunc)` 中查找（可在 prescan 顺手收集）。
   - 验证：0 告警、**1603/1603**（本轮为代码阅读与文档，未改代码）。
48. **M6：为步骤 ③ 预置 AST 槽位（`ast_call_t.is_ffi_call` / `ffi_ret_kind`）**
   - `include/parser/ast_call.h` 新增两个字段（纯数据、零行为变更）：`bool is_ffi_call`
     与 `uint8_t ffi_ret_kind`（0 = void、1 = i32、2 = i64/u64、3 = 指针/opaque、4 = f32/f64）。
   - **契约**（写在字段注释里）：callee 为 `<库>::<名>`（`AST_SCOPE_REF`）时，sema 的
     `::` 分支解析出 extern cfunc 签名，**由 sema 的 AST_CALL 处理点**用该签名填充这两个
     字段；compiler 据此改发 `BCODE_EXTERN_CALL`，**不再**走 callee + `CALL`。用**数值
     类别**而非 `type_t*`，避免类型句柄跨层传递。
   - 这样步骤 ③ 退化为"读字段 + 发指令"，不必在 compiler 里反查声明/解析类型。
   - 验证（强制全量重编）：`build exit=0`、**0 告警**、**1603/1603**。
49. **🎉 M6 打通：`crt::abs(-7)` / `crt::sqrt(9.0)` 在 clux 源码里直接可调**
   - **步骤 ②（sema）完成**：`src/sema/expr.c` 的 `ex_call` 开头拦截 FFI 调用——callee 是
     `AST_SCOPE_REF` 且库别名解析为 `SEMA_SYM_EXTERN_LIB` 时：查限定名函数符号（步骤 ①
     注册 ✓）→ 置 `call->is_ffi_call = true` + 按 `func_type_return(fs->type)` 写
     `ffi_ret_kind`（void→0/i32→1/i64·u64→2/指针·opaque→3/f32·f64→4）→ 返回返回类型的
     shadow 作为表达式类型；未找到声明则报
     `extern call crt::nope: no matching 'extern cfunc' declaration` ✓。
     该实现**只改一处**（`ex_call`），`::` 分支无需改动。
   - **移除过时占位诊断**：driver 里第 6 轮加的
     "extern cfunc X resolved, but dynamic calls are not implemented yet" 已删（调用已接通；
     编译期的 dlsym 校验仍然保留，符号不存在仍报错 ✓）。
   - **端到端实测**：`crt::abs(-7) == 7` ✓、`crt::sqrt(9.0) == 3.0` ✓（浮点走
     `ffi_call_f64`），两例 exit 0；未声明符号的调用被 sema 拒绝 ✓。
   - 新增 2 个源码级 driver 用例；验证：`build exit=0`、**0 告警**、**1605/1605**、
     示例 35/35（0 泄漏）。
   - **剩余**：Phase 5 回调（`ffi_closure`，与 win64 汇编同根受阻，见 §14.9）；§2 校验已接入 ✓。
50. **M6：libffi 原生路径排查收口（排除 11 项假设，转为独立技术债）**
   - 本轮再排除一项假设（**新栈区下方空间不足**：传入指针上移 64B 留足余量仍崩）。连同
     此前各轮，§14.4 现列出**完整排除清单（11 项）**：API 误用、参数打包、DLL 指针、
     汇编逻辑、C 侧传参、元数据剥离、X86_64、_CET_ENDBR、CET/IBT、下方余量、call-plan 绕行。
   - 现存事实：手工搭 frame 直调 ffi_call_win64 仍在 call *fn 之前崩，且 __try/__except
     完全无法触发 ⇒ rsp 被写坏。上游正统 Windows 路径需 msvcc.sh + MASM（本机无）。
   - **决策**：M6 调用能力由**类型化调用层**等价交付（整数/指针/浮点、0/1/多参均实测通过）；
     libffi 原生路径单列为**独立技术债**（需调试器或 MASM 工具链），不再阻塞 M6 功能。
   - 验证：build exit=0、0 告警、1605/1605、示例 35/35（0 泄漏）。
51. **M6：修复指针返回类型 —— `crt::malloc(16)` / `crt::free(p)` 例子打通**
   - 症状：`var p: opaque = crt::malloc(16);` 运行期 `implicit cast: not a widening conversion`。
   - 根因：`op_extern_call` 中 `ret_kind = PTR` 与 `I64` 共用分支，指针返回被压成 i64 类型，
     与 `opaque` 变量不匹配。
   - 修复：`FFI_RET_PTR` 拆为独立分支，压 `opaque` 类型值（`vm->type_opaque`）。复验 exit 0 ✓。
   - 顺带把 3 个"占位诊断时代"的旧断言更新为 `EXPECT_EQ(..., 0)`。
   - 验证：build exit=0、0 告警、**1605/1605**、示例 35/35（0 泄漏）。
52. **🎉 更正并打通：libffi 原生调用路径可用（MASM/ml64），CLUX_FFI_TYPED_CALLS 打开后全绿**
   - **结论更正**：第 50 项"libffi 原生路径受阻"的前提是错的 —— 根因不是"汇编不可用"，
     而是**选错了汇编器/语法变体**：clang 集成汇编器汇编 GAS 变体 `win64.S` 产出的
     `ffi_call_win64` 运行期写坏 rsp。上游正统路径 = MASM `ml64` + Intel 变体
     `win64_intel.S` ✓。
   - 本机 VS Build Tools 位于 `D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`
     （不在 PATH、不在 C 盘 → 早期搜索漏掉）；`ml64`/`cl`/`link`/`lib`/`MSBuild` 齐备 ✓。
   - 端口改为 `vswhere` 自动定位 ml64（可 `-DCLUX_ML64=` 覆盖）+ `clang -E -P` 预处理
     `win64_intel.S` + `ml64 /c`，缺 ml64 时自动回退旧路径 ✓。
   - `ffi_call_typed` 由无条件桩改为 `ffi_prep_cif` + `ffi_call`（libffi 路径）✓；
     打开 `CLUX_FFI_TYPED_CALLS` 后 `LibffiCallsFloatFunctionSqrt` /
     `LibffiCallsTwoDoubleFunctionPow` **全部通过** ✓。
   - 验证：build exit=0、**0 告警**、**1607/1607**、`FfiLibTest` 12/12。
   - 三个踩坑：CMake `$ENV{ProgramFiles(x86)}` 括号破坏解析；预处理必须 `-P`；
     ml64 的 `/Fo<path>` 必须是一个参数（否则 VERBATIM 转义成 `\"`）。
   - **剩余**：Phase 5 回调（符号 `ffi_closure_win64` 已在产物中 ✓）。
53. **M6：闭包（Phase 5 回调）机制验证通过 —— 端口补上 tramp.c**
   - 端口源列表缺 `src/tramp.c`：`closures.c` 在 `FFI_EXEC_TRAMPOLINE_TABLE` 被定义时不提供
     桩，故链接闭包会报 `ffi_tramp_is_supported/_alloc/_get_addr/_free` 未定义。已加入。
   - 独立探针（链接端口 ffi.lib）：`ffi_closure_alloc` + `ffi_prep_closure_loc` 生成的
     trampoline 当 C 函数指针调用 → `20+22 = 42` ✓ `CLOSURE OK` ✓。
   - 验证：build exit=0、0 告警、**1607/1607**。
   - **剩余**：语言层接线（把 `cfunc` 定义包装为闭包处理器，含 §2 参数/返回编组与解释器跳板）。
54. **M6：闭包运行时 API 并入项目（全绿）+ 一条诊断教训；Phase 5 集成收尾清单**
   - 并入 `clux_ffi_closure_create` / `clux_ffi_closure_destroy`（`vm/ffi_lib.h`/`.c`）：
     公共头只用**不透明指针**（避免 `struct _ffi_type` 在"声明早于 ffi.h"时的类型冲突，
     同时改善封装）；box/cif 用 `malloc`/`free`（无裸字节分配 API）；显式 `handle` 所有权；
     必须 `destroy` 否则静默泄漏可执行内存。
   - **教训**：三轮 `conflicting types` 的真因是**注释里写了 `ffi_type**/ffi_type*`**，
     其中 `*/` 提前闭合注释 → 后续文字成为代码。预处理输出剥掉注释 ⇒ 只看 `clang -E`
     的结果永远看不出问题。**回看含注释的源码**才是解法。
   - 验证：build exit=0、**0 告警**、**1607/1607**；独立闭包探针 `20+22 = 42` ✓ `CLOSURE OK`。
   - **剩余（Phase 5 收尾）**：① 把 `cfunc <名>(...): T { body }` 定义包装成闭包（解释器
     跳板 + §2 参数/返回编组），使其可作为 C 回调传出；② 查清 gtest 环境下闭包单测失败原因
     （独立探针同样形状通过）后启用该用例——目前**已停用**以免留红。
55. **M6：闭包单测启用并通过（Phase 5 API 验证完成）**
   - `FfiLibTest.ClosureProducesCallableFunctionPointer` 通过：`clux_ffi_closure_create`
     生成 trampoline → 当 `int(*)(int,int)` 调用 → `20+22 = 42` ✓（诊断输出
     `create=1 err= handle=… code=0x…` ✓）。
   - 此前该用例失败的原因是它仍按**旧的 `struct _ffi_type **` 签名**调用；API 改为
     **不透明指针**后（§14.14）即通过 ✓。
   - 验证：build exit=0、0 告警、**1608/1608**（1607 + 本用例）、示例 35/35（0 泄漏）。
56. **🎉 M6 Phase 5 端到端打通：clux 函数被 **C 侧**（含 libc `qsort`）当回调调用**
    - 新增 `tests/ffi_callback_e2e_test.cpp`：不再只验证"闭包机制可用"，而是
      **用 VM 里的 clux 函数值驱动它**——`clux_ffi_closure_create` 的 handler 内按
      签名把 C 实参编组成 clux 值 → `value_call(vm, fn, args, argc)` → 按返回类型回写。
      这正是 §14.15 记的"唯一未完成项"的机制内核。
    - **测试自建"编译并保留 VM"的流水线**：`driver_run_file` 跑完即销毁 VM，
      `driver_compile_to_bytecode` 是 driver 内的 `static`，而 `compile_module`
      是**公开入口**（`include/driver/compile_module.h`）——因此按 driver 同一顺序手工
      组装：`create_allocator` → `arena_new_default` → `diag_buf_new` → `vm_new` →
      `module_path_normalize` → `compile_module`。函数值从
      `mod->global_scope` 用 `scope_lookup` 取（`exec_run` 只跑注册段，
      函数值由此登记），再用既有的 `value_call` 调用。
    - 6 个用例：① 本 TU 的 C 函数按 C ABI 调 clux `add`（20+22=42，重复调用可重入）；
      ② **libc `qsort`** 用 clux 比较函数排序（闭包类型 `(ptr,ptr)->i32`，handler 内
      解引用元素地址再调 clux `cmp(a,b){return a-b;}`，断言数组升序且比较次数 ≥ n-1）；
      ③ clux 回调内部**再回调进 C**（`crt::abs`）——Phase 3 × Phase 5 复合；
      ④⑤ 语言级（见第 57 项）；⑥ 非 C ABI 签名被拒。
    - **踩坑（API 生命周期契约）**：`ffi_prep_cif` **借用** `arg_types` 数组
      （`cif->arg_types` 指向它，C 侧调用时才读取），因此**必须复制一份**放进闭包 box、
      随闭包释放。首版直接把调用方的数组交给 libffi：宿主级用例侥幸通过（数组是
      测试函数的局部变量，调用发生在同一函数内），语言级用例必崩
      （`cfunc_ptr` 的 arg_types 在**已被弹出的解释器栈帧**里）——症状是
      `0xC0000005`。已在 `clux_ffi_closure_create` 内复制并把契约写进注释。
      教训：把"调用方提供描述符数组"写进 API 时，要明确**谁负责它的生命周期**；
      第三方库借用调用方内存是很常见的隐性契约。
    - 验证：build exit=0、**0 告警**、`FfiCallbackE2E` 6/6。
57. **M6：语言层接线——`cfunc_ptr(f)` 让 **clux 源码**产出 C 函数指针（Phase 5 收口）**
    - 新增内建 `cfunc_ptr(f) -> opaque`（`src/vm/ffi_cfunc.c` + `vm.c` 注册 +
      `sema.c` 符号预注册，与 `len`/`upgrade` 同一套路：variadic + 返回类型占位，
      参数在 cfunc 内部自验）：
      `cfunc cmp(a: i32, b: i32): i32 { ... }` + `var p: opaque = cfunc_ptr(cmp);`
      → p 即可交给 C 的函数指针。宿主只负责**调用**，不再参与闭包搭建。
    - **§2 校验 + 可编组子集**：先过 `ffi_c_abi_reject_reason`（拒绝 option/切片/闭包…），
      再限定到本层能编组的类型（整数/bool/f32/f64/opaque）；§2 允许但本层尚未编组的
      （str/enum/struct/cunion/own·ref·fatal·share 指针）给出**明确原因**而不是猜着传参。
    - **所有权模型**：闭包句柄/上下文/trampoline 地址挂在 **`func_t`** 上
      （`ffi_closure` / `ffi_closure_ctx` / `ffi_closure_code`），随 `vm->functions`
      在 `func_destroy` 释放——与 `closure_scope` 同一"函数对象是 VM 生命周期对象"模型。
      因此重复 `cfunc_ptr(f)` 复用同一 trampoline（测试断言指针相等），
      且 libffi 的可执行内存不会静默泄漏（`delete_allocator` 也不漏报）。
    - **端到端实测**：语言级用例把 `cfunc_ptr` 产出的指针交给 ① 本 TU 的 C 函数
      （`add` → 42、`-8+3` → -5）② **libc `qsort`**（闭包体内自增 clux 全局计数，
      C 侧跑完从 VM 读回 ≥ 4，证明 C 库真的调用了 clux 闭包，且重入下模块全局可写）。
    - **教训（qsort 不稳定）**：语言级用例的比较函数恒返回 0，首版断言"顺序不变"——
      错了：C 标准**不要求 `qsort` 稳定**，等值元素的重排是实现自由（MSVC 的快排就会
      重排，实测 5 个元素 10 次比较后顺序确实变了）。改为断言**元素集合不变**
      + 闭包内自增的 clux 全局计数 ≥ n-1（真正证明"C 库调到了 clux"）。
    - 与 `docs/m6-design.md` §3.2 设计稿的差异已记入 §14.16：设计稿用 `ffi_func_t` +
      定义时立即建闭包、clux 侧调用也经 `ffi_call` 走同一指针；现实现把闭包**惰性**
      建在既有 `func_t` 上（不改函数对象布局、不改 clux 调用路径，少一次 ABI 往返）。
      未做：FFI 实参位置的**隐式**转换（现在要显式写 `cfunc_ptr(f)`）；
      闭包对 str/enum/struct/指针类的编组。
    - **踩坑复现**：新文件的头注释里写了 `/* ... */`（块注释内嵌注释）→ `*/` **提前
      闭合注释**，后续中文正文变成代码（`-Wcomment` + 一串 unicode 错误）。
      与第 54 项是同一坑——规则才刚写下就又犯了一次：**C 块注释里永远不要出现 `*/`**
      （要写示例代码就用 `//`，或把 `* /` 分开写）。
    - 验证：build exit=0、**0 告警**、**1614/1614**（1608 + 6）、示例 35/35（0 泄漏）、
      format 幂等 37/37；`cfunc_ptr` 可经 `clux run` 直接使用（负例：`?i32` 参数
      在 `cfunc_ptr` 处报 option 无 C 表示）。
58. **🎉 M6 收官：纯 clux 程序用 libc `qsort` 排序 + 顺带修掉 `clux format` 丢弃 M3 语法**
    - **纯 clux 端到端**（不写一行宿主代码）：`examples/ffi_callback/ffi_callback.cx`
      ```c
      extern cfunc crt::qsort(base: opaque, n: u64, size: u64, cmp: opaque): void;
      cfunc cmp(a: opaque, b: opaque): i32 {
          var x: i32 = (a as ref *i32).*;   // 等价于 C 的 *(const int*)a
          var y: i32 = (b as ref *i32).*;
          ...
      }
      crt::qsort(arr.&, 5, 4, cfunc_ptr(cmp));
      ```
      实测 `{4,2,5,1,3}` → `{1,2,3,4,5}`、比较 10 次、exit 0 ✓。
      这一条把 M6 的三个方向串成闭环：**clux 定义 → 取 C 函数指针 → 交给 C 库 →
      C 库回调 clux**。同时纠正上一轮的判断：clux **有**指针解引用
      （`.&` 取址 / `.*` 解引用 / `as ref *T`），回调里能真正取到元素值。
    - **顺带修掉一个真实的数据损坏缺陷**：`clux format` 会**静默吃掉** M3 语法——
      `var x: i32 = (a as ref *i32).*;` 输出成 `var x: i32 = ;`，
      `crt::qsort(arr.&, ...)` 输出成 `crt::qsort(, ...)`。根因：`src/parser/fmt.c`
      的 `render_expr` 缺 `AST_ADDR`/`AST_DEREF`/`AST_MOVE`/`AST_PTR`/`AST_NEW`/
      `AST_SCOPE_ANNOT` 六个分支，全落 `default: break`（与"漏 fmt 分支 = 静默改写
      `extern lib`/`cfunc` 关键字"同一类）。修法：补齐六个分支；**后缀族必须保住括号**
      （`.&`/`.*` 绑定力 26，低优先级操作数要 `render_expr_paren(..., 26)`，
      否则 `(a as ref *i32).*` → `a as ref *i32.*` 会被重解析成
      `a as ref *(i32.*)`，语义改变）。新增 3 个 fmt 用例。
    - 验证（强制全量重编）：`build exit=0`、**0 告警**、**1618/1618**（1608 + 7 FFI +
      3 fmt）、示例 **36/36**（0 泄漏）、format 幂等 **38/38**、`.cxs` 往返字节一致。
    - **⚠️ 本轮发现的新缺口（下一项即修掉）**：`bcode_graph` 容器（`.cxb`/`.cxs` v2）
      不携带 `extern lib` 声明 → FFI 程序 `bc emit` 后再 `run .cxb` 会在 `EXTERN_CALL`
      处报 `no loaded library 'crt' in this module`（源码直跑正常）。与 M5 修过的
      "`.cxb` 不携带依赖模块"同类 → 见第 59 项。
59. **🎉 M6 真正闭环：FFI 声明随容器落盘——FFI 程序的 `emit → run .cxb` 打通**
    - **缺陷（上一项记下的缺口）**：`module_t.ffi_libs` 只由**编译期**扫描 AST 建立，
      字节码里只剩 `EXTERN_CALL "crt::sym"` → 加载方不知道该 dlopen 什么，
      `clux bc emit f.cx -o f.cxb && clux run f.cxb` 报
      `exec: extern call: no loaded library 'crt' in this module`。
      与 M5 修过的"`.cxb` 不携带依赖模块"是同一类问题：**源码路径通、字节码路径断**。
    - **修法**（容器 v2 → v3）：
      1. `bcode_module_image_t` 增加 `ffi_aliases`/`ffi_paths`/`ffi_count`（**每模块**一段，
         依赖模块的声明归它自己——多模块下 section 归属必须正确）；
      2. 二进制段：导出表之后 `u32 ffi_count` + `[alias_len][alias][path_len][path] × n`
         （`gb_module_section` 是唯一实现，vm 路径与图路径共用）；
         **顺带修掉一个陷阱**：反序列化模块循环里的 `if (ecount == 0) continue;`
         会跳过 FFI 段 → 改为 `if (ecount > 0) { ... }` 后继续读；
      3. `.cxs` 文本：`externlib "<路径>" as <别名>` 指令（**有声明才写**，
         非 FFI 产物的 `.cxs` 因此与旧格式逐字一致，旧手工样本/往返测试不受影响）；
      4. 加载侧 `driver_execute_graph`：模块 `vm_module_bind` 之后按表
         `ffi_lib_open` 填 `mod->ffi_libs`（失败路径由 `vm_destroy → module_destroy`
         统一关闭已开句柄）；
      5. 兼容：版本 1/2/3 全可读，v1/v2 按"无 FFI 声明"处理。
    - **实测**：`examples/ffi_callback/ffi_callback.cx` `emit → run .cxb` ✓
      （`{4,2,5,1,3}` → `{1,2,3,4,5}`，回调 10 次——同时证明 EXTERN_CALL 与
      `cfunc_ptr` 闭包在容器加载后都可用）；`.cxb → .cxs`（首行
      `externlib "msvcrt.dll" as crt`）`→ .cxb` **逐字节一致**且可运行 ✓；
      FFI 声明在依赖模块里时 `externlib` 行落在 `MODULE "./ffi"` 段内 ✓。
    - 新增 4 个用例（`DriverM6.FfiProgramRunsFromCxb` /
      `FfiProgramRunsFromCxs` / `FfiProgramCxbCxsRoundTripIsByteIdentical` /
      `FfiInDependencyModuleRunsFromCxb`）。
    - 验证（强制全量重编）：`build exit=0`、**0 告警**、**1622/1622**、
      示例 36/36（0 泄漏）且 **36/36 `emit → run .cxb` 全通过**、format 幂等 38/38、
      `.cxs` 往返字节一致。
60. **M6：`extern var` —— 动态库导出的**全局数据符号**（读写）**
    - 此前 FFI 只覆盖库里的**函数**（`extern cfunc`）；库导出的**全局变量**没法
      访问。现在补齐：
      ```c
      extern lib "msvcrt.dll" as crt;
      extern var crt::_fmode: i32;
      crt::_fmode = 32768;        // 左值：写回库里的那块内存
      var old: i32 = crt::_fmode; // 读
      crt::_fmode += 1;           // 复合赋值 → LOAD → 运算 → STORE
      ```
      数据符号的"符号地址"就是变量地址（dlsym 直接给出），因此读写**直接作用于库
      里的内存**，没有本地副本；`extern var` 也因此是**左值**（与只读的 comptime
      常量、只能调用的 `extern cfunc` 形成对照）。
    - **类型必须显式给出**且属于可编组子集（`i8/i16/i32/i64`、`u8/u16/u32/u64`、
      `bool`、`f32/f64`、`opaque`）：宽度决定读/写多少字节，**必须与库里符号的真实
      类型一致**（读多/写多会踩到相邻符号）。§2 允许但布局不定的类型
      （`str`/`struct`/`enum`/切片/指针所有权）在这里**明确报错**。
    - **实现链路**（与 `extern cfunc` 对称）：AST（`ast_var_def_t.is_extern_var` +
      `lib_name`；`ast_scope_ref_t.is_ffi_var` + `ffi_var_kind`，与
      `ast_call_t.is_ffi_call` 同一契约）→ parser（`parse_extern_var`；赋值左值放行
      `AST_SCOPE_REF`，**语义判定交给 sema**）→ fmt（原样渲染，否则会被静默改写成普通
      `var … = …`）→ sema（限定名符号 `SEMA_SYM_EXTERN_VAR` + `::` 读取分支 +
      `shadow_assign_extern_var` 写入分支）→ compiler（`EXTERN_LOAD`/`EXTERN_STORE`）
      → VM（`op_extern_load`/`op_extern_store`，与 `op_extern_call` 共用
      `ffi_resolve_symbol`）→ driver（编译期 dlsym 校验）。
    - **类型码**：`0..3`=i8/i16/i32/i64、`4..7`=u8/u16/u32/u64、`8/9`=f32/f64、
      `10`=opaque（指针宽度）、`11`=bool；编码 `ffi_data_kind_of` 与解码
      `ffi_data_type_of` **成对实现在 `vm/ffi_lib.c`**（避免两处清单漂移）。
    - **两个真踩到的坑**：
      ① **bool 不能复用 u8 的类型码**——宽度一样，但 `EXTERN_LOAD` 会构造出 u8 值，
      `crt::flag != true` 于是报 `run: type does not support implicit cast`（类型不对
      而非值不对）；`bool` 单独一码后修好。
      ② **`FreeLibrary` 会卸载模块、导出变量复位**——测试夹具原先每次读取都
      open/close，引用计数归零 → DLL 卸载 → 下次 `LoadLibrary` 拿到**全新副本**
      （初值），于是"clux 写进去的值"在宿主侧读不到（用例内部读回却是对的，极易
      误判成实现 bug）。修法：夹具持有长期句柄（真实程序里库表由 `module_t` 持有到
      模块销毁）。**教训**：观察外部库的状态前，先确认模块仍驻留。
      ③ 顺带修掉 driver 里**同一符号错误被打印两次**（重复的 `diag_error`）。
    - **测试素材**：系统库里没有这些宽度的可写数据符号 → 新增
      `tests/ffi_testdata.c` + CMake `clux_ffi_testdata` SHARED 目标（12 个导出变量
      覆盖全部宽度，产物与 `clux_test.exe` 同目录 → 裸文件名即可打开）。
    - **测试**：`tests/ffi_extern_var_test.cpp` 13 例——字节码级（i32/u64/f64 读取、
      指针宽度→opaque 且等于宿主 dlsym 地址、写入后宿主独立复核、bool 类型、4 类
      诊断）；源码级（全宽度读取、写入+还原、复合赋值、opaque 往返、
      `.cxb`/`.cxs` 往返且反汇编保留 `EXTERN_LOAD "…" <kind>`、6 类诊断）。
      新增 `examples/ffi_extern_var/ffi_extern_var.cx`（读写真实系统库的 `_fmode`
      并还原、`_osplatform`、`_pgmptr`），源码运行与 `emit → run .cxb` 均 exit 0。
    - 验证：`build exit=0`、**0 告警**、**1635/1635**（1622 + 13）、示例 **37/37**
      （0 泄漏）且 **37/37 `emit → run .cxb` 全通过**、format 幂等 39/39。
61. **M6：`extern lib "*"`（当前进程自身）+ 顺带修掉 FFI 返回类型与实参校验两个真缺陷**
    - **动机（用户指出）**：libc 这类库**进程启动时就已经链接进来**，不该再
      `dlopen` 一次。把 `extern lib` 的**路径写成 `"*"`** 即声明"当前进程"伪库：
      ```c
      extern lib "*" as c;
      extern func  c::abs(x: i32): i32;   // extern func 与 extern cfunc 等价
      extern cfunc c::strlen(s: str): u64;
      extern var   c::_osplatform: u32;
      ```
    - **实现走"伪库句柄"而不是新语法**：`ffi_lib_open(alloc, "*")` 返回
      `is_process=true`、**无 OS 句柄**的 `ffi_lib_t`；`ffi_lib_sym` 见该标志即转交
      `ffi_sym_current_process`。于是 sema/compiler/容器**零特殊处理**——别名照常
      起、`<别名>::<名>` 照常引用，`.cxb` 里照常写 `externlib "*" as c`，加载侧
      `ffi_lib_open("*")` 就复原；`ffi_lib_close` 对无句柄伪库天然安全。
    - **解析语义 = `dlsym(RTLD_DEFAULT, name)`**（POSIX 直接调用）。Windows 没有
      RTLD_DEFAULT → "先主模块、再遍历已加载模块"（`K32EnumProcessModules` /
      `EnumProcessModules` 动态取，不加链接依赖）。**只查主模块查不到 `abs`/`strlen`**
      （它们在 CRT 模块里）——这一点是实测确认的。
    - **顺带修掉缺陷 ①：返回类型不精确**。`ret_kind` 原只有 5 个值，`u64` 返回被造成
      **i64** 值，`if (c::strlen(s) != 5u64)` 于是在运行期报
      `implicit cast: not a widening conversion`（症状在比较点、根因在调用层）。
      修法：补成**逐类型**编码（`BCODE_FFI_RET_*`：0..4 保持 v0 原义以兼容旧容器，
      新增 u32/u64/i8/i16/u8/u16/bool），VM 按码造出与声明一致的类型；`f32` 返回
      在**声明处**明确拒绝（浮点通路是全 double 签名，f32 会按 double 读错 XMM0）。
    - **顺带修掉缺陷 ②：FFI 实参完全不校验**。此前 `c::abs(1.5)` 会把 double 塞进
      整数寄存器而毫无提示。现在在调用点按"调用层实际能做的事"校验（§14.5 只有
      int64 槽与全 double 槽两条通路）：数量；整数族 ⇄ 整数族（同 int64 槽，语义同
      C 隐式转换——所以 `malloc(16)` 的 i32 字面量喂 u64 形参仍合法）；指针 ⇄
      指针/opaque/切片/str；`str` ⇄ `str`；浮点只在浮点签名里；浮点变参拒绝。
    - **测试**：`tests/ffi_process_test.cpp` 10 例（字节码级伪库句柄 + `c::strlen`
      返回 **u64 类型** + `EXTERN_LOAD "c::_fmode"`；源码级完整程序、`.cxb`/`.cxs`
      往返（`.cxs` 保留 `externlib "*" as c`、逐字节一致）、`extern func` ≡
      `extern cfunc` 且 fmt 归一化、数据符号写入宿主可见；缺陷回归 5 类诊断）。
      新增 `examples/ffi_process/ffi_process.cx`（只用 `*` 调 abs/strlen 并读
      `_osplatform`）。
    - 验证（强制全量重编）：`build exit=0`、**0 告警**、**1645/1645**、
      示例 **38/38**（0 泄漏）且 38/38 `emit → run .cxb` 全通过、format 幂等 40/40。
62. **M6：删除内建 printf，改用 libc 的原生 printf（并补齐 C 变参与精确类型调用）**
    - **动机**：M1 把 C 的 printf 硬编码成内建（`builtin_printf.c`：自己解析格式串、
      自己转发给 `printf(spec, ...)`），还带一个 `%v`"打印任意值"的方言扩展
      （`value_dump` 实现）。有了 M6 的 FFI 之后应当让它回到本来的身份。
    - **语言层：零预置、显式声明**（用户选择）：
      ```c
      extern lib "*" as libc;                            // 当前进程自身
      extern func libc::printf(format: str, ...): void;  // 只用输出 → 声明 void
      ```
      - 声明 `void` 的原因：clux 表达式语句不许丢弃非 void 结果
        （`use '_ = expr' to discard`）；需要计数时声明 `i32` + `var n = ...`。
      - **别名用 `libc` 而不是 `c`**：`::` 左侧先按作用域解析，**局部变量会遮蔽库别名**
        （`var c: Color = ...;` 之后 `c::printf` 被当成对变量的成员访问）——这个坑在
        改示例时立刻踩到（enums/closure/const_volatile/sizeof 四个示例都有变量 `c`）。
      - `%v` 随之消失（格式符就是 C 的原生集合）；`value_dump` 保留为独立工具
        （当前仅单测使用）。内建只剩 `upgrade`/`len`/`cfunc_ptr`。
    - **引擎层补齐两件事**（否则真 printf 跑不起来）：
      ① **C 变参**：新增 `ffi_call_typed_items`（`nfixed==argc` → `ffi_prep_cif`，
      `nfixed<argc` → `ffi_prep_cif_var`）；"是否变参 + 固定参数个数"打包进
      `EXTERN_CALL` 的 **ret_kind 操作数高位**（`BCODE_FFI_RET_PACK`）——不新增操作数，
      旧产物高位恒 0，天然读作"非变参" ✓。
      ② **C 默认实参提升**：变参位置 `f32→double`、窄整型/bool→`int`（写进临时槽）；
      定参不需要提升——`value->data` 与 C **同构**，直接把 `value_data(v)` 交给 libffi。
    - **顺带把类型映射补成完整 1:1**（用户："基础数据类型可以完整的和 C 1:1 转换"）：
      统一用 libffi 精确描述符（`ffi_data_ffi_type`），**f32 参数/返回值都支持**、
      混合整数/浮点签名也支持（删掉"全 double 签名"限制与 f32 返回禁令）；
      `BCODE_FFI_RET_*` 补到逐类型；实参描述符按值类型取（含 `enum` → 底层整数、
      const/volatile 先剥限定符）；sema 校验即"能否共用同一描述符"（整数族⇄整数族、
      浮点必须同型、指针族⇄指针族、str⇄str；结构体按值见第 63 项）。
    - **两个真踩到的坑**：
      ① **libffi 的 `ret_type` 不能为 NULL**（会直接解引用崩溃）：调用层用
      "NULL = void"约定并在内部补 `ffi_type_void`。
      ② **FFI 实参求值必须经真正的 AST 槽**：最初 `sema_expr(sema, &a, ...)` 传的是
      局部副本，而 comptime 折叠是"就地替换节点"，写在副本上就丢了 → 编译器仍编译
      未折叠的调用，运行期 `exec: undefined variable`。改成沿 `&call->args` /
      `&(*slot)->next` 遍历。
    - **影响面**：删除 `src/vm/builtin_printf.c` + `vm_register_printf` + sema 预注册
      （内建 id 顺移：upgrade=0/len=1/cfunc_ptr=2）；示例 36 文件、测试 10 文件改写；
      测试统一经 `test_common.h::clux_with_printf_prelude()` 在写文件时补声明；
      两个"内建函数值"用例改用 `len`。
    - 验证：`build exit=0`、**0 告警**、**1645/1645**、示例 **38/38**（0 泄漏）
      且 38/38 `emit → run .cxb` 全通过、format 幂等 **40/40**；
      原生 printf 实测 `%d %s %.2f %c %x %f %llu`、0/1/2/5 变参、`u64`/`i8` 提升、
      `f32` 返回（`sqrtf`）全部正确。
63. **M6：聚合类型完备（struct / cunion / enum / char\*）+ 修掉 FFI 签名的 pass 顺序缺陷**
    - **起因（用户提问）**："M6 彻底完备了吗，兼容数据类型包括结构体都支持了吗"。探针实测：
      标量/指针/opaque 早已支持，但 `str` 只能传参不能返回、**enum 与 struct 连声明都
      用不了**（报 `missing type`）、`extern var` 只认标量/指针、回调只编组标量。
    - **先修缺陷**：`missing type` 不是"没实现"而是 pass 顺序问题——FFI 签名校验跑在
      `pass1_names`，那时程序自定义类型只是登记未激活 → `resolve` 得 NULL。连
      "传结构体指针"（`ref *P`）都被挡住。修法：`pass1_names` 只登记类型已就绪的；
      **新增 `pass1c_extern_abi`**（`pass1b_types` 之后）统一 resolve + §2 校验 + 补登记；
      未知类型的诊断改成 `type could not be resolved`。
    - **类型怎么跨层传**：指令操作数个数是冻结的（旧 `.cxb` 兼容），但聚合类型的宽度/
      布局要运行期查表 → 复用 `CONSTRUCT` 的机制：**compiler 在指令前压一个"类型值"**，
      运行期 pop 出来当类型用（新码 `BCODE_FFI_RET_TYPED` / `BCODE_FFI_DATA_TYPED`；
      `str` 各有独立码；nfixed 从 bits16..31 挪到 bits9..15 给类型值腾位，旧产物这些位
      恒 0 → 语义不变）。
    - **描述符构造 `ffi_type_for`（递归）**：struct 按字段递归建
      `ffi_type{FFI_TYPE_STRUCT}`；**cunion 等价建模成"size/align 个整数成员"**
      （libffi 没有 union；union 的 size 必是 align 整数倍，故 size/align 与 ABI 分类
      完全一致）；enum → 底层整数。新建的描述符 malloc/`ffi_type_release`（闭包把
      它们挂到 ctx 的 dispose 上，活到闭包销毁——libffi 的 cif 持指针）。
    - **实参编组零特判**：clux 的 `value->data` 与 C 同构（编译器保证平凡布局），
      `ffi_arg_desc` 直接把 `value_data(v)` 交给 libffi（结构体就是那块 C 布局字节）；
      回调方向同理按 size 拷贝。
    - **sema 类型可接受性**：struct/cunion 要求**同型**；变参允许聚合类型（C 对聚合不做
      默认提升）；返回允许 `str`/结构体/enum/指针族；`extern var` 允许
      `str`/struct/cunion/enum；`FFI_MAX_ARGS` 8 → **16**。
    - **测试**：`tests/ffi_testdata.c` 增补 8B(INTEGER)/16B(SSE)/24B(MEMORY) 结构体、
      嵌套结构体、enum、`char*`、struct 回调、变参 struct 等导出；新增
      `tests/ffi_struct_test.cpp` **11 例**。判定方式：clux 程序把检查结果写进库里的
      `td_i32`，宿主侧读回断言——**`driver_run_file` 的返回值只表示"编译并运行成功"，
      不代表 main 的返回值**（这一点此前被误用，值得记一笔）。
    - 验证：`build exit=0`、**0 告警**、**1656/1656**、示例 **39/39**（0 泄漏）
      且 39/39 `emit → run .cxb` 全通过、format 幂等 **41/41**。
      新增 `examples/ffi_struct/ffi_struct.cx`：用 libc 真实的 `div` 演示按值返回
      结构体、`getenv` 演示 char\* 返回（结构体按值**传参**的可移植库函数不存在，
      那条路径由测试库覆盖）。
65. **`usize`：C 的 `size_t`（随架构变化）**（第 77 项已把它从"别名"改成独立类型）
    - **动机（用户指出）**："之前把 size 大小统一成 u64 了"——`len`/`sizeof`/`alignof`
      一律返回 u64，在 32 位平台上宽度是错的；FFI 里声明 C 的 `size_t` 形参也只能
      写死 u64。
    - **设计：不是新类型，而是别名**（与 C 的 `typedef` 同义）。`vm_type_usize(vm)`
      按指针宽度返回平台类型：8 → `u64`，4 → `u32`。这样：
      * `usize` 与底层类型**完全同一**（同一个 `type_t`）→ 可互相赋值/比较，
        与 `u64` 之间没有隐式转换障碍（`len(s) != 5u64` 这类既有代码不受影响）；
      * FFI 描述符复用同一张表 → `extern cfunc libc::malloc(n: usize)` 就是
        C 的 `size_t`；
      * 名字注册成**第二个绑定**（`scope_define(..., "usize", <平台类型>)`），
        不占新 type id、不动类型系统。
    - **覆盖面**：`len(...)`（shadow + 运行时值 + sema 注册的签名）、`sizeof`/`alignof`
      （sema 折叠 + CTFE 折叠出的常量类型）、字面量后缀 `5usize`（值域 `SIZE_MAX`，
      解析时**归一化**成平台宽度名 u64/u32，编译器据此选 `PUSH_U*` 指令）、
      变量/结构体字段/参数/返回、FFI 形参与返回。
    - **测试**：新增 `tests/usize_test.cpp` 5 例——VM 层断言 `type_lookup(vm,"usize")`
      与 `vm_type_usize(vm)`/`vm->type_u64` 同一且 `len` 返回它；源码层用测试库的
      `td_i32` 收判定值，覆盖 len/sizeof/alignof、`usize`⇄`u64` 互换、
      字段/参数/返回、`sizeof(Buf) == sizeof(usize) * 2usize`、
      FFI（`malloc`/`memset`/`free`/`strlen` 的 size_t 形参与返回）。
      `array_test`/`array_bcode_test` 的 len 类型断言改用 `vm_type_usize`。
    - **文档**：language-spec §2.1/§2.3/§3.1（关键字 + 后缀 + 类型表）、
      m2-design §8（sizeof/alignof → usize）、m4-design §5/§11.3（len → usize）、
      m3-design 的 malloc 声明、bytecode-mnemonics（LENGTH）。
    - 验证：`build exit=0`、**0 告警**、**1661/1661**、示例 **39/39**（0 泄漏）
      且 39/39 `emit → run .cxb` 全通过、format 幂等 **41/41**。
66. **`data(s)`：从序列取首元素指针（C ABI 的 `(T*, size_t)` 适配）**
    - **需求（用户提出）**：新增一个内建函数从 slice 取 `ref ptr`，适配 C ABI。
      与 `len(s)`（= usize = `size_t`）配对，写成 `f(data(s), len(s))` 就与 C 的
      `(T*, size_t)` 同形。
    - **语义**：`data(s) -> ref *T`（T = 元素类型；str → u8 即 `const char*`）。
      `own/ref/fatal []T` → 切片首元素；`[N]T` → 数组值自身的数据块；`str` → 首字节
      （空串给**有效**空串指针而非 NULL）。恒借用、永不拥有；空切片可能 NULL。
    - **关键决策：它是指令（`BCODE_DATA`）而不是内置函数**。第一版做成内置 cfunc
      实测**悬垂**：内置函数的实参被 `value_call` clone 进被调作用域（保证内置改不到
      调用方的值），函数形态只能拿到**副本**的地址（slice 缓冲区副本 / 数组副本块），
      调用一结束即失效（读回 `0xDDDDDDDD`）。取存储身份必须像 `x.&`（ADDR）那样在
      调用点就地完成：编译器压借用视图，新指令读其中的真实 ptr 字段。
      新操作码追加在末尾 + `.cxs` 助记符 `DATA`。
    - **顺带修掉两个真缺陷**：
      ① **内建特判从未生效**：`ex_call` 求值 callee 时 AST_IDENT 被折叠成 FUNC_REF，
         而 `upgrade` 的返回类型精化与 `len` 的实参校验是"按 callee 名字 + AST_IDENT"
         匹配 → 一直是死代码（`len(5)` 不会被拦；upgrade 靠 opaque 隐式转换侥幸通过）。
         修法：`sema_symbol_t` 加 **`builtin_op` 标记**，注册时打标、调用点按标记匹配
         （与节点形态/名字/fid 顺序无关）——这是"不该用 fid 判内建"的第二次教训。
      ② **空字符串字面量 panic**：`""` → `arena: zero-size allocation`
         （`parse_string_lit` 对 `out_len == 0` 直接 `arena_alloc(0)`）。修法：分配
         1 字节写 `'\0'`，长度仍为 0——顺带让 `data("")` 给出有效空串指针。
    - **边界（有意）**：clux 没有指针算术（`p[i]` 编译错误）——指针只喂 C，
      clux 侧遍历用序列下标或 `p.*`。
    - **测试**：`tests/ffi_testdata.c` 增补 `clux_sum_i32`/`clux_first_i32`/
      `clux_fill_i32`（C 写回调用方缓冲区）/`clux_str_first_byte`；新增
      `tests/data_builtin_test.cpp` **7 例**（含悬垂回归、`(ptr,len)` 求和、
      str→char*、结果类型精确、诊断、空切片）；新增
      `examples/ffi_data/ffi_data.cx`（与 `memcmp`/`strlen` 对接）。
    - 验证：`build exit=0`、**0 告警**、**1668/1668**、示例 **40/40**（0 泄漏）
      且 40/40 `emit → run .cxb` 全通过、format 幂等 **42/42**。
67. **结构体命名空间成员 + 方法调用糖（`Test::data` / `Test::getId` / `t.getId()`）**
    - **需求（用户提出）**：
      ```c
      struct Test {
        id: i32;
        var  data = 123;                          // 静态属性
        func getId(self: ref *const Test): i32 { return self.id; }  // 方法
      }
      Test::data;        Test::getId(t.&);        // 命名空间访问
      var t: Test = .Test{ .id = 234 };
      t.getId();         // ≡ typeof(t)::getId(t.&)
      p.getId();         // p 是指针 → ≡ typeof(p.*)::getId(p)
      ```
      **`self` 不是关键字**，只是普通参数名——方法糖按**位置**取第一个参数当接收者，
      不做任何名字特判（用户明确要求）。
    - **关键设计：成员本质就是模块级全局声明**。编译流水线在 `parse` 之后调用
      `ast_expand_struct_members`，把 `struct` 体内的 `var`/`func` 成员**改名**为
      `Test::data` / `Test::getId` 并**追加进 `prog->funcs`**——此后 sema / compiler /
      运行期完全走既有全局变量与全局函数路径（零新运行时机制）；`fmt` 路径**不**调用
      展开（成员随结构体渲染，避免重复打印）。
    - **限定访问**：sema 的 `::` 分支在左侧是**类型符号**且 `T::x` 在全局表里存在
      （VAR/FUNC）时，把节点改写成同名 `AST_IDENT` → 读取/赋值/取函数值/调用全部复用
      既有路径（赋值目标同理，在 `shadow_assign` 里改写后再走普通变量赋值，含复合赋值
      与 const 检查）。
    - **方法糖**：`ex_call` 开头识别 `callee` 是 `AST_MEMBER` → 取对象静态类型
      （指针 → base，值 → 自身）→ 在全局表里找 `T::f` → 命中即把 callee 换成限定名
      `AST_IDENT`，并**把接收者插为第一个实参**（值 → `t.&`（ADDR）；指针 → `t`）。
      随后完全走既有调用路径（callee 求值 / 实参求值 / 类型检查无新代码）。
    - **前置修复（本特性的地基，第 66 项之后单独提交）**：①指针成员访问自动解引用
      `p.x ≡ p.*.x`（读/写/复合赋值，链式成立；写穿 const 拒绝）；②`ref *T → ref *const T`
      的运行期转换缺陷（`ptr: unsupported implicit cast for pointer`）——不加这条，
      `t.getId()` 传 `t.&` 给 `ref *const Test` 形参必然失败；③运行期字段操作接受
      const/volatile 视图（PTR_GET 后拿到的是 `const T`）。
    - **测试**：新增 `tests/struct_members_test.cpp` 4 例（静态属性读/写/复合赋值 + 函数内
      共享同一份存储；方法糖三种接收者形态与显式限定调用等价；方法体内 `self.id` 读写；
      诊断：写 const 接收者 / 不存在的方法）。
    - **顺带修掉一个数据损坏 bug**：`clux format` 原先**静默删掉**结构体成员
      （`struct T { var x = 1; }` → `struct T {}`）——fmt 的 struct 分支没渲染 members。
      现已渲染并保证幂等 + 格式化后语义不变。
    - 验证：`build exit=0`、**0 告警**、**1674/1674**、示例 **41/41**（0 泄漏）且
      format 幂等 **43/43**。
    - 测试与示例：`tests/struct_members_test.cpp` 6 例；`examples/struct_members/`（静态属性跨实例共享 + 值/指针接收者方法 + 只读方法 + 显式限定调用）。
    - （已修复，见 m6-design §14.22.2）：非指针的 `T → const T` 隐式转换运行期报 `struct: implicit cast only within same struct`；通用入口加「只加限定」分支会波及 slice/qualify shadow 路径（5 处回归），已按类型族落地（struct vtable + sema 剥限定 + 运行期字段操作），并新增 tests/const_convert_test.cpp 覆盖构造/值传参/指针传参/TDZ 与 3 个负例。
68. **`volatile` 在脚本态完全忽略（sema 全面放行）**
    - **需求（用户指出）**："volatile T 完全无视，volatile 在脚本态无意义，sema 完全放行"。
    - **实现：三处构造点一律折叠为 sub**（缺一处就会出现"声明侧是 volatile 类型、值侧不是"）：
      ① sema 类型解析 `resolve_type_expr` 的 `AST_VOLATILE` → 直接返回 sub；
      ② compiler `compile_type_expr` 的 `AST_VOLATILE` → 只编译 sub、**不发**
         `BCODE_CREATE_VOLATILE`（否则运行期类型与 sema 视角不一致）；
      ③ CTFE `ctfe_expr` 的 `AST_VOLATILE`（类型别名 `type X = [2]volatile i32;` 走这条）
         → 返回内层类型值。第三处是实测漏掉后暴露的（`Driver.RunFileTypeDefAutoHoist`
         报 "cannot initialize variable 'vp' of type [volatile i32; 2] with [i32; 2]"）。
    - **AST 与 fmt 不动**：源码里的 `volatile` 仍原样打印（格式化不静默丢关键字）；
      VM 的 `TYPE_KIND_VOLATILE` / `type_volatile_intern` 保留为 API（源码不再产生它）。
    - **效果**：volatile 不参与任何类型检查——读写/运算/复合赋值/与 `T` 双向赋值/传参/
      比较/取字段/作指针 base 全放行；`const volatile T`、`volatile const T` 只保留
      const 语义（既有的 const volatile 重赋值拒绝用例仍通过）。
    - **测试**：`tests/const_convert_test.cpp` 新增 `VolatileFullyIgnored`（用测试库
      `td_i32` 收判定）：volatile 变量读写/复合赋值、volatile↔i32 双向赋值、按值传参、
      `sizeof(volatile i32) == sizeof(i32)`、`ref *volatile T`（→ `ref *T`）读写、
      `volatile Test` 传给 `const Test` 形参、`const volatile`/`volatile const` 组合。
    - **文档**：language-spec §3.3 新增折叠说明；m2-design §10 标注"2026-10 更新，
      原 volatile 自动解包/vtable 代理设计已被取代"。
    - 验证：`build exit=0`、**0 告警**、**1677/1677**、示例 **41/41**（0 泄漏）且
      41/41 `emit → run .cxb` 全通过、format 幂等 **43/43**。
69. **结构体指针向上自动转换（大 → 小，前缀字段兼容）**
    - **需求（用户提出）**："结构体指针支持向上自动转换，只要字段兼容，就可以大结构体
      转小结构体"——配合第 67 项语法糖：把"派生"结构体指针直接喂给按"基"结构体声明的
      接收者/参数（`Base::getId(d.&)`）。
    - **判定**：新增 `struct_type_prefix_compatible(vm, target, source)`（type_struct.c）：
      target（小/基）的字段是 source（大/派生）字段表的**前缀**（名字 + 类型逐一相等，
      目标字段数 ≤ 源）。C 布局自左向右 → 前缀字段的 offset 与类型一致 → 同一地址按
      小结构体解释安全（C 的"公共起始序列"规则）。
    - **两处成对落地**（缺一即"sema 放行但运行期报错"或反之）：
      ① `ptr_type_extends`（type_ptr.c）：`ref *Big` 用在 `ref *Small` 处合法；
      ② `ptr_implicit_cast`（type_ptr.c）：身份拷贝指针目标地址（不涉及所有权）。
    - **边界（都是刻意收紧的）**：
      * 只允许**借用目标**（ref）：own/share **目标**的向上转换会按小结构体释放/析构，
        漏掉尾部字段的 own 资源（泄漏）→ 不隐式允许；
      * 反向（小 → 大）永不允许——多出的字段没有存储；
      * **值**转换仍要求类型完全相同（`struct_type_compatible`），否则静默切片；
      * **丢限定不允许**：加 `ptr_upcast_keeps_quals` 守卫——初版漏了它，前缀判定把
        `ref *const T → ref *T` 也"兼容"掉了，被既有负例 `ConstConvertTest.NegativeCases`
        抓出（这正是"先跑全量"的价值）。
    - **测试**：新增 `tests/struct_upcast_test.cpp` 2 例——正向（ref/own 向上转换、
      写穿回原存储、显式基指针、3→2 前缀、按基声明的函数参数）与 5 个负例（向下转换、
      前缀名字不符、前缀类型不符、值转换、丢限定）。
    - **文档**：language-spec §3.3 增补"结构体指针向上自动转换"小节。
    - 验证：`build exit=0`、**0 告警**、**1679/1679**、示例 **41/41**（0 泄漏）且
      41/41 `emit → run .cxb` 全通过、format 幂等 **43/43**。
70. **结构体组合（`...Base;`）搬到"类型 seal 之前"的窗口 + seal 窗口不变量**
    - **需求（用户提出）**：`struct Test { ...Base; }` 把 Base 的字段嵌进 Test。
      用户纠正实现路径："这个结构体组合构建类型应该在 sema 后阶段，不是语法树嫁接，
      而是遍历旧的 type（Base）复制到新的 type（Test）中，因为需要考虑未来的装饰器
      会在 sema 创建 type 后修改 type"；并补出隐含规则："新 struct 在聚合后的
      attribute 可以覆盖原来的（重写，但是字段不允许覆盖，冲突报错），也就是一条
      隐含的规则，在 seal 之前可以复写编辑类型的 attribute"。
    - **重构（本轮核心）**：抛弃"seal 之后就地改写字段表 + 重算布局"的做法，改为
      **在目标类型 seal 之前**完成组合：`type_struct_alloc_open` → `add_field` × 自有
      字段 → 插入各 `...Base` 的字段表 → 登记嵌入表 → **只 seal 一次**。布局只在 seal
      里算一次（`struct_layout_apply`），`type_t*` 身份由"同一个开放对象就地 seal"
      保持（sema_type_register / types_by_id / CTFE 拿到的都是 seal 的返回值）。
    - **不变量（代码强制）**：`type_struct_insert_embed_fields` / `type_struct_set_embeds`
      对 `type_is_sealed(t)` 为真的类型**拒绝**（返回 false，类型一字不改）——与既有
      `type_struct_add_field` 同一口径。新增 `StructTypeTest.EmbedFieldsOnlyEditableBeforeSeal`
      直接断言"seal 前可写、seal 后拒绝且不改动"。clux 源码自身**没有** attribute /
      装饰器机制（`attr`/`attribute` 只在 third_party 命中 C 宏），因此"seal 前可编辑
      属性"这条规则以**不变量 + 文档 + 测试**落地，未发明新语法。
    - **定义顺序无关**：`sema_struct_compose_pass` 先做嵌入环检测（DFS + 在途标记），
      再"依赖序遍历 + 反复尝试"——每轮把 `...Base` 已就绪的定义当场做掉，直到一轮没有
      新进展。因此 `...Base` 的 Base 写在 Test **之后**（甚至整条链倒序）都可用；直接
      自嵌入 / 双环 / 长环统一报 `circular struct embedding ('...' cycle ...)`。环检测
      放在求值之前是必须的：环里的定义永远等不到依赖就绪，先求值只会静默延后（无诊断）。
    - **字段不允许覆盖**：插入后按最终字段序逐名比对——自有字段撞嵌入字段、两个嵌入
      字段重名、菱形里共享的 Base 实例字段走两条路径，全部报错（诊断区分"与嵌入字段
      冲突"和"继承自两个嵌入"，并写明 `fields cannot be overridden by struct
      embedding`）。
    - **成员继承（不复制）**：`Base::name` 只有一份，`Test` **自己没声明**时 `t.getId()` /
      `Test::getId` / `Test::count` 经嵌入链解析到同一份存储（静态属性是同一个全局变量，
      读写互通）。查找规则：先 `Test::name` 自己，再按 `...` 顺序深度优先、按类型指针
      去重、传递展开；恰好一个命中才可用，多个 → 歧义报错，零命中 → 沿用调用点既有诊断。
    - **测试**：`tests/struct_compose_test.cpp` 扩到 21 例（新增 Base 声明在后、整条链
      倒序、菱形不误报环 + 成员按类型去重、菱形字段冲突、自嵌入/双环/长环、组合类型与
      手写类型结构兼容但身份不合并）；`tests/struct_test.cpp` 新增 seal 窗口不变量用例。
    - **文档**：language-spec §3.3 增补"结构体嵌入（`...Base;`）"（语法、位置决定前缀
      兼容、成员继承与歧义、字段冲突、seal 前可编辑 attribute 的不变量）；m6-design
      §14.23 说明 type 层组合机制与 seal 前窗口。
70b. **本轮修掉的两个既有缺陷**（组合重构暴露）
    - `ptr_implicit_cast`（type_ptr.c）对 shadow 值无条件放行：非法的指针隐式转换
      （如 `ref *Test → ref *const Base`，前缀不成立）在 sema 静默通过、漏到运行期才报
      "ptr: unsupported implicit cast for pointer"。抽出类型层判据
      `ptr_implicit_cast_ok`，shadow 与真实转换走同一套判据（编译期就拒绝）。
    - `type_set_name` 只对已分配 program id 的类型生效：在**开放对象**（id=0）上设显示
      名，名字堆内存会挂在开放对象上，seal 去重命中回收开放对象时泄漏（6 字节 "Test"）。
      改为 seal + `sema_type_register` 之后设置（与仓库其它类型构造路径一致）。
    - 验证：`build exit=0`、**0 告警**、**1692/1692**、示例 **42/42**（0 泄漏）且
      42/42 `emit → run .cxb` 全通过、format 幂等 **44/44**。
71. **结构体组合的 attribute 覆盖语义（成员可被新 struct 重写）+ 具名 struct 身份不合并**
    - **需求（用户澄清，唯一权威口径）**：attribute = **结构体的字段 + `var` 定义的静态
      属性**（`func` 方法按同一覆盖规则处理）。"新 struct 在聚合后的 attribute 可以覆盖
      原来的（重写，但是字段不允许覆盖，冲突报错）"——组合后 `Test` 自己声明的同名
      attribute **静默覆盖**从 `Base` 继承来的，合法且不报歧义；**字段是唯一例外**，
      同名一律报错。
    - **覆盖语义的裁决点**：`sema_struct_member_lookup` 第 1 步"先 `T::name` 自己"——
      命中即返回，不再沿嵌入链找。因此：
      - `Test` 声明了 `var count` / `func f` → `Test::count` / `t.f()` 解析到**自己的
        那一份**，与 `Base::count` / `Base::f` **互相独立**（两次声明各自一个全局变量 /
        函数，写 `Test::count = 9` 不影响 `Base::count`）；
      - `Test` **没声明**时才继承 → 静态属性是**同一份**全局变量（读写互通）；
      - 歧义（`member 'x' ... is ambiguous: found in P, Q`）**只保留**在"自己没有、而
        多个嵌入都提供同名 attribute"的情形；自己一声明就不再有歧义。
    - **kind 组合的确定性口径**：`Test` 用 `func count` 覆盖 `Base` 的 `var count`
      （或反向）按"**名字优先、自己的胜出**"处理——**不**回退到 `Base` 的那一份（否则
      "用谁的"会随写法漂移）。使用点若与自己的 kind 不符就报错：`t.f()` 而自己是 `var`
      → `cannot call 'f': it is a struct member variable, not a method`；`Test::f = v`
      而自己是 `func` → `cannot assign to 'Test::f': it is a func member (a method,
      not a variable)`；`t.n` 命中成员而非字段 → `'n' is a member of struct 'Test',
      not a field`。三处诊断分别落在 `ex_call` / `shadow_assign` / `ex_member`。
    - **字段冲突诊断**：保持报错，错误信息里明确"字段不可被覆盖"（英文
      `fields cannot be overridden by struct embedding` / `duplicate field name 'v'
      after struct embedding: inherited from both 'P' and 'Q'`）。
    - **修掉一个既有缺陷：具名 struct 身份被去重合并**。`type_struct_seal` 的去重
      intern 原来只比字段表（`struct_same`），于是 `struct P { var n: i32 = 1; }` 与
      `struct Q { var n: i32 = 2; }`（字段表都是「1 个 i32 字段 n」）被合并成同一个
      `type_t` 对象：后 seal 的把 `type->name` 覆盖掉（P 的类型显示名变成 "Q"），并且
      `struct R { ...P; ...Q; }` 的**嵌入表退化成 [X, X]**——`collect_member_sources`
      按类型指针去重后只剩一个命中，`R::n` / `R::f` **不再报歧义**（实测 `R::n` 编译
      通过并读到 Q 的值，与 `AmbiguousMember` 用例的意图相悖）。修法：新增 seal 前可设
      的 `type_struct_mark_unique`（`struct_type_t::unique`），**具名 struct 定义**
      （`sema_struct_compose_def` 统一入口）标记为 unique 并**跳过去重**；匿名/内部
      构造（sema 匿名 struct 推断、union payload、字节码运行期 DEFINE_TYPE/SEAL）保持
      既有 dedup 语义。结构兼容与指针前缀向上转换按字段表判定，不受影响。
    - **测试**：`tests/struct_compose_test.cpp` 由 21 例扩到 **31 例**，新增
      `OwnVarOverridesInheritedVar`（两份独立）、`OwnFuncOverridesInheritedFunc`、
      `OverriddenFuncKeepsBaseReachable`（`Base::f` 仍可达）、`UnowneredMemberStillShared`
      （不声明时共享同一份）、`OwnMemberSilencesAmbiguity`、`CrossKindOverrideResolvesToOwn`、
      `AmbiguityRemainsWithoutOwnDeclaration`、`CrossKindOverrideIsAUseSiteError`、
      `FieldsNeverOverridable`、`DistinctNamedStructsAreNotMerged` 共 10 例。
    - **文档**：language-spec §3.3 增补"attribute 覆盖"小节（含 kind 裁决与"字段是唯一
      例外"）、m6-design §14.23.4/§14.23.5/§14.23.8 与 §14.23.10。
    - 验证：`build exit=0`、**0 告警**、**1711/1711**、示例 **42/42**（0 泄漏）且
      42/42 `emit → run .cxb` 全通过、format 幂等 **44/44**。
72. **设计原则固化：clux 不做隐式虚表（静态分派）+ 用户侧手写虚表示例**
    - **决策（用户原话，唯一权威）**："已经差不多了，clux 不要看不见的虚表，如果需要
      类似虚表的功能，用户应该自己实现。" 落地为四条：① **永久不做**隐式/隐藏虚表、
      不做自动动态分派（对象布局无 vptr、调用点不查表）；② 方法调用 `t.f()` ≡
      `typeof(t)::f(t.&)` 保持**编译期静态绑定**；③ 需要虚表语义时**用户显式实现**
      （函数类型字段当槽位，自己填表、自己查表）；④ 与 VM 内部的 `vtable_t`
      （**类型自身的运算行为表**）无关，文档里明确区分。**本次 C/C++ 源码改动 0 行。**
    - **先探针后落地（13 个 `build/tmp` 探针）**：函数类型字段可用（`var draw:
      func() -> i32` / `func(opaque) -> i32`）、` .T{ .f = impl }` 可填、可后置赋值、
      可经 `obj.f(args)` 调用、结构体成员方法（`Circle::area`）/ 函数字面量 / 闭包都能
      填槽；**完整手写虚表模式跑通**（`...Shape` 公共前缀 + 每类型一张全局表 +
      统一入口 `area_of(s)` + `[2]ref *const Shape` 与 `own []` 异构遍历，exit 0）。
      唯一不支持项是**字段类型依赖成环**：自引用（`next: ref *const Node`、函数签名里
      点名自身）与互相引用（两个 struct 各点名对方）都在字段处报
      `unknown field type for '<field>'`；**前向引用反而支持**（字段类型点名**后面**声明
      的 struct，含写在函数签名里，exit 0 —— 依赖图无环即可）。
      **（第 73 项已解除此边界：递归结构体现已支持，"依赖成环"不再是限制；`opaque`
      绕法仍然有效，但它今天的主要价值是接收者下转型。）**
      **最小绕法**：表行签名写 `opaque`（≈ `void*`）+ 实现里 `as` 还原——指针 → opaque
      隐式、opaque → 指针显式，属于"用户可自行实现"，不需要改调用约定/对象布局/加指令。
      另记三条用法：含 `ref` 字段的变量要 `'<*>` 标注；`len` 是 `usize`（下标用 usize）；
      指针 → opaque 不能写 `as`（隐式即可）。
    - **新增示例**：`examples/manual_vtable/manual_vtable.cx`（自检式，三张表
      Circle/Rect/Square + `name_of`/`area_of` 统一入口 + 数组/切片遍历 + 同型不同表的
      两个实例演示"表是数据、运行期换表换行为"），文件头写明"clux 不做隐式虚表，
      这是用户侧模式"。输出：
      `manual_vtable: circle=27(circle) rect=20(rect) square=36(square) total=83 doubled=72`。
    - **文档**：`docs/language-spec-m1.md` 结构体嵌入之后新增非目标条款（静态绑定 /
      用户自实现 / 与 `vtable_t` 区分 / 用户侧模式边界）；`docs/m6-design.md` 新增
      **§14.24**（原则、设计理由、探针结论与诊断原文、四件套骨架、与 `vtable_t` 对照表）。
      核查 `docs/` 全文：无任何旧文字暗示"未来补虚方法/动态分派"（其余 `vtable` 命中
      全是 VM 类型运算行为表，保持不动）。
    - 验证：强制全量重编 **188 步 exit 0、0 告警/0 错误**；全量 **1712/1712 PASSED、
      0 FAILED**、无泄漏报告；示例 **43/43**（带 `func main`）`run` exit 0 且 0 泄漏、
      43/43 `emit → run .cxb` 通过、format 幂等 **45/45**（`fmt(fmt(x)) == fmt(x)`）。

73. **递归结构体（自引用 / 互相引用）——解除第 72 项记录的唯一语言边界**（2026-10）
    - **语义**：字段类型依赖图**允许成环**，只要环经过"布局不依赖被引用类型完整布局"的
      包装：指针族（`own/ref/fatal/share/weak *T`）、切片、函数签名、`const`/`volatile`、
      数组 `[N]T`。**按值递归**（`struct A { a: A; }`、`[N]A` 直接内联、`?A`）仍拒绝：
      `struct 'A': field 'a' has incomplete type 'A': recursive struct by value
      (break the cycle with a pointer, e.g. 'ref *A')`。定义顺序无关（前向引用照旧）。
    - **sema 物化**（`struct_compose.c:materialize_struct_type`）：求值前先给目标定义建
      **开放**类型对象（分配 id + 显示名 + 编译期类型名绑定）并**入池**（失败路径由
      `vm_destroy` 回收；`type_struct_seal` 按指针避免重复入池），字段类型表达式于是能解析
      到"正在定义中的自己"与环上其它定义。
    - **AST 结构预判**（`slot_blocked_by_unsealed`）：判定字段类型是否**按值内联**了尚未
      seal 的结构体，**不创建任何类型对象**（避免把未定型的派生类型 intern 进池并折叠进
      槽位）；`option/tuple/union` 包装按值内联元素布局 → 重置"在指针之下"标记。
    - **按值递归环诊断**（`report_incomplete_field_cycles`）：顺"按值阻塞"链找环，链回到
      起点才报；区分"按值"与"包装类型"两种文案；一次性门闩（试跑轮不重复打印）。
    - **布局延迟定型**（`type_sync_derived_layout`）：`const`/`volatile`/数组这些"布局派生
      自 sub/elem"的包装在构造时拷到的是**未 seal 的 sub**（size=0）→ 在 sub seal 之后就地
      同步（链式到不动点）。调用点放进 `type_struct_seal` 内部，**编译期与运行期共用一处**。
    - **hoist 依赖定义修正**（`compile_hoist.c:emit_ref_dep_type`）：早期"引用依赖"（指针
      base / 切片 elem / func 参数返回）直接 `LOAD_TYPE` 不定义依赖，会让指针 base 捕获
      "稍后被 seal 去重回收"的开放对象（实测：开放 `const i32` 命中 sema 既有 `const i32`
      后被 `allocator_free`）→ 悬垂 → 运行期 `ptr: unsupported implicit cast for pointer`。
      新规则：依赖未处理 → 先定义；**正在定义中（环）** → 直接 LOAD 其开放对象（环上被
      捕获者一定是本程序新建、不会与 sema 既有类型去重的类型，身份稳定）。
    - **VM 值层修复**（递归字段常用 `?ref *const T` 暴露的既有缺口）：`value_implicit_cast`
      放宽 `T → ?T` 提升门槛；`value_lift_option` 内层类型不同时先做一次内层隐式转换
      （`ref *T → ref *const T`）并在拷贝后释放临时值（原先每次提升泄漏一个 `value_data`）。
    - **测试**：`tests/struct_recursive_test.cpp` 12 例（链表 / 方法遍历 / 互相引用 / 二叉树 /
      自引用函数签名字段 / 数组与切片包装布局 / `?ref *const T` 提升 / 前向引用 + 3 个按值
      递归负例 + option 包装负例）；新增 `examples/recursive_struct/recursive_struct.cx`
      （链表 + 方法、二叉树 + 递归、互相引用、自引用签名字段，自检式）。
    - **顺带修掉既有缺陷**：`clux format` 渲染后缀解包 `a.!` 丢前导点（输出 `a!`）→
      格式化产物无法重新解析、幂等在第二遍失败；修法与 `.*`/`.&` 同款（补点 + 后缀优先级
      保括号），新增 `Fmt.UnwrapKeepsLeadingDot`。
    - **文档**：`language-spec-m1.md` §3.3 新增"递归结构体"小节 + 修正旧"依赖不能成环"
      条款；`m6-design.md` 新增 **§14.25**（机制六点）并在 §14.24.3 标注旧结论已解除。
    - 验证：强制全量重编 **0 告警 / 0 错误**；全量 **1725/1725 PASSED / 0 FAILED**、0 泄漏；
      示例 **44/44** `run` 与 44/44 `emit → run .cxb`、format 幂等 **46/46**。
74. **defer（延迟执行块）：闭包值 + 作用域 defer 栈，pop_scope 时内存释放前逆序调用**（2026-10）
    - > **实现已更新**（2026-10-16，`docs/ownership-refactor.md` §4.2 决策 E）：
      > DEFER 前移到编译期——sema 改写为 `AST_FUNC_DEF` + 作用域退出点 `CALL` 序列；
      > 运行期 `BCODE_DEFER` / `scope_t::defers` / `scope_run_defers` 全删，
      > `vm_pop_scope` 回归"只弹栈销毁"。下方描述保留作历史记录，当前实现以
      > `ownership-refactor.md` 为准。语法形态、捕获语义（clone 值、LIFO、嵌套）不变。
    - **语法/口径**：`defer ['<a,b>] [|cap, (name: T = init)|] { body }`。捕获列表
      **复用闭包那一套**（`parse_capture_list` 改为导出后直接调用；作用域标注同样复用
      `parse_scope_annotation`），空捕获三种写法等价——省略、`defer || {`、`defer | | {`
      （`||` 是 lexer 的单个 SYMBOL token，defer 之后只能是捕获列表/块体，与逻辑或无歧义；
      三者 AST 相同，fmt 统一渲染为省略形态）。只接受 `defer` 后跟块体——没有参数表，
      也没有返回类型。
    - **AST**：新增语句节点 `AST_DEFER`（壳）+ **复用闭包节点** `ast_func_def_t`
      （`ast_defer_t.closure`：`params/return_expr = NULL` = 无参 void、`captures` /
      `scope_annot_names` / `body` 与闭包同字段）。于是 sema 侧直接走
      `sema_check_func_literal`（捕获可见性 / §9.1 own 拒绝 / §9.3 ref 强制标注 /
      body 类型检查 / 签名登记），compiler 侧直接走 `MAKE_FUNCTION` + `SET_CLOSURE`
      发射——**捕获语义就是闭包语义（clone 值，非引用）**，零复制。
    - **执行时机**：`vm_pop_scope` 里**先** `scope_run_defers`（`scope_t::defers` 逆序
      `value_call`，逐个从栈尾弹出），**再** `scope_destroy`（own/fatal dispose + 释放
      vars/scope）。因此 defer 体内读得到本作用域的一切，含**即将释放的 own/fatal 变量
      与其堆块**（测试用 `defer '<*> |(r: ref *T = p.*.&)|` 读 own 堆块内容锁定这条顺序）。
      覆盖全部**正常**离开作用域的路径：块尾 `POP_SCOPE`、`break`/`continue`
      （`balance_scopes_out` 补发 POP_SCOPE）、`return`（`func_vcall` 收尾逐层 pop）、
      switch 分支体、循环体每轮迭代；顺序 = 内层先于外层、同层 LIFO。
      **错误/panic 值向上传播**的展开刻意**不**触发 defer（`scope_destroy_subtree`
      分支的注释在 defer 之前就写着"避免触发 defer"）——边界在 `include/vm/scope.h`
      与语言规格里写明。
    - **所有权（无泄漏/无 double free）**：`defers` 是 **`value_t*` 借用向量**；值本体由
      `MAKE_FUNCTION` 经 `value_make` auto-track 进同一个 scope 的 `owned`，故栈只记指针，
      执行完由 `scope_destroy` 照常回收（func value 的 dispose 是 no-op，`func_t` 归
      `vm->functions`）；`defers` 向量自身只释放结构。空向量直接返回 → 普通作用域零开销。
    - **新指令 `BCODE_DEFER`**（无操作数，**追加在指令集末尾**不动既有编号）：弹函数值 →
      借用登记进当前作用域 defer 栈。三处同步：`include/vm/bcode.h` + `src/vm/exec.c`
      （`op_defer` + handler 表）+ `src/vm/bcode_asm_defs.c`（`"DEFER"`），
      `docs/bytecode-mnemonics.md` §4.4/§4.14 补表；`.cxs` 往返逐字节一致、
      `emit → run .cxb` 通过。
    - **返回值口径**：defer 的返回值不传给任何消费点（作用域正在销毁）。但 **error 值**
      会以 `defer: <消息>` 打到 **stderr**（不改变控制流/退出码）——否则 defer 体内的
      失败会被静默吞掉。块体按**函数体**处理：非 void 表达式语句须 `_ = expr`，
      `return` 只从该块体返回（void，不能带值），`break`/`continue` 跨不出去。
    - **其余落点**：lexer 关键字表加 `"defer"`（**未动 `self`**）；`fmt` 补 `AST_DEFER`
      渲染分支（否则会静默吃掉整条语句）；`sema/stmt_build.c` 显式跳过（不建子作用域，
      3a/3b 子作用域索引对齐）；CTFE 对 `defer` 明确报错（不静默丢弃）；
      `compile_func.c:prescan_stmt` 收集 defer 的闭包（否则基底/函数体不构造）。
    - **顺带修掉既有缺陷**：`clux format` 对闭包**完全没有作用域标注渲染分支**
      （`func '<x> |r| ...` 的标注被静默吃掉 → 格式化产物再编译直接报错，与第 73 项记的
      `a.!` 丢点同类）。新增 `render_scope_annot_names` 并在命名函数/函数字面量两处接上，
      新增 `Fmt.ClosureScopeAnnotationPreserved` 锁定。
    - **测试**：`tests/defer_test.cpp` **21 例**（LIFO / 空捕获三写法 / clone 捕获 /
      多捕获两形态 / 嵌套含 return / 循环 LIFO / break·continue / 多层 return 弹出 /
      **defer 先于 own 释放** / defer 体内调用函数 / defer 体内再登记 defer / switch 分支 /
      函数体顶层 + 9 组负例 + `move()` 捕获正例）；`tests/fmt_test.cpp` 新增 3 例；
      新增 `examples/defer/defer.cx`（自检式，输出
      `defer ok: lifo=321 capture=7 nested=321 loop=102132 own=43 slice=60`）。
    - **文档**：`language-spec-m1.md` 新增 §4.1.3（语法/捕获/LIFO/执行时机/控制流交互）；
      `m6-design.md` 新增 **§14.26**（口径九条 + 实现落点表 + 已知边界）；
      `bytecode-mnemonics.md` 补 `DEFER`。
    - 验证：强制全量重编（touch 全部 src/include/tests）**190 步 / 0 告警 / 0 错误**；
      全量 **1749/1749 PASSED / 0 FAILED**、无 `memory leak` 报告；示例 **45/45** `run`
      与 45/45 `emit → run .cxb`、format 幂等 **47/47**（`fmt(fmt(x)) == fmt(x)`）。
75. **语义收紧：类型不是运行期值（`var` 不能接收类型）+ `type T = typeof(a)` 可用**（2026-10）
    - **起点是一个崩溃**：`var ty = typeof(a);` 后 `sizeof(ty)` → `0xC0000005`。
      `ex_sizeof_alignof` 用 `value_is_type(op, TYPE_KIND_TYPE)` 判"是不是 type value"，
      但**类型为 `type` 的变量读出来是 shadow（data = NULL）**，`value_as` 解引用空指针。
    - **口径（用户确认）**：`typeof` 返回的是**类型**，类型不能由 `var` 接收——
      绑类型只有 `type`：`type Ty = typeof(a);`。`var t = i32;` / `var t = <i32,i32>;`
      同理拒绝（``cannot initialize 't' with a type value … bind it with `type <name> = <type expr>` ``）。类型表达式仍可用于**类型位置**（`type` 右值、
      类型标注、cast 目标、`sizeof`/`alignof`/`typeof` 的操作数）与类型计算。
    - **连带修好两处**（否则正确写法也跑不通）：
      ① `type T = typeof(a)` 曾被 type-rhs 预检误报 `'a' is a variable, not a type`
      （预检错误地下钻 typeof/sizeof/alignof 的**表达式**操作数）→ 新增
      `AST_WALK_SKIP_CHILDREN` 遍历动作，预检对这三者不下钻；
      ② 局部 type def 提升到块入口求值时，同块 `var a` 尚未激活 →
      `ctfe: cannot determine operand type`；`ctfe_operand_type` 增加"按声明查找"
      回退（并给 `sema_eval_type_def` 传 `sema_scope`）。
    - **防御**：新增 `value_is_type_value()`（要求 data 非空），`sizeof`/`alignof`/
      `typeof`/`const`/`volatile` 一律用它判类型实体——即使未来放开某条 shadow 路径
      也不会再空指针解引用。
    - **测试**：`SemaTest.TypeValueNotBindableToVar` / `…Literal`、
      `SemaTest.TypeAliasChainViaTypeDecl`、`SemaTest.TypeDefRhsTypeofLocalVar`、
      `SemaTest.SizeofTypeofOperandIsUsize`、`SemaTest.TypeExpressionsInTypePosition`、
      `SemaTest.TypeExpressionNotBindableToVar`、
      `Driver.RunFileTypeDefTypeValueRejectedInVarInit`、
      `Driver.RunFileTypeDefRhsTypeofLocalVarPasses`、
      `Driver.RunFileTypeValueInVarInitRejectedWithHint`、
      `Driver.RunFileTypeExpressionsInTypePositionPass`、
      `Driver.RunFileTypeExpressionInVarInitRejected`（4 种类型表达式逐个验）；
      原先编码"类型可绑进变量"的 6 个测试按新语义重写。
    - **文档**：`m2-design.md` §8 增补口径；`repro/sema-defects/README.md` D 段更正。
76. **`.cxs` 手写样本复活：FFI 声明 + `bc asm` 失败路径泄漏**（2026-10）
    - **现象**：`examples/asm/*.cxs` 5 个样本全部"能汇编、运行报
      `exec: undefined variable`"——printf 自 M6 起不再是内建，样本却仍写
      `push "printf"; call N`。它们恰好是"手写汇编怎么调外部函数"的唯一示例。
    - **修复**：样本改用 `externlib "*" as libc` + `extern_call "libc::printf"
      <argc> 65792`（文本格式早已支持 `externlib` 行，只是样本没跟上）；`hello.cxs`
      头部补充 packed 编码说明。
    - **顺带**：`bcode_graph_asm_parse` 的失败路径漏释放两处——模块映像表
      `clux.vm.graph.mods` 与循环内新建的入口 `import_path`（`clux.vm.graph.path`）。
      改为把 `import_path` 所有权移交模块映像、失败时交 `bcode_graph_free` 统一释放。
    - **测试（此前 `.cxs → 图` 零覆盖）**：新增 `tests/bcode_graph_text_test.cpp`
      （解析 externlib / 往返保留 `externlib` 行 / 失败不泄漏 / 坏 `externlib` 行拒绝）、
      `tests/asm_examples_test.cpp`（5 个样本逐个 `asm → run`，并结构性断言不再出现
      内建 printf 写法）。
    - 验证：全量 **2081/2081 PASSED / 0 FAILED**；`examples` 入口 45/45、`.cxs` 5/5
      均可运行并输出预期结果。
77. **`usize` 改成独立类型（C 的 `size_t` 语义角色，但不与 u64 同一）**（2026-10；取代第 65 项的"别名"设计）
    - **动机（用户指出）**：`usize` 必须是独立数字类型，大小与操作系统架构一致
      （相当于 C 的 `size_t`），而不是 `u64`/`u32` 换个名字。
    - **类型实体**：新增内建单例 `vm->type_usize`（`src/vm/type.c`）——kind INT、
      无符号 vtable、`size/align = sizeof(void*)`、name `"usize"`、**id 18**
      （`TYPE_ID_BUILTIN_COUNT` 18 → 19，紧随 opaque 17；module 不占 id 表）。
      `vm_type_usize()` 从"按指针宽度返回 u64/u32"改为直接返回该单例；
      `vm_register_builtin_types` 只做 id 表 + global scope 登记（不再是别名绑定）。
    - **整数族**：`type_int.c` 的 `is_unsigned_int` 纳入 usize（算术/位运算/比较/
      转换都按无符号整数走）；**隐式转换的宽窄判定改用 `type->size`**（同宽 = 表示
      相同 → 允许；缩窄拒绝）——序号比较会把 `usize → u64` 误判成缩窄；
      `type_promote` 里 usize 与同宽无符号类型收敛为 usize，其余按 rank。
    - **长度语义统一**：`len(...)`（签名 + 运行时值）、数组/切片 `length`、
      `sizeof`/`alignof`（sema 折叠 + CTFE `ctfe_make_int`）、`for (i in …)` 的
      计数器（`compile_stmt.c` 新增 `emit_usize_const`）、`[N]` 边界全部是 usize。
    - **字面量**：`5usize` 不再归一化成 u64/u32（`parse_int_lit.c`），类型名原样
      保留为 `usize`，编译器按 `usize->size` 选 `PUSH_U64`/`PUSH_U32`；fmt 因此
      也不再把它重写成 u64（顺带修掉一个格式化破坏源码的坑）。
    - **FFI**：新增 `BCODE_FFI_DATA_USIZE`(14) 与 `BCODE_FFI_RET_USIZE`(15)——
      `ffi_data_kind_of` / `ffi_data_type_of` / `ffi_data_ffi_type`（uint32/uint64
      按平台）/ `ffi_ret_to_data_kind` / sema 的返回码映射全部接上。`strlen` 这类
      `size_t` 返回的函数现在真的得到 `usize`（此前会退化成 u64）。
    - **其它**：`bool`/`float` 的显式转换目标、`clux eval` 的整数打印、
      `comptime` 常量编解码、enum 底层/变体值白名单、enum 变体发码都认识 usize。
    - **测试**：`tests/usize_test.cpp` 重写 + 扩充（VM 层断言"独立身份"：与
      u64/u32 不是同一个 type_t、id 表可查、len 返回它；源码层 6 例：长度语义、
      同宽双向隐式/变宽显式、类型身份 `usize == u64` 为假、字段/参数/返回、
      FFI size_t 往返、sizeof 折叠类型；负例：与有符号类型隐式混合被拒）。
    - **文档**：language-spec §2.1/§2.3/§3.1、m2-design §8、m6-design §14.20 的
      码表、architecture-m1 规模表。
    - **新增指令**：`PUSH_USIZE`（U64 操作数 → usize 值，VM 按平台截断）——
      `PUSH_U64` 造的是 u64，`?usize{...}` 提升/结构体字段/FFI 实参等处会因类型
      身份不同失败；`.cxs` 助记符表与扩展高亮同步（118 条）。
    - 验证：全量重编 **0 告警**；**2086/2086 PASSED**；`examples` 45/45（run 与
      emit→run 均通过）；`fmt` 幂等 48/48；`.cxs` 样本 5/5。

78. **FFI 动态库表示形式：通用库名（CMake 风格）取代完整文件名**（2026-10）
    - **动机（用户指出）**：`extern lib "<完整文件名>" as <别名>;` 把平台文件名
      写死在源码里（`msvcrt.dll` / `libc.so.6`），**无法跨平台**。参考 CMake 的
      同名风格改成**通用库名**，执行器按平台展开候选文件。
    - **新语法**：`extern lib <别名> from "<库名>";`（与 `import <别名> from
      "<路径>"` 同形，别名在前）：
      ```c
      extern lib libc   from "c";        // c.dll / libc.so / c.so
      extern lib user32 from "user32";   // 通用名，自动补 .dll
      extern lib libc   from "*";        // 当前进程自身（不变）
      ```
      旧形式**已删除**；解析器对它给出**迁移提示**（而不是笼统语法错误）。
    - **解析（`vm/ffi_lib.c`）**：候选序列 = **as-is**（显式路径 / 已带后缀的
      文件名 / 系统搜索路径上的裸名字）→ 平台候选（Windows `<name>.dll`；
      macOS `lib<name>.dylib` / `<name>.dylib` / `lib<name>.so` / `<name>.so`；
      其它 `lib<name>.so` / `<name>.so`），逐个尝试，第一个成功者即加载；失败时
      诊断里**列出试过的候选名**（"名字拼错/不在搜索路径"最好定位）。
    - **容器（关键）**：`ffi_lib_t` 同时记**声明的通用名**（`ffi_lib_name`）与
      实际命中的候选（`ffi_lib_path`，诊断用）；`.cxb`/`.cxs` 序列化写**通用名**
      ——产物不再携带解析后的绝对路径，因此可跨机器/跨平台搬运（加载侧按名字
      重新展开候选）。旧 `.cxb` 里存的是显式文件名，靠 as-is 候选继续可加载。
    - **迁移**：示例/测试/文档共 63 个文件改用新语法（脚本迁移 + 人工校正）；
      Windows 专用示例（msvcrt）改用通用名 `msvcrt` 以体现新写法。
    - **测试**：新增 `tests/parse_extern_lib_test.cpp`（新语法解析 / 显式路径 /
      旧形式迁移提示 / 缺 `from` / 空库名 / 非标识符别名；
      `ParseExternLibTest.*` 6 例）；`tests/ffi_lib_test.cpp` 新增 4 例（通用名
      自动补平台后缀并能解析符号、显式文件名 as-is、失败诊断列出候选、`"*"` 伪库
      名字）；`tests/driver_module_bcode_test.cpp` 新增/改写 3 例（缺 `from`
      语法错误、旧形式迁移提示、`ContainerStoresGenericLibraryName` 断言容器里
      存的是通用名且**不含**平台文件名）。
    - **文档**：m6-design §1.1（语法 + 候选规则 + 容器语义）、bytecode-mnemonics
      （`externlib "<库名>" as <别名>` 存通用名）、language-spec（printf 一节）、
      `include/vm/ffi_lib.h` 的契约注释。
    - 验证：全量重编 **0 告警**；**2098/2098 PASSED**；`examples` 45/45（run 与
      emit→run 均通过）；`.cxs` 样本 5/5。

79. **FFI 库解析跨平台化：候选并集 + exe 目录回退 + Linux/WSL 实跑**（2026-10）
    - **用户反馈**：①"当前环境有 WSL，可以试下 linux 的 so 支持"；②"windows 上 so
      也需要支持（不直接区分操作系统）"。
    - **候选表改成跨平台并集**（`ffi_library_candidates`，`vm/ffi_lib.c`）——不再按
      编译期 OS 分支：
      `<name>`（as-is）、`<name>.dll`、`lib<name>.so`、`<name>.so`、
      `lib<name>.dylib`、`<name>.dylib`。
      判据是"当前加载器能否装载"，不是扩展名：Windows 的 `LoadLibraryA("x.so")`
      能装载**任何** PE（扩展名不参与判定），MinGW/MSYS 产出的 `.so` 因此可直接
      用；Linux 上 `dlopen("x.dll")` 只是一次失败的 open。同一份 `.cxb` 搬到另一
      个平台无需改名字（容器里存的就是通用名）。
    - **装载两轮**：① 按名字（系统搜索路径）；② `<可执行文件目录>/<候选>`——
      Linux/macOS 默认**不**搜索 exe 目录，而"库与程序放一起分发"是最常见的用法。
      第二轮排在后面：系统库优先，避免同目录同名文件劫持（安全默认）。
      诊断保持紧凑：名字候选列一次，第二轮用 `(+ <exe 目录>/{same})` 概括。
    - **测试**：`tests/ffi_lib_test.cpp` 新增 `CandidatesArePlatformAgnosticUnion`
      （断言 6 条候选的顺序与内容、显式路径排第一、cap 截断）与
      `WindowsLoadsSoNamedPeThroughCandidateUnion`（把测试 DLL 复制成
      `lib<名字>.so`，用通用名打开 → 命中的正是 `.so` 候选，符号可解析）。
    - **Linux/WSL 实测**（WSL2 Ubuntu 26.04 / gcc 15 / CMake 4.2）——期间修掉三处
      **既有的 POSIX 可移植性缺口**（此前只在 Windows 构建过）：
      1. `include/core/allocator.h` 用 `size_t` 却没包含 `<stddef.h>`（Windows CRT
         头传递引入，POSIX 不引）；
      2. `src/vm/ffi_lib.c` 用 `va_list` 却没包含 `<stdarg.h>`；
      3. `CMakeLists.txt` 在 POSIX 上没接 libffi：新增 `find_library(ffi)` 并把它
         封装成同一个 `FFI::ffi` 目标（带 `CLUX_HAVE_LIBFFI=1`），Windows 仍走
         `third_party/libffi-port`。
      ICU 源码复用 Windows 侧 `build/_deps` 缓存（WSL 到 GitHub 不可达），
      以 `FETCHCONTENT_FULLY_DISCONNECTED=ON` 离线配置。
    - **顺带修掉的测试可移植性**（Linux 实测暴露，均为既有问题）：
      `ffi_lib_test` 的 sqrt/pow 改用平台数学库（glibc 的 libm）；
      `ffi_extern_call_test` / `driver_ffi_own_test` 的系统库与测试库名改为平台常量；
      `ffi_process_test` 里依赖 Windows CRT 数据符号（`_fmode`）的两例在 POSIX 上
      `GTEST_SKIP`（程序体的数据符号段也只在 Windows 拼入）；
      `VmLifecycle.PopGlobalPanics` 的死亡测试改用 threadsafe 风格（fast 风格在
      POSIX 上会拿不到子进程 stderr）。
    - 验证：Windows 全量 **2100/2100**；WSL2 Ubuntu 26.04 构建 **0 告警**、
      全量 **2097 passed / 2 skipped**（跳过项即上面两个 Windows CRT 数据符号用例）。
      `libclux_ffi_testdata.so` 由通用名 `"clux_ffi_testdata"` 命中候选
      `libclux_ffi_testdata.so`（走 exe 目录回退，未设 `LD_LIBRARY_PATH`）；
      libffi 走系统库（`-- FFI: using system libffi: libffi.so`）。

80. **FFI 导入第二阶段：`extern lib ... assert(<条件>)` 条件后缀**（2026-10）
    - **需求（用户提出）**：实际动态库是否导入、导入哪一个，在不同场景下不同，
      静态的 `extern lib` 不够用。方案：可选后缀
      `assert(<key> == <value>)` / `assert(<key> != <value>)`，键由编译/运行时
      注入（类似 CMake `-D`），值恒为字符串；OS 注入默认键（如 `OS_TYPE`）；
      只支持基本 `||` / `&&`；键**不是变量**，只在 assert 里可见。
    - **设计决定（用户确认）**：**编译期求值**——条件不满足时**整条 extern lib
      语句被裁剪**，于是"两条不同平台的 `extern lib crt from ...`"不冲突
      （`crt` 只生效一条）。实现落在**前脱糖**（parser → sema 之间），与
      `foreach` 展开同一层；sema / driver / 容器都看不到被裁剪的声明。
    - **键集合 = 进程级全局静态表**（用户明确要求）：`include/core/ffi_cond.h`
      + `src/core/ffi_cond.c`（无堆分配）。内建 `OS_TYPE`（`WIN64`/`WIN32`/
      `LINUX`/`MACOS`/`BSD`）、`OS_ARCH`（`X64`/`X86`/`ARM64`/`ARM`）；
      命令行 `-D KEY=VALUE` / `-DKEY=VALUE`（可重复，同名覆盖内建）。
    - **条件语言**：`KEY == "值"`、`KEY != "值"`、`&&`、`||`、括号；
      `&&` 优先级高于 `||`（与 C 一致）。未定义的键**报错**（
      `unknown define 'X' (inject it with -D X=<value>)`）——拼错若静默为
      false，整条声明会被无声裁剪。
    - **语法**：`extern lib <别名> from "<库名>" assert(<条件>);`。`assert` 是
      **软关键字**（只在该位置按文本识别，不占词法关键字，`var assert = 1;`
      照常可用）。解析期采集 token 并规范化成单空格文本，同时做语法校验
      （键是否存在留到脱糖时判定）；fmt 原样保留后缀（幂等，漏渲染会把
      "仅 Windows 生效"变成"永远生效"）。
    - **CLI**：`clux run ... [-D KEY=VALUE]...` 与 `clux bc emit ... [-D ...]`
      （`cmd/path.c` 的 `cmd_take_defines` / `cmd_apply_defines`；单横线选项由
      handler 显式扫描，与 `-o` 同一约定）。help/usage 同步。
    - **测试**：新增 `tests/ffi_assert_test.cpp` 18 例——条件语言（内建键注入、
      覆盖、6 类语法错误、`&&`/`||` 优先级与括号、未知键）、解析（AST 规范化
      文本 / 无后缀 / 畸形条件 / `assert` 软关键字）、端到端（同别名互斥声明
      只生效一条、`-D OS_TYPE=LINUX` 切换、未知键编译期报错、自定义键切换、
      `bc emit` 容器只留存活声明）、fmt 幂等、`-D` 参数收集（两 token / 粘连 /
      缺值 / 超容量）。
    - **新增示例**：`examples/ffi_assert/ffi_assert.cx`（三平台各一条 `crt`
      声明，Windows 实跑输出 `runtime strlen("hello") = 5`）。
    - **文档**：m6-design §1.1（条件后缀小节）、language-spec（"动态库声明的
      条件后缀"）、README（M6 FFI 状态）、`include/core/ffi_cond.h` 契约注释。
    - **示例处理**（条件后缀让"按平台选库"在示例里落地）：
      * `examples/ffi_callback`：`crt` 改成三平台条件声明（`msvcrt` / `libc.so.6`
        / `libSystem.B.dylib`）。`qsort` 三平台都有，示例因此**在 Linux 上也能跑**
        （`qsort called the clux comparator 8 times`）。
      * `examples/ffi_process`：去掉 Windows 专用的 `libc::_osplatform`
        （数据符号只有 Windows CRT 有），保留跨平台的 `abs` / `strlen` / `printf`，
        示例回到三平台可跑；数据符号的演示指向 `ffi_extern_var`。
      * `examples/ffi_extern_var`：`_fmode` / `_osplatform` / `_pgmptr` 是
        Windows CRT 数据符号、POSIX 无等价物 → 声明加
        `assert(OS_TYPE == "WIN64" || OS_TYPE == "WIN32")`，头部明确"本示例
        Windows 专用"；其它平台在第一个 `crt::` 引用处即报"未知库别名"。
      * 顺带修掉 4 处仍在提示**已删除旧语法**的诊断/注释
        （``add `extern lib "..." as crt;` `` → ``extern lib crt from "<name>";``）。
    - 验证：Windows 全量 **2118/2118 PASSED**；examples 49 个 `.cx` 全过（含新增
      示例）；`fmt` 幂等（examples+repro 共 79 文件，条件后缀原样保留）；
      `.cxs` 样本 5/5；WSL2 Ubuntu 全量 **2115 passed / 2 skipped**（跳过项是
      §79 里那两个 Windows CRT 数据符号用例）。Linux 上 `ffi_callback` /
      `ffi_process` / `ffi_assert` 三个示例直接跑通，`ffi_extern_var` 按预期在
      编译期报未知库别名；`bc emit -D` 产物可直接 run。

81. **函数是编译期的值：comptime 函数返回函数（含闭包）**（2026-10）
    - **需求（用户提出）**：装饰器的前置能力——"comptime 函数能返回正常函数，
      理论上函数就是编译期的值"。
    - **落地**：
      * comptime 函数返回**普通函数**（全局函数 / 体内定义的局部函数 /
        struct 方法）→ 折叠为 `AST_FUNC_REF`（`LOAD_FUNCTION <fid>`）。
      * comptime 函数返回**闭包**（`func |f, n| ...`）→ **常量闭包**：
        CTFE 求值字面量时按捕获值物化实例 env（`func_instantiate_raw` +
        `func_closure_store`，与运行期 `MAKE_FUNCTION`/`SET_CLOSURE` 同构）；
        sema 编码为 "func id + 捕获常量表"（`sema_ct_capture_t`）；
        折叠成**克隆的 func 字面量**（括号捕获 `|(name: T = 值)|`），
        compiler 走既有函数字面量路径 —— **零新指令、零容器格式变更**
        （`.cxb` 里就是 MAKE_FUNCTION + SET_CLOSURE，实测 `.cxb→.cxs→.cxb`
        字节往返可运行）。
      * 捕获值必须是编译期常量；捕获运行期变量给出明确诊断
        （`ctfe: cannot capture runtime variable 'x'`）。
    - **新增 API**：`func_closure_store`（`vm/function.h`，SET_CLOSURE 与 CTFE
      物化共用）、`sema_ct_const_t::captures/capture_count`、
      `func_t::ct_literal`（编译期字面量 AST，供折叠取回参数/函数体；
      `pass_globals` 早于 Pass 3b，`sema_func_by_id` 此时查不到字面量）。
    - **测试**：`tests/comptime_closure_test.cpp` 8 例——返回普通函数、返回体内
      定义的函数、返回捕获常量的闭包、返回捕获**函数值**的闭包（装饰器核心
      形态）、多捕获、`.cxb` 往返（含反汇编出现 MAKE_FUNCTION/SET_CLOSURE）、
      捕获运行期变量的诊断、装饰后仍是可传参的一等值。
    - 验证：Windows 全量 **2126/2126 PASSED**；WSL2 Ubuntu 全量 **2123 passed / 2 skipped**（跳过为既有 Windows CRT 数据符号用例）。

82. **装饰器语法 `[[<expr>]]`**（2026-10）
    - **需求（用户提出）**：语法 `[[<expr>]]`，非泛型场景支持两类装饰器——
      全局变量（`[[ann]] var a = 123;` ≡ `var a = ann(123);`）与函数（含 struct
      方法，把源方法传入、返回新方法）。装饰器**必然是 comptime 的**（用户明确）。
    - **语义（前脱糖展开，纯 AST 改写）**：
      * `[[d]] var a = v;` → `var a = d(v);`；多个装饰器与 Python 一致
        （`[[a]] [[b]] f` ⇒ `a(b(f))`）。
      * `[[d]] func f(...)` → 源函数改名 `__decor_N_f` + `var f = d(__decor_N_f);`
        ——`f` **仍是函数**（可递归/可作值），装饰在模块初始化期做一次。
      * `struct S { [[d]] func m(self: ref *S, ...) }` → 成员变"包装方法"
        （`return __decorval_N_m(self, ...);`），原方法体搬到模块级
        `__decorsrc_N_m`，装饰值 `var __decorval_N_m = d(__decorsrc_N_m);`
        ——方法必须留在方法表里（`obj.m()` 走 MEMBER_CALL）。
      * 合成名一律 `__` 前缀（保留命名空间，用户不可声明）。
    - **解析**：`[[<expr>]]` 前缀（可叠），`parse_decorator_prefixes` 在顶层与
      struct 成员循环开头调用；只允许 var/func（其它声明明确报错）；`[[` 未以
      `]]` 闭合是**语法错误**（不会被吞成普通声明）。
    - **fmt**：var / 顶层函数 / 成员三处渲染都保留 `[[...]]`（漏渲染会静默丢掉
      装饰语义）。
    - **只允许顶层**（用户补充要求）：局部变量/局部函数上的装饰器**明确拒绝**
      （`decorators [[...]] are only allowed on top-level 'var' / 'func'
      declarations (and struct members), not on local declarations`）——装饰器展开是
      "全局绑定 + 模块初始化期折叠"，函数体里没有对应形态。
    - **测试**：`tests/decorator_test.cpp` 12 例——全局变量、多装饰器顺序、顶层
      函数（装饰一次 + 仍是一等值）、struct 方法、普通函数（可折叠）也能当装饰器、
      不可折叠装饰器报编译期错误、非 var/func 声明拒绝、未闭合 `]]` 语法错误、
      fmt 保留前缀。
    - **新增示例**：`examples/decorator/decorator.cx`（四种形态一次演示，
      Windows/Linux 均输出 `a=42 b=8 dbl(5)=110 p.get()=7`）。
    - **修掉的两个真实缺陷**（方法装饰器返回"带 ref 参数闭包"时暴露，\`decor_all\`
      组合用例才触发）：
      1. 捕获读取在第一次 walk 时被改写成 \`__ctx.env.<cap>\` 并**就地写回 AST**；
         同一函数体随后被再次类型检查（字面量重复 walk）时按符号查找 \`__ctx\`
         → 误报 \`undefined variable '__ctx'\`。修法：\`ex_ident\` 对**生成节点**
         \`__ctx\` 不走作用域查找（它只存在于运行期调用帧），直接按当前函数签名
         给 shadow。
      2. 具名字面量在 CTFE 里**重复折叠**（sema 试跑轮 + 正式轮）时再次
         \`scope_define\` 同名 → \`duplicate variable definition\`。修法：已存在则
         \`scope_set\`（幂等）。
      两处修完，"值装饰 + 函数装饰 + 方法装饰"同文件共存可跑。
    - 验证：Windows 全量 **2138/2138 PASSED**；示例 50 个 `.cx` 全过；fmt 幂等
      （`[[...]]` 保留）；WSL2 Ubuntu 全量 **2135 passed / 2 skipped**。

83. **调研 + 实现：带参数的装饰器 `[[dec(args)]]`**（2026-10）
    - **调研问题（用户提出）**：comptime 函数作为 comptime 函数的返回值可能吗？
      理论上这是带参数装饰器的基础。
    - **调研结论（实测）**：
      1. **comptime 函数目前不是一等值**——`comptime func id(): func(i32)->i32
         { return twice; }` 报 `ctfe: comptime function 'twice' cannot be used
         as a value`；`comptime` 是声明标记（`ast_func_def_t.is_comptime`），
         不是类型的一部分，类型系统里没有"comptime 函数类型"。
      2. **返回普通函数/闭包是可行的**（常量闭包，见第 81 项）。
      3. **带参数装饰器并不需要第 1 点**：只需要"装饰器表达式在编译期可折叠"。
         实测 `add(100, src)`（源函数作为最后一个实参）在**今天的 CTFE** 上
         就能折叠成常量闭包。
      4. 另一条路（"让 comptime 函数返回装饰器闭包，再在编译期调用它"）目前
         卡在 `ctfe: function id N is not a clux function definition`——CTFE 的
         调用路径按 id 查 `sema_func_by_id`，而 comptime 体内创建的字面量不登记
         进 sema 函数表（只有 fid）。试过加宽该路径（用 `fn->ct_literal` 回退 +
         从 env 绑捕获），仍卡在嵌套闭包创建时捕获绑定拿不到调用帧作用域，
         故本轮**不采用**该路线。
    - **采用的设计**：`[[dec(args)]] decl` ⇒ **`dec(args, <decl>)`**——被装饰的
      声明恒为**最后一个实参**（`[[dec]]` 是零参数情形，规则统一）。纯前脱糖
      改写，零 CTFE 改动、零容器格式变更。
      * 函数：`dec(args, __decor_N_f)` → `var f = ...`；
      * 值：`dec(args, <init>)`；
      * 方法：`dec(args, __decorsrc_N_m)`；
      * 链：`[[scale(10)]] [[add(5)]] f` ⇒ `scale(10, add(5, f))`。
    - **测试**（`tests/decorator_test.cpp` 增至 18 例）：带参数的函数装饰器、
      参数化链、带参数的值装饰器、带参数的方法装饰器；原有 14 例不受影响
      （非调用形态的装饰器走原路径）。
    - **新增示例**：`examples/decorator_param/decorator_param.cx`——参数化值装饰器
      （`[[plus(1)]] var v = 41`）、参数化函数装饰器（`[[add(100)]] func dbl`）、
      参数化链（`[[scale(10)]] [[add(5)]] func combo`）、参数化方法装饰器
      （`[[addm(100)]] func get(self: ref *Point)`）。Windows/Linux 输出一致：
      `v=42 dbl(5)=110 combo(1)=70 p.get()=107 p.plain()=8`；`clux format` 幂等且
      保留带参数的装饰器前缀。
    - 文档：language-spec 新增"带参数的装饰器"小节（含 comptime 函数非一等值的
      说明）；本条记录调研结论与未采用的路线。
    - 验证：Windows 全量 **2144/2144 PASSED**；WSL2 Ubuntu 全量 **2141 passed /
      2 skipped**；examples 50 个 `.cx` 全过。