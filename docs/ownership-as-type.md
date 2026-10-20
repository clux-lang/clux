# 所有权升级为类型构造（重构方案，v0）

> **动机（用户提出）**：每个值必有类型，所以所有权应该和 `const` / `volatile` 一样是
> **类型的组合**，而不是 `value_t` 上的状态。这样它自然进入反射与泛型体系，也能消掉
> 现在一堆"所有权不在类型里"导致的特殊处理和纠结。
>
> **现状（代码事实）**：`ownership_t` 记在 `value_t` 上（`include/vm/ownership.h`），
> 由 `value_set_ownership` / `value_ownership` 读写（22 + 27 处调用）；类型层只有
> **槽位**所有权：`struct_field_t.ownership`、`tuple_elem_t.ownership`、
> `union_member_t.ownership`、`cunion_member_t.ownership`、`option_type_t.inner_ownership`、
> `slice_type_t.elem_ownership`、`array_type_t.elem_ownership`、`func_sig_t.param_ownerships[]` /
> `return_ownership`。AST 侧靠 `sema_type_expr_ownership`（29 处）从 token 里捞 `own`/`ref`。
> 特殊处理散布在：`func_sig_eq` 手比所有权、extern 签名规则、cfunc→func 包装的 `move`、
> `type_has_own_slots` 手写递归、`OWN_FATAL` 只存在于 sema、反射被禁止暴露所有权（因为那是
> 值的状态）……

---

## 1. 目标模型：所有权是类型修饰符

与 `const` / `volatile` 同构（`TYPE_KIND_CONST` / `TYPE_KIND_VOLATILE` + `sub` 字段 +
`type_qualifier_sub` + 布局同步 + intern 去重）：

```
TYPE_KIND_OWN    { sub }   拥有资源：move-only，作用域结束由持有者销毁
TYPE_KIND_REF    { sub }   借用：持有者不销毁，不得让引用活过被引用者
TYPE_KIND_FATAL  { sub }   将亡：必须被消费一次（move 出去或显式销毁）
```

- `type_qualifier_sub` 一并覆盖 `own/ref/fatal`（限定链统一剥）。
- 布局 = `sub` 的布局（同 const/volatile），`type_sync_derived_layout` 加入新池。
- intern 去重按 `(kind, sub)`；`own *Node` 与 `*Node` 是**两个不同类型**，
  于是 `func_sig_eq`、`struct` intern、`field_type(i)` 全部自动正确。

### 语义格（待确认，§6-Q3/Q4）

| 类型 | 可拷贝 | 作用域结束 | 典型来源 |
|---|---|---|---|
| `T` | ✓ | 无动作 | 字面量、值类型 |
| `own T` | ✗（只能 move） | 销毁（FREE / 递归销毁） | `new T{…}`、`own` 参数、`own` 字段 |
| `ref T` | ✗（借） | 不销毁 | `x.&`、`ref` 参数、`data()`、切片元素 |
| `fatal T` | ✗ | **必须已消费**，否则报错 | `move(x)`、`clone` 中间值 |

转换（编译期，零指令）：
`T --move--> fatal T`、`own T --move--> fatal T`、
`fatal T --绑定/实参--> own T`、`ref T --clone--> own T`、
`fatal T --free--> void`。

---

## 2. 会消失的"割裂点"（重构收益）

| 现在 | 之后 |
|---|---|
| `value_t.ownership` 字段 + `value_set_ownership` / `value_ownership`（49 处） | 删除；销毁决策改为**按静态类型**在编译期决定 |
| `value_is_borrowed()` 运行期判据 | 改为按类型判定（`type_is_ref(t)`），或直接由编译期生成的销毁序列决定 |
| `sema_type_expr_ownership`（29 处）从 token 捞所有权 | 类型解析直接产出 `own T` / `ref T` / `fatal T` |
| 8 处槽位所有权字段 | 全部编码进成员的类型（字段类型 = `own *Node` 等） |
| `func_sig_eq` 手比 `param_ownerships` / `return_ownership` | 类型相等即包含所有权，手比逻辑删除 |
| `type_has_own_slots` 手写递归（struct/union/tuple/option/slice/array 各分支） | 只看 `type_is_own` / `own` 修饰是否存在 |
| extern `own` 参数/返回值规则、cfunc→func 包装的 `move` 判断 | 统一从参数/返回类型读 |
| 反射**禁止**暴露所有权 | `T.kind == "own"`、`T.ownership == "own"`、`T.unqualified` 剥出底层类型——现在它是**类型信息**，不再是冗余值状态 |

---

## 3. 实施阶段（每阶段全绿 + 独立提交）

### P0 基础设施（纯增量，不改语义）
- 三个新 kind + 三个类型池（`vm->own_types/ref_types/fatal_types`）+ intern + 访问器
  （`type_is_own/ref/fatal`、`type_ownership_of`、`type_qualifier_sub` 扩展）。
- 布局同步、`type_kind` 字符串名、反射 key：`kind` → `"own"`/`"ref"`/`"fatal"`，
  新增 `ownership`（`"own"/"ref"/"fatal"/"none"`）与 `unqualified` 剥修饰。
- 此阶段不构造任何新类型 → 行为零变化；测试覆盖 intern/剥修饰/反射 key。

### P1 类型解析产出所有权类型（并行期）
- `sema_resolve_type_slot` / 类型表达式解析：`own T` → `own T` 类型；函数参数、返回、
  字段、`?T`、切片/数组元素、`cfunc(...)` 签名全部按类型记录。
- 与此同时**保留** value-state 机制照常工作（两套并行）——测试断言
  `T.ownership` / `T.kind` 正确，且既有语义（2226 例）不变。
- 槽位字段暂留（写入等于类型里的所有权），便于逐步切换。

### P2 切换判定到类型
逐块把"看值状态/AST"改成"看类型"：
1. sema 的 move/fatal 消费检查、借用逃逸、拷贝禁止；
2. 销毁序列（`gen_destroy_impl` / `type_has_own_slots`）只读类型；
3. FFI 规则（`ffi_c_abi_reject_reason_own`、extern `fatal` 返回）；
4. cfunc→func 包装的 `move(p)`；
5. option/切片/数组元素销毁。
每块独立跑全量，出问题单独回滚。

### P3 删除旧机制
- 删 `value_t.ownership`、`value_set_ownership`、`value_ownership`、`value_is_borrowed`、
  `sema_type_expr_ownership`、8 个槽位所有权字段、`func_sig_eq` 所有权比较。
- 反射把 `ownership` / `unqualified` 正式纳入 key 表（含 `field_type(i).ownership` 链式）。
- 更新文档（language-spec 所有权章节、architecture、reflect 设计 §3.10 改写为"所有权是
  类型修饰符，故可反射"）、milestones。

---

## 4. 最大风险：运行期借用标志

`OWN_REF` 在运行期用于"不要释放"（`value_is_borrowed`，60 处 `OWN_REF` 引用）。改成
类型后，**每个**销毁点都必须能从静态类型决定：绑定（`var y = x.*`）要 materialize、
`data()` 返回的指针、临时值、`vec`/切片元素借用等。P2 的第 1 步会先做**站点审计**：
把 60 处分类（编译期可判定 / 需要 materialize / 真是运行期事实），再逐类替换。
如果审计发现某类确实无法静态判定，那一处保留一个**局部**运行期标记（不回到全局
`value_t.ownership`），并记录原因。

---

## 5. 兼容与验证

- 每阶段跑：Windows 全量 + examples 全量 + fmt 幂等 + WSL2 全量；提交前更新计数。
- 反射/FFI/cfunc 相关测试是主要回归面（`reflect*`、`ffi_*`、`cfunc_to_func`、`own_param*`）。
- 中间态（P1）允许两套机制并存，但**不允许**两套结论不一致：加一个 debug 断言
  `value_ownership(v)` 与 `type_ownership_of(value_type(v))` 在关键节点一致（P3 删除）。

---

## 6. 待确认决策

| # | 问题 | 建议 |
|---|---|---|
| Q1 | 表示：三个 kind（OWN/REF/FATAL 各带 `sub`）还是单 kind + qualifier 字段？ | **三个 kind**（与 const/volatile 完全同构，`type_qualifier_sub` 一行扩展） |
| Q2 | 裸 `T` 的含义 | `T` = 无所有权（可拷贝）；`own T` 是不同类型 |
| Q3 | 修饰符能否嵌套/顺序：`own const T` / `const own T` | 允许任意顺序，`type_qualifier_sub` 一路剥；intern 按 (kind, sub) |
| Q4 | `move(x)` 的结果类型 | `fatal T`；绑定/传参时变为 `own T`（消费） |
| Q5 | `ref T` 能否作为字段类型（借用存进结构体） | 沿用现有规则（逃逸检查禁止），类型层允许 |
| Q6 | 反射 key 命名：`ownership`（str）还是 `T.is_own` 布尔族 | `ownership` 一个 str，另加 `unqualified`（与 const/volatile 同一 key 名） |
