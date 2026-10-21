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

## 2.5 运行期语义：**剥离 / 穿透，零行为**（用户澄清）

所有权修饰符是**纯编译期**概念：运行期把它们**剥掉**，只看底层 `sub`，不产生任何
行为差异。落地成三条硬规则：

1. **没有运行期标志**。`value_t.ownership` 直接删除（不是"以后再说"）——运行期的
   `value_t` 只有 `type`（裸类型）+ `data`；`value_set_ownership` / `value_ownership` /
   `value_is_borrowed` 全部消失。
2. **运行期类型永远是裸类型**。任何进入运行期的类型引用（`LOAD_TYPE` / `DEFINE_TYPE` /
   构造指令的 type_id / 字段布局 / FFI 描述符 / 值分配）都先 `type_strip_ownership`：
   `own T`、`ref T`、`fatal T` 与 `T` 在运行期是**同一个** `type_t`（同 size/align/vtable）。
   在 `value_make` / 类型指令入口加 debug 断言"运行期类型不带所有权修饰"。
3. **所有"要不要销毁/要不要拷贝"的决策都在编译期**。现在的 `value_is_borrowed()` 分支
   （60 处 `OWN_REF`）不是"运行期事实"，而是"编译期没决定好"：编译器在**需要独立值时**
   发 materialize/clone（`BCODE_MATERIALIZE` 已有），否则按裸类型传指针。所以这些分支
   在 P2 逐个删除，而不是改成运行期判断。

随之而来的结构选择：`own T` 这类类型仍 intern 在 vm 类型池里（它们要参与类型相等、
反射、签名），但**运行期指令只引用剥过的类型**；两者布局相同，因此 `type_equivalence`
不受影响。销毁序列（`gen_destroy_impl` / `type_has_own_slots`）在**编译期**看未剥的
类型图决定，生成的字节码里只出现裸类型。

---

## 3. 实施阶段（每阶段全绿 + 独立提交）

### P0 基础设施（纯增量，不改语义）—— ✅ 已完成（2026-10）
- 三个新 kind + 三个类型池（`vm->own_types/ref_types/fatal_types`）+ intern + 访问器
  （`type_is_own/ref/fatal`、`type_ownership_of`、`type_qualifier_sub` 扩展）。
- 布局同步、`type_kind` 字符串名、反射 key：`kind` → `"own"`/`"ref"`/`"fatal"`，
  新增 `ownership`（`"own"/"ref"/"fatal"/"none"`）与 `unqualified` 剥修饰。
- 此阶段不构造任何新类型 → 行为零变化；测试覆盖 intern/剥修饰/反射 key。

### P1 类型解析产出所有权类型（并行期）—— 🚧 P1a 已完成（2026-10）

> **实测结论（2026-10，重要）**：P1b（把字段/参数/返回类型也换成 `own T` 等，同时
> 保留旧判定）**无法独立成立**——类型一旦真的带所有权，全仓的类型**身份比较**就
> 开始不一致：只包 struct 字段 **62 个测试失败**；再加上签名参数/返回 **108 个**；
> 在 `type_equal`/`type_extends` 里加"所有权透明"过渡桥后仍 **109 个**（因为大量
> 比较走的是指针身份 `==`、`value_implicit_cast` 的身份短路、以及各子系统自己的
> 类型断言）。
>
> 所以 **P1b 与 P2 合并**：产出与判定必须**同一提交内按子系统一起切换**：
>
> | 批次 | 内容（同一提交） | 影响面 |
> |---|---|---|
> | B1 | `option` 内层 / 切片元素 / 数组元素类型带所有权 + 这些路径的判定 | 小 |
> | B2 | struct 字段（含 union/cunion/tuple 槽位）+ 字段读写/构造/赋值判定 | 中 |
> | B3 | 函数签名（参数/返回）+ 调用/返回/FFI 判定 | 大 |
> | B4 | 删除槽位字段 + `value_t.ownership`，跑全量确认 | 收尾 |
>
> 每批都必须：产出该处所有权类型 → 把该子系统的所有类型比较改成**所有权感知**
> （剥或含）→ 全量 2232+ 绿 → 独立提交。

> **B1 实测（2026-10，进一步结论）**：只把 option 内层 / 切片元素 / 数组元素类型
> 包成 `own T` 等（判定不动）→ **24 个测试失败**；修掉 option 初始化与解引用的
> 剥比较后仍有 **23 个**，根因是**值的产生路径**还在产出裸类型：`new T{}` 返回的
> shadow 类型是 `*T`（值状态记为 own），于是 `?own *T` 的构造被拒/走了拷贝分支 →
> 泄漏或 move 判定错位。
>
> 也就是说：**类型化不能只覆盖"声明侧"（字段/签名/元素），必须连同"值的产生侧"
> （`new`、`&`、`move`、构造、隐式提升）一起切换**，因为两侧的类型要能对上。这
> 进一步说明 P2 更适合**一次性协调切换**（一个大提交/一条分支），而不是按声明侧
> 的子系统切。

- `sema_resolve_type_slot` / 类型表达式解析：`own T` → `own T` 类型；函数参数、返回、
  字段、`?T`、切片/数组元素、`cfunc(...)` 签名全部按类型记录。
- 与此同时**保留** value-state 机制照常工作（两套并行）——测试断言
  `T.ownership` / `T.kind` 正确，且既有语义（2226 例）不变。
- 槽位字段暂留（写入等于类型里的所有权），便于逐步切换。

### P2 切换判定到类型 —— ✅ 核心「产出与判定同批切换」已完成（2026-10）

P1b/B1 的实测结论是"产出侧与判定侧必须同一提交切换"。本轮按这一点一次性完成：

- **类型解析产出所有权类型**：`AST_PTR` / `AST_SLICE_TYPE` / `AST_OWNED_TYPE` 直接
  `type_ownership_apply` 出 `own T` / `ref T` / `fatal T`；`?T`、`[N]T`、`[]T`、
  元组、结构字段、联合成员、函数签名的槽位类型随之携带所有权。
- **值产生侧一起切换**：`new` / `make` → `fatal *T` / `fatal []T`；`x.&` 与
  `data()` → `ref *T`；`move` / `clone` → `fatal T`；调用表达式 = 签名返回类型
  （已含 `fatal` / `ref`）。
- **结构视图透明**：新增 `type_strip_ownership` / `type_kind_of`；`type_strip_quals`
  与全部 `type_*` 访问器（ptr/slice/option/array/tuple/struct/union/cunion/
  func/cfunc/opaque）穿透所有权修饰；sema/ctfe/compiler 的 kind 判定统一改走
  `type_kind_of`（276 处），`own T` 不再被误判为"不是指针/不是切片"。
- **所有权转换**：`value_implicit_cast` 增加 `fatal T → own T` / `fatal T → ref T` /
  `own T → ref T` 的重贴标签（布局相同，零拷贝）；反向不成立。
- **FFI**：`ffi_c_abi_reject_reason_own` / `ffi_type_for` / `ffi_data_kind_of` /
  `ffi_data_kind_needs_type` 一律先剥裸类型；所有权仍从类型读。
- **顺带修掉的真问题**：
  * `sema_eval_type_def` 与类型解析重复包装（`type R = ref *i32` 曾是
    `ref (ref *i32)`）——现在只在尚未携带时补包；
  * `cfunc → func` 包装体把已带 `ref` 的形参类型又包了一层（曾生成
    `ref **Box`）——现在直接引用带所有权的类型；
  * 函数形参/返回的所有权在 AST 折叠成 `AST_TYPE_REF` 后 token 丢失——现在
    从解析出的**类型**回读（`type_ownership_of`）。

**仍未做（P3）**：`value_t.ownership` / `value_set_ownership` / `value_ownership` /
`value_is_borrowed` 仍作为运行期借用镜像存在；8 个槽位所有权字段仍在；sema 的
move/fatal/借用判定仍从符号/值状态读取（与类型保持一致）。P3 删除这些字段并把
判定改为只读类型。

验证：Windows 全量 **2232/2232**、examples 53 个入口全过、fmt 幂等 53/53；
WSL2 全量 **2229 passed / 2 skipped**。

### P3 删除旧机制 —— 🚧 P3a / P3b / P3c / P3d 已完成（2026-10）

- ✅ **值层所有权状态已删除**：`value_t.ownership`、`value_set_ownership`、
  `value_ownership` 全部移除。值只剩 `type / data / is_shadow / borrowed / frame`。
  所有权查询改为 `value_type_ownership(v)`（由类型派生），sema 的写入改为
  `value_set_type_ownership(vm, v, own)`（同布局重贴标签）。
- 运行期只剩一个 **`borrowed` 视图位**：data 指向父值 data 块内部（字段/元素
  借用视图），只回答"dispose 时是否释放 data 块"。它不是所有权状态；
  `value_is_borrowed` 谓词暂时保留，等 P3c 与借用 materialize 改造一起处理。
- 顺带修掉：FUNC 类型被 `own/fatal` 包装后，sema 里直接
  `(func_type_t *)value_type(...)` 强转读签名会越界/读到错误签名（`clone(func…)`
  的调用返回类型变成 `fatal i32`）——所有这类强转改为先 `type_strip_ownership`。
- ✅ **P3b：函数签名的所有权数组已删除**。`func_sig_t` 只剩
  `params / param_count / return_type / is_variadic`；形参/返回所有权一律由
  类型携带（`type_ownership_of(sig->params[i])` / `type_ownership_of(return_type)`）。
  `func_sig_eq` 不再手比所有权（类型相等即包含）；`param_ownerships` /
  `return_ownership` 的分配、释放、比较与全部调用点都删掉（`type_func_sig`
  从 7 参简化为 5 参，22 处调用点迁移）。生成的 destroy/clone helper 也把
  `ref` 形参与 `fatal` 返回直接编进类型。
- ✅ **P3c：符号层的所有权副本已删除**。`sema_symbol_t` 不再有 `ownership`
  字段；所有判定改 `sema_sym_ownership(sym)`（由 `sym->type` 派生）。变量/捕获
  推断出的所有权直接写进类型（`strip_fatal`：`fatal T` → `own T`），删掉 30+
  处 `sym->ownership` 读写；`sema_fn_ctx_t.return_ownership` 也删除，返回位判定
  改读 `return_type`。捕获的作用域标注检查保留“声明侧修饰词”的旧语义
  （显式 ref / 纯 id 外层 ref / opaque 才要求标注）。
- ✅ **P3d-1：槽位所有权判定改为读类型**。销毁序列、clone 展平、构造槽位校验、
  借用源分析、`type_has_own_slots`、hoist 的 `SET_FIELD_OWNERSHIP` 全部改为
  `type_ownership_of(槽位类型)`（cunion 槽位按语义恒为 NONE）。7 个槽位字段暂时保留，
  但只作为**运行期类型镜像**（hoist 产物，供运行期 vtable 的 free/clone/assign 使用），
  不再是任何编译期判定的依据。
- ✅ **P3d-2：删除 AST 侧的所有权提取入口**。`sema_type_expr_ownership` 删除；
  所有消费者（option/数组/元组/切片元素、extern/全局函数签名、extern var、捕获、
  函数字面量）改为 `type_ownership_of(已解析类型)`。CTFE 的类型表达式求值也补上
  所有权（`AST_PTR` / `AST_SLICE_TYPE` / `AST_OWNED_TYPE` 及数组元素槽位），
  `type X = own *T` 不再依赖 P1a 的“类型定义后补包”。
- **待办（P3e）**：删除 7 个槽位所有权字段（需要把运行期 free/clone/assign
  改为只走编译期生成的序列，或让运行期槽位类型保留所有权信息）、
  AST 上的 ownership 缓存、`type_fatal_variant_of` / `type_fatal_base_of`
  恒等壳，以及运行期 `borrowed` 视图位（最终与借用 materialize 改造一起删除）。
- 反射已把 `ownership` / `unqualified` 纳入 key 表；“所有权是类型修饰符，故可反射”
  已在文档层面成立。
- 验证：Windows 全量 2232/2232、examples 53 全过；WSL2 全量 2229 passed / 2 skipped。

---

## 4. P2 的"运行期边界"清单（剥离点）

按"运行期只看裸类型"的规则，逐个边界检查（每条都是 1-3 行改动，不再是风险）：

| 边界 | 处理 |
|---|---|
| `value_make` / `value_alloc_data` | 断言 + 剥（防御式：调用方本应已剥） |
| `BCODE_LOAD_TYPE` / `DEFINE_TYPE` / 构造 | 剥后取 type_id |
| 字段布局 / `struct_type_seal` | 字段类型存入类型池时剥（布局只关心 sub） |
| FFI 描述符（`ffi_type_for`） | 剥后按裸类型映射 |
| `gen_destroy_impl` / 销毁序列 | **不剥**：编译期按未剥类型图决定 FREE 集合 |
| `x.*` 读取 / `data()` / 切片元素 | 编译期决定是否 materialize（已有 `BCODE_MATERIALIZE`） |
| 比较 / 打印 / 哈希等通用 vtable | 剥后按裸类型分派 |

替换完毕后在 debug build 打开"运行期类型必须无修饰"断言，全量跑一遍即可确认没有
漏网的穿透点。

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
| Q7 | 运行期类型池是否也存 `own T`？ | 存（参与相等/反射/签名），但**运行期指令只引用剥过的类型**，并在 debug 断言"运行期类型无修饰" |
