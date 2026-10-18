# 所有权矩阵探针（类型族 × 操作）

本目录是"含 own 指针的复合类型"的**全量覆盖**巡检工具：把类型族与操作组合成
矩阵，逐格生成一个最小 clux 程序、逐格实跑，按 exit code / 崩溃码归类。

存在理由：这一族代码的失效形态要么是**段错误**要么是**内存泄漏**，点状抽查
（"每种类型挑一个用例"）会漏掉整整一类组合。2026-10-13 用它一次跑出 19 格
崩溃（全部集中在赋值与函数边界），修完六处根因后降到 0 格。

## 用法

```sh
# 1) 生成矩阵（默认写到 <repo>/build/tmp/matrix）
python tools/ownmatrix/gen_matrix.py

# 2) 批量实跑并汇总（输出矩阵表 + 坏格清单 + REJECT 明细）
python tools/ownmatrix/run_matrix.py
```

两个脚本都只用标准库，路径按仓库根写死（`D:\projects\clux\...` 形式在脚本内的
`DIR` / `CLUX` 常量里，换机器时改这两处）。

## 矩阵维度

**类型族（7）**

| 族 | 类型 | 覆盖点 |
|---|---|---|
| `struct` | `struct T { p: own *i32; n: i32; }` | 直接 own 字段 |
| `nested` | `T { inner: I; n }`，`I { p: own *i32 }` | 一层内嵌 |
| `deep` | `T { m: M; n }`，`M { i: I; n }` | 两层内嵌 + 中间层标量 |
| `tuple` | `.<own *i32, i32>` | 元组元素 |
| `array` | `[2]I` | 数组元素（多个 own 块） |
| `union` | `union T { p: own *i32; n: i32 }` | tag union 的激活 member |
| `optinner` | `?I` | option 内层含 own（**当前已知未支持**，见下） |

**操作（17）**

`01` 显式标注构造 · `02` 推断构造 · `03` move 到局部 · `04` clone 到局部 ·
`05` move 后丢弃 · `06` 直接丢弃构造 · `07` 内层作用域退出 · `08` 赋值 move ·
`09` 赋值 clone · `10` 作数组元素 move · `11` 作数组元素 clone ·
`12` fatal 形参 · `13` fatal 返回 · `14` 具名实参 move 传入 ·
`15` fatal 形参原样 return（转发） · `16` 二次 move · `17` move 后 clone

> 2026-10 更新：显式标注用 **`own T`**（含 own 槽位的构造产 FATAL，NONE 位置
> 接不住——见 docs/ownership-refactor.md §0.4）；形参位置用 **`own T`**
> （`fatal` 描述的是调用表达式的值，不是形参，§0.5），`fatal` 只用于返回位。

每格断言**字段值**（`a.p.* == 7 && a.n == 3` 之类），所以"跑通"不只是没崩，
值也必须对。

## 结果分类

| 类别 | 含义 |
|---|---|
| `ok` | exit 0 且 stderr 无泄漏报告 |
| `REJECT` | exit 1 —— **需人工判定**：合理的语义拒绝（如类型层临时限制）还是真缺口 |
| `LEAK` | exit 0 但 stderr 有 `memory leak` —— 销毁序列漏了 own 槽位（**真 bug**） |
| `ACCESS_VIOLATION` / `HEAP_CORRUPTION` / … | 崩溃码，**真 bug** |
| `TIMEOUT` | 挂起（可能是析构死循环） |

`run_matrix.py` 会把非 `ok` / 非 `REJECT` 的格子单独列出来（那才是必须修的），
并把所有 `REJECT` 的首条诊断附上供判定。

## 闭包 / defer 捕获维度

`closure_matrix.py` 追加一个维度：**族 × 捕获写法**（7 族 × 7 种写法 = 49 格），
每格除断言字段值外还检查泄漏报告。

| 写法 | 内容 |
|---|---|
| `C1` | 纯 id 捕获 |
| `C2` | 括号捕获 + `move`（**唯一能捕获含 own 聚合的形态**：`(bb: own T = move(a))` + `var f = move(...)`） |
| `C3` | 捕获后把源 `move` 走 |
| `C4` | 捕获后再 `clone` 源 |
| `C5` | 同时捕获两个含 own 的值 |
| `C6` | `defer` 捕获（复用同一套捕获解析） |
| `C7` | 块内捕获（闭包更早释放） |

当前结果：**C2 在 struct/nested/deep/union 四族 `ok`**；其余写法是**设计上应拒绝**
的形态——纯 id 捕获 `own` 聚合 = copy（§9.1 明令禁止，必须 `(bb = move(a))`），
所以它们报 `DIAG`（诊断）而不是缺陷。`tuple` / `array` / `optinner` 族仍被类型层
限制挡住（见下）。

> 附注：`closure_scope` 的生命周期**并非无人管理**——`vm_destroy` 会对每个函数
> 调 `scope_destroy(vm, &fn->closure_scope)`。此前的捕获泄漏根因是
> `value_own_copy_into` 没有回收 `ptr_clone_block` 的中间块（每构造一个含 own 的
> 字段泄漏 8 字节），**不含闭包的程序同样泄漏**；见 `minimal_probe.py` 的对照。
> 2026-10 的另一处：捕获槽销毁原来只发 `FREE f.<cap>`，对**聚合捕获**是空操作
> （内嵌 own 堆块全泄漏）；现在按捕获槽的 type 对象展开（`ast_var_def_t::resolved_type`）。

## 当前已知缺口（2026-10 复查②）

矩阵 119 格的最新结果（`python tools/ownmatrix/run_matrix.py`）：

| 族 | 结果 |
|---|---|
| 全部 7 族（`struct` / `nested` / `deep` / `tuple` / `array` / `union` / `optinner`） | **17/17 ok**（含 10/11 的数组元素 move/clone 两格） |

**零崩溃、零泄漏**（`LEAK` / 崩溃码均为 0）。闭包维度：C2（move 捕获）在 7 族
全部 `ok`；其余六种写法按 §9.1 设计上应拒绝（纯 id 捕获 own 聚合 = copy）。

1. **`cunion` 不参与所有权运算**（设计如此，2026-10）：成员**可以是指针/切片**
   （类型层限制已放开），但无 tag → 不生成销毁序列、不产 FATAL、
   `type_has_own_slots(cunion)` 恒 false。指针成员的释放途径见下条
   （`_ = move(c.p)`）。
2. **union / cunion 成员 move 豁免**（2026-10）：“局部 = 整体”的内存布局让
   `move(u.a)` / `_ = move(c.p)` 合法——整体进 TDZ，作用域退出不再销毁整体。
   这是 **cunion 指针成员的官方释放途径**（无 tag、不自动销毁）。经指针
   （`p.a`）或嵌套结构（`s.u.a`）仍拒绝（整体 TDZ 会泄漏堆块/漏掉兄弟 own
   槽位）。配套修了“move 后重新赋值”的二次释放（`STORE_REINIT`）。
2. `clone` 只深拷贝 own 槽位（切片缓冲区总是深拷贝，元素按槽位所有权决定是否
   递归克隆），ref 槽位保持借用指针——与销毁序列同一套规则。
3. **clone 已扁平化**（2026-10）：`clone(x)` 由 sema 在编译期展开为构造 / NEW /
   循环序列（`NEW` 只申请内存、`CONSTRUCT` 在给定 dest 上构造），运行期不再按
   类型递归；`CLONE` 指令只剩叶子用途与闭包/comptime 回退。

2026-10 本轮补上的缺口（已修复，回归见 `tests/own_aggregate_test.cpp`）：
`tuple` 逐元素、`[N]own *T` / `[N]Box` 运行期 for、`own []own *T` / `own []Box`
逐元素循环、`?own *T` / `?Box` 的 `if (v != nil)`，以及**赋值点旧值销毁**
（`p = new …` / `a[i] = …` / `p.* = …` —— A 方案 §2.3 的"旧块摘挂"）。
配套：新直接形态节点 `AST_OPT_IS_SOME`（`!= nil` 的 op 是 token_t*）与
`AST_FOR_INDEX`（u64 计数器循环）；`array_type_t::elem_ownership` /
`slice_type_t::elem_ownership` / `option_type_t::inner_ownership` 进类型描述与
去重键；hoist 对 array/slice/option 也发 `SET_FIELD_OWNERSHIP`；`MAKE` 指令带
元素所有权立即数；`clone` 的切片分支总是深拷贝缓冲区（浅拷贝会双释放）。

### 已修复（留档，避免回退）

0. **tag union 携带 own 指针**（2026-10）：放开 `union_member_t` 的指针/切片限制
   后，sema 在 Pass 3b 回走自己生成的销毁语句 → `AST_IF` 消费 3a 未建的作用域 /
   `AST_IS_TAG` 不是用户表达式（`scope tree desync` / `unhandled expression node`，
   更早表现为栈溢出）。修法：`ast_node_t::is_generated` 标记生成节点、`walk_stmt`
   跳过。配套：`own *聚合` 的销毁序列补 `AST_DEREF`（`v.f` 不能直接对指针发
   FIELD_GET/IS_TAG）；`new U{...}` 走 tag 哨兵 + CONSTRUCT；运行期"覆盖旧值"
   按槽位所有权释放（`ptr_free_slot_value`）；`union`/`cunion` 去重键补 ownership；
   hoist 用新指令 `SET_FIELD_OWNERSHIP` 把槽位所有权带进运行期类型对象；闭包
   聚合捕获槽按 type 对象展开销毁。回归：`tests/tag_union_own_test.cpp`（26 例，
   每例断言 exit 0 + 无泄漏）。
1. **`optinner` 全列**（原：`cannot initialize optional ?I with fatal I`）
   两层根因：sema 的 option 字段类型比对没把 fatal 折回 base；而折回时"按同布局
   重新 intern"对**具名 struct 不可靠**（标了 `unique`、不参与去重，每次新建实例）。
   修法：`type_fatal_variant_of` 创建变体时登记 `fatal → base`（`vm->fatal_bases`），
   折回查登记表拿规范身份。见 `docs/m3-design.md` §7.4。
2. **`13_fatal_return` 的 `array` / `union` 两格**（原：运行期
   `array: implicit cast only within layout-compatible array/tuple`）
   真正的原因**不是**元素类型不统一，而是返回值路径上**第 6 步**（safe_cast 到声明
   返回类型）抢先做了一次 `base → fatal` 的 cast —— 那是**非法方向**（各族
   `implicit_cast` 只认 `fatal → base` 的接管）。修法：声明返回类型 fatal 且带资源时
   第 6 步让开，交给第 7 步 `value_move` + `value_relabel_into`。
   struct / 指针族此前靠布局兜底偶然通过。见 `docs/m3-design.md` §7.5。
3. **`13_fatal_return` 的 8 字节泄漏**：随提交 `0732858` 修复
   （`value_own_copy_into` 未回收 `ptr_clone_block` 的中间块）。
4. **字段级 move 漏放**：`move(s.p)` / `move(o.i.p)` 此前全部放行 —— TDZ 是符号级的
   （`sema_symbol_t.moved`），字段没有"局部 TDZ"，同一字段可重复 move、被读成空值。
   修法：`ex_move` 在操作数是字段/元素访问且类型 own/fatal/含 own 资源时拒绝。
5. **闭包捕获 move 漏放**：`move(q)`（q 为 `func |(q = move(p))|` 的捕获槽）此前放行 ——
   捕获作用域由闭包所有调用共享，move 进去的 TDZ 无法恢复。修法：捕获解析时给符号打
   `is_capture` 标记（不能用 `own_scope` 反查：括号捕获的参数槽 `own_scope == NULL`）。
   见 `docs/m3-design.md` §7.6。

### 尚未拦截（sema 侧，非缺陷但规则不齐）

捕获 **`own *T` 裸变量**已被拦（`cannot capture own/fatal pointer … use move()`）。
**含 own 槽位的聚合按值捕获（纯 id）**同样已被拦——`resolve_func_captures` 用
"捕获槽也是具名位置"的规则：非 own 捕获槽收 FATAL 报 `must be an own capture`，
copy OWN 聚合报 move-only。要捕获含 own 的聚合只有一种形态：
`var f = move(func |(bb: own T = move(a))| ...)`（见闭包矩阵 C2）。

### ref 聚合的作用域检查（另一个维度，见 `docs/m3-design.md` §5/§5.1）

含 ref 字段的聚合类型（struct/tuple/array/union）在三处强制/校验作用域标注：

| 位置 | 规则 | 状态 |
|---|---|---|
| 变量声明 | 含 ref 字段**必须标注**（`'<a>` / `'<*>`） | ✅ `check_var_annot` |
| 参数位 | 同上（顶层 `ref *T` 除外） | ✅ 参数检查 |
| 返回位 | 同上 | ✅ `sema_check_ret_ref_annot` |
| **赋值** | 标注 `'<A>` = "借的东西至少活到 A" → 要求 `借源作用域 ⊇ A` | ✅ `borrow_set` provenance（§5.1） |

**方向极易写反**：`'<*>` 是**最强**承诺。赋值的五条方向性用例
（`tests/driver_m3_test.cpp` 的 `Driver.StepBAggregate*`）：

| 用例 | 期望 |
|---|---|
| `g: '<*> H = .H{.r = p}`（承诺活到全局，实际只借到 p） | 拒绝 |
| `g: '<p> H = .H{.r = inner.&}`（只借到更内层块局部） | 拒绝 |
| `g: '<p> H = move(h)`，h 只借到块局部（继承借源） | 拒绝 |
| `g: '<p> H = .H{.r = p}`（同锚点） | 通过 |
| `g: '<p> H = .H{.r = gx.&}`（承诺比借源短） | 通过 |

复现用例：`repro/own-compound/ref_agg_assign_scope.cx`（通过侧）与
`ref_agg_assign_escape.rejected.cx`（拒绝侧）。

> 矩阵本身不覆盖 ref（矩阵面向 own 所有权扩散）；ref 的覆盖在
> `driver_m3_test.cpp` 与 `defer_test.cpp`。

### own 的 move 边界（再一个维度，见 `docs/m3-design.md` §7.6）

TDZ 是**符号级**的，由此推出两条约束（此前都漏放，已修）：

| 情形 | 判定 |
|---|---|
| `move(s.p)`（聚合的 own 字段单独 move） | ❌ 字段无局部 TDZ，可重复 move |
| `move(o.i.p)`（嵌套字段） | ❌ 递归生效 |
| `move(s)`（整体 move 聚合） | ✅ 转移的唯一正道 |
| `clone(s.p)`（字段 clone） | ✅ 不改变源的绑定状态 |
| `move(q)`，q 是闭包捕获槽（`func \|(q = move(p))\|`） | ❌ 捕获作用域由所有调用共享，TDZ 不可恢复 |
| 闭包内 `move(c)`，c 是捕获的结构体 | ❌ 同上 |
| 闭包体内**自定义**的局部变量 move | ✅ TDZ 随该次调用消亡 |

复现：`repro/own-compound/move_whole_only.cx`（正例）与
`move_field_and_closure.rejected.cx`（反例）；测试 `Driver.StepBStruct*Move*` /
`Driver.StepB*ClosureCapture*` / `StepBWholeStructMovePasses` 等 8 例。

