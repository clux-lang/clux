# sema / driver 缺陷复现用例（已修复，作为回归档案）

这些是最小复现程序，全部用 `build/clux.exe` 实跑验证过。**每个缺陷都已修复**，
并已转成 `tests/` 下的 gtest 回归用例（见每条的"回归"字段）。保留本目录是为了：

1. 记录缺陷的**原始触发条件**（复现程序本身就是最小规格说明）；
2. 修复回退时能立刻验证（直接 `clux run` 即可看到行为变化）。

构建：`cmake --build build`（不要 clean）。运行：`.\build\clux.exe run repro\sema-defects\<name>.cx`

---

## A. 常量折叠在部分槽位丢失 → 运行期 `exec: undefined variable` ✅ 已修

comptime 变量在运行期不存在，引用它的表达式**必须**被折叠为字面量。`sema_expr`
通过可写节点槽（`ast_node_t **`）做折叠写回；9 处调用传的是**局部变量副本**且
事后不写回 AST ⇒ 折叠丢失 ⇒ 编译通过、运行期报 `exec: undefined variable`。

| 文件 | 修复前 | 修复后 |
|---|---|---|
| `comptime_union_field.cx` | ❌ exit 1 `run: exec: undefined variable` | ✅ exit 0（输出 5） |
| `comptime_make_elem.cx` | ❌ exit 1 `run: exec: undefined variable` | ✅ exit 0（输出 `5 6`） |
| `comptime_struct_field.cx` / `comptime_array_elem.cx` / `comptime_nested_struct.cx` | ✅（对照：本就正常） | ✅ |

**修复**：`src/sema/expr.c` 的 8 处补 `n->fields = f;` / `cf->value = value;` /
操作数回写（含 `fields_last` 修正）；`src/sema/stmt.c` 的 switch 模式与 make 元素
改为**链上指针迭代**（`ast_node_t **link`），把真实链槽交给 `sema_expr`。
**回归**：`DriverM4.MakeElementsFoldComptimeVar`、`Driver.RunFileUnionFieldComptimeVarFolds`。

## B. 匿名构造字段被求值两次 → `move` 假阳性 ✅ 已修

`infer_anon_struct` / `infer_anon_tuple` 已完整求值+校验+重排字段，但调用点**漏了
`return`**，控制流继续落入具名构造分支对同一批字段表达式**二次求值**；第二次求值
看到第一次留下的 `moved` 标记 → 含 `move()` 的字段被误拒。

| 文件 | 修复前 | 修复后 |
|---|---|---|
| `move_double_eval.cx`（匿名 `.{}`） | ❌ exit 1 `variable 'p' used after move` | ✅ exit 0（输出 7） |
| `move_named_ctor.cx`（具名 `.Cell{}`） | ✅（对照） | ✅ |
| `anon_ctor_comptime_call.cx` | ✅（comptime 调用重复求值幂等） | ✅ |

**修复**：`src/sema/expr.c` 匿名构造分支末尾补 `return value_make_shadow(...)`。
**回归**：`Driver.AnonConstructFieldEvaluatedOnce`。

## C. `comptime var` 作类型槽位（数组边界）不工作 ✅ 已修（并扩展到所有类型/所有常量位置）

`docs/m2-design.md` 承诺 `var arr:[N]i32`（N 为 comptime var）合法，此前**全局与
局部都失败**；字面量表达式边界正常。

**根因**：常量槽位由 CTFE 求值，而 CTFE 的标识符解析只查 **VM 作用域链**；
comptime var 不生成运行期字节码、其常量只编码在符号表（`sym->ct`），从未落在这条
链上 ⇒ 槽位解析拿不到值。另有两处**顺序**问题：全局 `type T = [N]i32` 在 `pass1b`
解析而 comptime var 求值在 `pass2` 之后；局部 `type T = [C]i32` 在 3a/块提升预扫
解析而局部 comptime var 求值在 3b。

**修复**：
1. `sema_eval_comptime_var` 求值成功后把**常量真实值绑定进 VM 作用域**
   （全局 → `vm->root_scope`，与全局变量 shadow value 同处；局部 → 当前块作用域）；
2. 新增 `pass_comptime_early`：`pass1b` 之前"试跑"全局 comptime var（临时诊断缓冲，
   成功才提交、失败留晚期正式报错）→ 覆盖 `type T = [N]i32` 与签名 `[N+1]i32`；
3. `walk_block` 提升预扫按源序求值局部 comptime var（并把它排除出 `prior_vars`
   遮蔽预检——comptime var 是编译期常量，不遮蔽类型名）；
4. `stmt_build` 把"含非字面量数组边界 + 作用域内有未求值 comptime var"的槽位推迟到 3b
   （否则 3a 的硬诊断触发快速失败闸门使 3b 不运行）；
5. 类型支持扩展到 struct/tuple（`sema_ct_encode`/`sema_ct_lit` 复用 `AST_CONSTRUCT`
   + CTFE 的 `ctfe_make_aggregate`），至此**支持所有类型**。

| 文件 | 修复前 | 修复后 |
|---|---|---|
| `b1_expr_2p3.cx`（`[2+3]i32`）/ `b2_sizeof.cx` | ✅（对照） | ✅ |
| `b3_ct_global.cx` / `global_ct_bound2.cx`（全局 comptime var 作边界） | ❌ `array length must be a compile-time constant` | ✅ |
| `b4_ct_local.cx` / `local_ct_bound2.cx`（局部） | ❌ 同上 | ✅ |
| `g1_global_undef.cx` / `g2_global_fill.cx` / `l1_local_undef.cx` / `l2_local_fill.cx` | ❌ 同上 | ✅ |
| `g3_typealias.cx`（全局 `type T = [N]i32`） | ❌ `expression is not a compile-time type computation` | ✅ |
| `l3_local_type.cx`（局部 `type T = [C]i32`） | ❌ `'C' is a variable, not a type` | ✅ |
| （新增用例）`[N+1]i32` / 签名槽位 / `make(i32, N, …)` / `sizeof` | — | ✅ |
| （新增用例）struct / tuple / 枚举 / 数组 / 字符串 / 浮点 / 布尔常量 | struct/tuple ❌ | ✅ 全部支持 |

**求值顺序无关**（后续补齐）：全局编译期实体之间存在真实依赖环
（`comptime var N = add(1,2); type T = [N]i32; func f(a: T)`），单趟顺序无解 →
`pass1b_types`/`pass2_types`/`pass_globals` 改为可反复调用的"试跑轮"，直到不再有
新实体就绪（诊断进临时缓冲丢弃），最后用真实诊断报错。常量位置因此**与书写顺序
无关**；CTFE 调用普通函数（常量槽位）也按 AST 解释执行。

**回归**：`tests/driver_comptime_test.cpp`（14 个用例，含晚期求值/逆序书写）。

## D. `<v, N>` fill 落在 struct 字段位 → 静默放过 ✅ 已修

`sema_expr` 没有 `case AST_FILL`（落 `default` 返回 void shadow），struct 构造
路径也不检查 fill ⇒ fill 被当"一个匿名字段"计数，最终以误导性的字段数诊断或
编译器的 `unsupported expression node 'fill'` / `'tuple'` 暴露。

| 文件 | 修复前 | 修复后 |
|---|---|---|
| `fill_in_struct.cx` / `fill_one_field.cx` / `anon_fill.cx` | ❌ `expected 2 fields for S, got 1` / 编译器内部文案 | ✅ 明确诊断 `construct: fill element '<value, count>' is not allowed in struct constructors` |
| `fill_one_field_anon.cx`（`.a = <5,3>` 具名字段放元组） | ❌ `internal: unhandled expression node 'tuple'` | ✅ `unsupported type expression`（`<5,3>` 是元素非类型的元组**类型**） |

**修复**：
1. `sema_expr` 补 `case AST_FILL`（明确诊断）与 `default:`（报内部错误，不再静默）；
2. struct 构造两条路径（AST_NEW / AST_CONSTRUCT）与 `infer_anon_struct` 在字段数
   检查**之前**显式拒绝 fill；
3. 连带补齐 4 种**类型表达式**在表达式位置的分支（`AST_TUPLE` / `AST_FUNC_TYPE` /
   `AST_OPTION` / `AST_PTR`）——它们此前同样落 `default` 被静默吞掉，
   `var x = <i32,i32>;` 之类合法写法会失败。

**回归**：`SemaTest.FillInStructConstructorRejected`、
`SemaTest.TypeExpressionsAreValuesInExpressionPosition`、
`Driver.RunFileTypeExpressionsAsValuesPass`。

## E. 被导入模块内的 struct 构造运行期失败（M5） ✅ 已修

| 场景 | 修复前 | 修复后 |
|---|---|---|
| `module_struct_main.cx` → `module_struct_lib.cx` | ❌ `run: construct: unsupported type (...)` | ✅ exit 0（输出 9） |
| 入口文件内 struct 构造 / 模块内数组构造 / 标量 | ✅（对照） | ✅ |

**根因**：`sema_type_register` 按 `TYPE_ID_PROGRAM_BASE + 模块内下标` 分配 id，而
运行期 `vm->types_by_id` 是**全 VM 单表**（多模块共享同一 vm），模块字节码用
`LOAD_TYPE <id>` 直接索引 ⇒ 两模块 id 空间重叠，后编译模块的 `DEFINE_TYPE` 覆盖
先者槽位，先者取到别的模块的类型。数组之所以正常，是因为它走 `PUSH_ARRAY` 内联
重建类型、不查 id 表。

**修复**：program 类型 id 改为**全 VM 全局唯一**——新增
`vm_type_id_alloc`（`src/vm/type.c` + `include/vm/type.h` 的 `type_id_alloc`，
计数器 `vm.type_id_next`）；`sema_type_register` 与编译器临时分配
（`compile_type.c` 的 `temp_type_id`）统一走它；`sema_type_t` 新增 `index` 字段，
hoist 区的 `done[]` 三态表由"id - BASE"改为**按登记下标**索引。

**回归**：`DriverImport.StructConstructInsideDependencyModule`、
`DriverImport.ProgramTypeIdsUniqueAcrossModules`（两模块各含 struct/union/enum/数组
+ 入口模块自己的 struct）。

## F. `bc emit` / `build -asm` 编译失败路径泄漏整个 sema 作用域树 ✅ 已修

| 命令 | 修复前 | 修复后 |
|---|---|---|
| `clux bc emit sema_error_for_leak.cx -o out.cxb` | ❌ exit 1 + stderr **42 条 leak**（`sema_scope_t`/`sema_symbol_t`/`strmap_t`/`rbtree_t`/`vec_t`/`str`） | ✅ exit 1，**0 条 leak** |
| `clux bc emit <正常文件>` | ✅ 0 leak（对照） | ✅ |

**根因**：`driver_compile_to_bytecode` 的三条失败路径调用 `driver_compiled_dispose`
时 `out->scope_tree` 尚未赋值（成功路径末尾才赋值），而 `sema_destroy` 明确不释放
作用域树。
**修复**：三条失败路径各补 `if (scope_tree) sema_scope_destroy(&scope_tree);`。
**回归**：`Driver.CompileErrorPathDoesNotLeakSemaScopeTree`（捕获 stderr 断言无
`memory leak detected`）。

## G. 顺带修掉的静默漏洞：for 步进的赋值从不做类型检查 ✅ 已修

`walk_for` 把 update 槽无条件交给 `sema_expr`，而 update 可放**赋值语句**
（`j = j + 1`）——`AST_ASSIGN` 落入 `sema_expr` 的 `default` 被静默返回 void
shadow ⇒ 步进赋值从未类型检查（`j = "x"` 也能编译通过）。`default:` 改为报错后
立刻暴露；现 update 与 init 同路走 `shadow_assign`。
**回归**：`SemaTest.ForUpdateAssignmentTypeMismatchRejected` + `ForUpdateValidAssignmentPasses`。

## H. `anon_ct` 目标类型栈 push/pop 不对称 ✅ 已修

12 处写成"有条件压栈 + 无条件弹栈"（类型为 NULL 或栈满 16 层时压栈被跳过而弹栈
照做）⇒ 深度计数与栈内容失配 ⇒ 后续 `.{}` 推断到**外层**的目标类型。
**修复**：新增配对接口 `sema_anon_push` / `sema_anon_pop(sema, token)`（`sema.h`），
全部 16 处调用点统一改用；裸 `anon_ct_depth++/--` 已归零。
**回归**：`SemaTest.AnonCtPushPopTokenIsSymmetric`。

## I. 死状态清理 ✅ 已完成

| 项 | 证据 | 处理 |
|---|---|---|
| `vm->comptime` | 16 写、**0 次真读**（8 对 save/restore 纯仪式） | 字段与全部仪式删除（含注释改写） |
| `sema->reexports` | 2 写 0 读（本人 M5 中途改设计留下的死字段） | 删除（`vec_new`/`vec_free`/`vec_push` 一并去掉） |
| `sema->func_has_return` | 5 写 1 声明 0 读（功能已被 3a 的 `definitely_returns` 取代） | 删除 |
| `sema_func_t::name` | 2 写 0 读 | 删除 |
| `sema_scope_t::frame` | 1 写 0 读（VM 侧 `scope_t::frame` 才是活的） | 删除（含 `symbol.h` 的死 include） |
| `compiler_t::current_scope` | 2 写 0 读 | 删除 |
| ~~`flow_snap.after`~~ | **报告称只写，实测在用**（`stmt.c:1547/1634/1664` 等） | **保留** |

## 未能证实的报告结论（记录以免被当作事实）

- **nil 比较绕过 TDZ/moved 检查**：对照实验显示"移动后的 POD 可选值普通读取"也
  不报错，无法区分"nil 特有问题"与"POD 的 move 本就是空操作"。
- **`flow_snap.after` 只写**：实测被读取，报告有误。
- **`t->id` 回写导致跨模块潜伏缺陷**：机理未证实，但 E 项确实暴露了模块场景下的
  真实类型问题（已修）。
