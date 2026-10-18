# clux 单所有权重构：堆块反向挂载模型（v3）

## 0. 所有权模型定稿：**值状态的流动检查**（2026-10，取代按类型判定）

> 本节是与用户逐条确认后的**权威模型**。§1–§4 的实现叙述若与之冲突，以本节为准。

### 0.1 核心思想

**所有权不是指针的"特权"，而是编译期**值**的状态**；编译器检查的是
**所有权流动是否合法**，而不是"类型是不是指针"。

由此：
- 每个编译期值（**包括没有名字的表达式**）都携带一个所有权状态；
- 位置上的标注（`own` / `ref`）是"**该位置要求什么状态的值**"，不是类型修饰；
- `own i32` **语法合法**，但因为 `i32` 没有 FATAL 产生式而**永远无法初始化**
  ——"不可用"是规则的**推论**，不需要专门的类型检查分支。

### 0.2 四种值状态

| 状态 | 含义 |
|---|---|
| **NONE** | 普通值，没有需要转移的堆块 |
| **FATAL** | 将亡值：有待转移的堆块，**必须被接住**（不能落到具名位置上） |
| **OWN** | 已持有堆块 |
| **REF** | 借用视图，不持有 |

### 0.3 产生式 → 产出状态

| 产生式 | 产出 |
|---|---|
| 字面量 / 纯计算 / **字段不含 own 的 `CONSTRUCT`** | **NONE** |
| `new` / `move` / `clone` | **FATAL** |
| **字段含 own 的 `CONSTRUCT`**（从内到外逐层传播） | **FATAL** |
| 调用"返回 `fatal`"的函数 —— **`fatal` 修饰的是"调用表达式"整体的值** | **FATAL** |
| 读 `own` 变量 / `own` 参数 / own 字段 | **OWN** |
| 读 `ref` 变量 / `ref` 参数 | **REF** |
| 调用"返回 `'<a> ref *T`"的函数（**ref 产生式**） | **REF** |

### 0.4 位置 → 可接收的状态（右 → 左流动）

| 位置 | 接收 |
|---|---|
| NONE 位置（普通变量 / 参数 / 返回） | **NONE** |
| `own` 位置（变量 / 参数 / 字段） | **FATAL** |
| `ref` 位置（变量 / 参数） | **REF**、**OWN**（借出） |
| 返回位标 `fatal` | 函数体交出 FATAL；**调用点产 FATAL** |
| 返回位标 `'<a> ref *T` | 函数体交出 REF；**调用点产 REF** |
| 返回位标 `own` | **不允许**（调用点无处接 OWN） |

### 0.5 参数与返回值：同一张表

**参数传递 ≡ `形参 = 实参` 的赋值产生式**；返回 ≡ 函数体交出的值进返回位。
所以**一张表同时管"赋值 / 传参 / 返回"三处**，不为函数边界另立规则。

- **参数位只有 `own` / `ref` 两种接收能力**；**`fatal` 只用于返回值**
  （它描述的是调用表达式的值，不是形参）。
- `own` 形参接收 FATAL 后，**形参的值状态是 OWN**（函数负责释放，或 move 出去、
  或随 `fatal` 返回位离开）。

### 0.6 特殊规则

1. **`move` / `clone` 只接受 OWN，产出 FATAL**（不接受 FATAL/NONE/REF）
2. **有 `ref` 存活**的 own 值 → **禁止 `move`**（整套规则里唯一的特例）
3. **不存在 `OWN → NONE` 的下调** → 函数体内若存在 own，必须 `move`/`clone` 出去，
   或随 `fatal` / `ref` 返回位离开
4. **借出是隐式的**：`var r: ref *T = a`（a 是 own）即借出，无需额外语法；
   运行期就是一次指针赋值
5. **`own i32` 不可达**：`i32` 的产生式是 NONE（栈值 / 无 own 字段的构造），
   而 NONE 只能流向 NONE → 永远无法初始化

### 0.8 销毁序列的来源：**type 对象是唯一权威**（2026-10 决定）

聚合的销毁**完全由 AST 生成**（不建 sema 侧"类型→槽位"缓存，也不走运行期
递归），但**"哪些槽位是 own"必须从 type 对象反推**，不能读定义 AST、更不能读
构造式。

**为什么 AST 不可信**：未来会加入**装饰器**，而装饰器会改造类型（改字段名、
改字段类型、增删字段）。定义 AST 描述的是"装饰器之前"的形态，构造式只覆盖
被显式写入的字段——两者都可能与最终类型不符。type 对象在装饰器之后仍是权威。

**因此所有权必须记在"字段对象"上**——这是**合法的类型描述**：

- ✅ `struct_field_t.ownership` / `tuple_elem_t.ownership` /
  `union_member_t.ownership` / `cunion_member_t.ownership`：字段位置的所有权
- ❌ `type_t.ownership`（"这个类型本体是 own 吗"）：仍然禁止——所有权是**值**
  的状态，不是类型的标签

二者的区别是本模型的核心：**类型描述的是"结构里有几个可挂载所有权的槽位"，
不是"这个值是不是 own"**。前者是类型的一部分（装饰器会改它），后者是值的状态。

> 实现注意：槽位所有权必须**纳入类型去重（intern）键**。否则
> `Holder { r: ref *i32 }` 与外观相同的 own 槽位版本会被 intern 成同一个类型，
> 销毁序列就会错。同理，装饰器改型后必须重新 seal / 重新入池。

**生成形态**（全部是语言层结构，C 后端逐条翻译即可，无需在 C 侧重建分析）：

| 类型 | 生成的销毁 AST |
|---|---|
| `own *T` / `own []T` | `FREE v` |
| struct / tuple | 逐 own 槽位：`FREE v.f1; FREE v.f2;` |
| `[N]T`（T 含 own 槽位） | `for i in 0..N { <T 的销毁序列，作用于 a[i]> }` |
| `own []T` | `for i in 0..len { … }`（运行期长度） |
| `?T` | `if (v != nil) { <T 的销毁序列，作用于 v.! > }`（语言层 nil 判定语法；inner 槽位 own 才释放） |
| tag union | `if (tag == k) { <payload 的销毁序列> }` |

插入点：**作用域尾部 / return / break / continue 处插入的是语句列表**
（`FREE` 是**表达式**，见 `ast_free.h`）。`compile_exit_with_defers` 已按
`compile_stmt` 逐条编译 `defers` 链，故 `for` / `if` 可直接入链，退出协议无需改动。

**已落地**：`FREE` 改为表达式（`0adc72e`）；闭包 own 捕获按 `FIELD_GET` 展开
（`d96940d`）；struct / tag union / **tuple / array / slice / option** 的槽位展开、
`own *聚合`（先解引用再逐槽位）、`new U{…}`、闭包聚合捕获槽、**赋值点旧值销毁**
（2026-10，见 §7.1）。
**未落地**：`cunion` 按设计永不参与自动销毁（无 tag，运行期无法判定活跃成员）。


### 0.9 本模型取代的机制

| 旧机制 | 现状 |
|---|---|
| `type_t.ownership` 字段 | **已删除** |
| `fatal T` 类型变体 / `fatal_bases` 折回 / `type_fatal_bind_base` | **已删除** |
| 类型层谓词 `type_owns_resources` / `sema_type_contains_*` / `type_directly_owns` | **已删除** |
| `fatal` 形参形态 | 应删除（参数位只有 own/ref） |
| "复合类型禁止携带指针/切片" | **临时限制**：为不干扰基本所有权功能验证而先行移除，聚合待后续恢复（其 own 字段由 §0.3 的 CONSTRUCT 规则天然支持） |

---

## 设计原则

在错误的基础上重新设计，不打补丁。当前单所有权实现是一路迭代出来的，所有权
**泄漏到了运行期**，与 §3"运行期零检查"的设计目标相悖。重构的核心决策：

1. **所有权全在编译期**（sema + compiler）——全局堆块表、红黑树、反向挂载全部
   是编译期分析结构，运行期完全解耦
2. **运行期只有显式指令**——sema 在作用域退出点明确插入 FREE 语句销毁 own 块，
   无 SCOPE_EXIT 运行期遍历；type_t 纯布局无 ownership
3. **share/weak 已移除**——RC 引用计数体系先行移除，待后续重新设计
4. **语法冻结不动**——本次只做实现层重构，关键字/标注/move/clone/new 形态不变

---

> ### 实现状态：P1/P2 已完成（2026-10-15 提交），P3 FREE/DEFER 字节码已定稿（§4.2），实施中
>
> **2026-10-16 前置重构**：复合类型（struct/tuple/array/union/option/cunion）字段
> 禁止携带指针/切片类型（sema 层强制）。移除复合类型递归释放机制
> （ptr_free_owned_recursive / ptr_clear_owned_recursive / ptr_clone_block /
> ptr_type_needs_scan 的复合分支）、fatal 复合变体系统（type_set_ownership /
> make_fatal_variant / ex_construct_fatal / BCODE_SET_OWNERSHIP 处理函数 /
> type_owns_resources / type_contains_ref 的递归分支）。指针/切片只能出现在
> 变量声明/参数/返回值等顶层位置。自引用链表/树等结构待后续重新设计。
> 测试 1868→1781（删除 87 个指针字段相关测试）。

---

## 1. 问题：ownership 泄漏到运行期的五面

| # | 泄漏面 | 现状实现 | 重构后 |
|---|--------|---------|--------|
| 1 | `type_t.ownership` 参与去重 intern | fatal 变体双实例 + `fatal_bases` 映射 | type_t 纯布局，无所有权维度 |
| 2 | 字节码携带所有权立即数 | `PUSH_PTR <own>` / `SET_OWNERSHIP` | 无所有权立即数 |
| 3 | 运行期按类型递归扫描 | `ptr_free_owned_recursive` / `ptr_clear_owned_recursive` / `ptr_type_needs_scan` | 销毁序列编译期生成，运行期只有显式 FREE |
| 4 | vtable 按 ownership 分派 | `struct_clone` 的 `ptr_clone_block`、`struct_dispose` 递归 | 纯布局拷贝 + 显式句柄操作 |
| 5 | 函数边界/FFI 运行期查签名 | `type_owns_resources(sig->params[i])` | 调用点编译期已知，直接生成 move/clone |

根因：设计决策 §7.2"所有权扩散"的实现方式——复合类型递归含 own 字段时，靠给整个
复合类型打 ownership 标记（fatal T）来表达。于是所有权必须进类型系统、进指令流、
进运行期扫描。

## 2. 核心模型

**三句话：**

1. **编译期堆块全局登记**——sema 维护一张全局堆块表（红黑树），`block` 记录
   `own`（当前拥有它的 value）、`type`、`scope`（当前挂载作用域）
2. **move = 修改 block->own**（O(1)），跨作用域天然正确；ref 不拥有，sema 维护
   ref 指向堆块直接访问
3. **scope 销毁 = 编译期生成 FREE 序列**——sema 在作用域退出点按 scope_code 从
   红黑树批量取块，生成显式 FREE 指令；运行期零元数据、零遍历

### 2.1 编译期数据结构

```
红黑树（key = (scope_code, value_code)）
  ├─ 全局堆块表：跟踪程序里每个 own 堆块的诞生/转移/死亡
  ├─ block 记录：{ own: 当前拥有它的 value, type, scope: 当前挂载作用域 }
  ├─ value_code / scope_code：单调递增内部编码
  └─ 复合类型扁平槽位表（type → slots，含嵌套展开）
```

### 2.2 运行期形态

```
type_t: 纯布局（size/align/kind/字段表），无 ownership
value:  持句柄（指向 payload 的裸指针），无 block 元数据
scope:  零堆块记录，无 SCOPE_EXIT 遍历
指令流: 只有 NEW / MOVE / FREE / SET_FIELD / SET_INDEX 等显式序列
```

运行期就是一个"裸指针 + 显式 free 序列"的执行器——与手写 C 一样。所有所有权判定
在编译期已算完并编码进指令流。

### 2.3 关键语义

| 操作 | 编译期（sema） | 运行期（指令） |
|------|---------------|---------------|
| `new T{...}` | 红黑树插块，block->own = 目标 value | NEW：分配 + 返回句柄 |
| `move(a)` | 改 block->own（跨 scope 删插换 key） | MOVE：源槽置 NULL+TDZ、目标槽接句柄 |
| `clone(a)` | 深拷贝：新块 + 拷贝 payload，新块 own = 新 value | CLONE：分配 + 拷贝 |
| 构造 `.T{...}` | 临时块 own = 临时 value；DEFINE 时转移 | CONSTRUCT + DEFINE |
| 函数参数/返回 | 块随参数/返回值转移，改 block->own | MOVE |
| `s.p = new T{...}` | 新块 own = s；旧块摘挂 | SET_FIELD + NEW |
| 作用域退出 | 按 scope_code 批量取块，生成 FREE 序列 | FREE 序列 |
| ref | 逃逸检查 = ref 存活 ≤ block->own 的 scope 存活 | 直接经句柄访问 |

### 2.4 ref 语义

- ref 值 = 指向 block 的句柄 + 类型（`ref *T`）
- 访问 `r.*` = 解引用一跳
- ref **不改变 block->own**——观察者
- 逃逸检查：ref 的存活 ≤ block 的 owner value 的 scope 存活
- 作用域标注 `'<a,b>` **保留**，尤其函数边界返回值 ref 必标注，调用点不深入
  函数体逃逸分析

### 2.5 复合类型扁平槽位表

- 复合类型的"能挂载堆块的槽位"在**类型定义时扁平确定**（sema 静态维护
  `type → slots`）
- 扁平展开：`A { b: B }`, `B { p: own *i32 }` → A 的槽位表 =
  `[{offset_of_b.p, own *i32}]`
- `set_field` / `set_index` 触发反向挂载分析
- 销毁零递归：对每个扁平槽位操作

## 3. share/weak 已移除（2026-10-16）

share/weak RC 引用计数体系已全部移除（依赖单所有权 fatal 产物作为创建源，
在单所有权重构期间先行移除）。`rc_block_t` 控制块、`upgrade` 内置函数、
`OWN_SHARE`/`OWN_WEAK` 枚举值、`VTABLE_PTR_SHARE`/`VTABLE_PTR_WEAK` vtable、
fatal→share 创建通道全部删除。share/weak 语义待后续重新设计。

## 4. 实施阶段（P1-P5）

| 阶段 | 内容 | 验收 |
|---|---|---|
| P1 | 编译期堆块表基础设施：红黑树(scope_code, value_code) + block 记录 + value/scope 内部编码 + 复合类型扁平槽位表（sema 侧纯新增，不动运行期） | 现有测试零回归 |
| P2 | sema 所有权分析接入：new/move/clone/构造/参数/返回全路径维护红黑树改 own；set_field/set_index 反向挂载；作用域退出点批量取块标记待 FREE | 所有权矩阵探针逐格比对现状输出 |
| P3 | compiler 生成 NEW/MOVE/FREE 指令 + 作用域退出点 FREE 序列；运行期新增三条指令，删除 ptr_free_owned_recursive/ptr_clear_owned_recursive/ptr_type_needs_scan；DEFER 前移编译期挂载，删除 BCODE_DEFER/scope 级 defer 栈 | 矩阵 119+49 全绿，泄漏零 |
| P4 | 类型系统去 ownership：删 type_t.ownership、fatal 变体、fatal_bases、SET_OWNERSHIP、PUSH_PTR/SLICE 的 ownership 操作数、vtable 按 ownership 分派 | 1868 测试全绿 |
| P5 | 函数边界/FFI 改编译期判定 + share/weak 重新设计 | 全量+泄漏零 |

### 4.1 P1 内部决策（已定）

- **决策 A**：复合类型扁平槽位表挂 `sema_t` 全局缓存（`type → slots`，惰性构建）
- **决策 B**：value_code / scope_code 静态分配（单调递增计数器挂 sema_t）
- **决策 C**：红黑树 key = (scope_code, value_code) 复合，先比 scope_code 再比 value_code

### 4.2 P3 内部决策（已定：FREE + DEFER 字节码）

**决策 D：FREE 采用作用域局部 slot_id（NEW 分配时登记）**

- `NEW <slot_id>` —— 分配堆块，句柄登记进**当前作用域局部表**（slot_id 由编译器
  按作用域分配，块内唯一）；栈压句柄
- `FREE <slot_id>` —— 读表释放 + 置 NULL；NULL 槽 panic（防御编译器 bug，非运行期扫描）
- slot_id 随 move 转移持有槽（红黑树 `block->own` 的逐指令投影，源槽置空）
- **编译到 C**：`NEW` → `T *__blk<N> = alloc(...)`；`FREE` → `free(__blk<N>)`；
  slot_id = 函数内唯一临时变量，零运行期元数据。**方案 B（value_code 全局 id 表）
  已否决**——需运行期 (id→ptr) 动态表，违背 §2 原则 2"运行期零元数据"，且 C 后端无对应物

**决策 E：DEFER 前移编译期挂载，改写为作用域退出点 CALL**

- sema：`sema_scope_t` 新增 `defers` 向量（元素 = `ast_func_def_t*` 借用，AST
  arena 托管）；AST_DEFER walk 时 `sema_check_func_literal` 检查后登记进当前
  scope（**不建子作用域**——保持 stmt_build build_block 索引对齐）；
  `sema_scope_destroy` step 2 释放向量
- compiler：AST_DEFER 定义点**统一走块入口实例化**（与局部函数
  `compile_func_bind_instance` 同构）——`MAKE_FUNCTION <fid>` + 捕获绑定 +
  `DEFINE __defer_N`（编译器私有名，块内唯一）；循环内每轮新实例
- 作用域退出点（正常块尾 / RET 前 / break·continue 跨出层）：**逐层逆序**发
  `PUSH __defer_N; CALL 0; POP`；`CALL 0` 压闭包值后调用（无参 void，净 0），
  value_call 建帧 + closure_scope 查找链可达捕获值（与 m6 scope_run_defers 同款语义）
- **执行顺序硬要求**：每层 = 逆序 CALL 该层 defer → FREE 该层块 → POP_SCOPE，
  从内到外；defer 体仍能读到本作用域尚未释放的 own 块
- **error 无路径**：error 是编译期概念（shadow 失败即穿透，等价 panic，不生成
  运行期代码），运行期无错误展开分支 → 所有 defer 执行点编译期全覆盖
- VM（P3 删）：`BCODE_DEFER` / `scope_t::defers` / `scope_add_defer` /
  `scope_run_defers` 全删；`vm_pop_scope` 回归"只弹栈销毁"，零隐式执行

## 5. 保留的语义（不破坏现有测试/语法）

- `own` / `ref` / `fatal` 关键字语法不变（`share` / `weak` 已移除）
- `'<a,b>` 作用域标注保留——尤其函数边界：返回值 ref 必标注，调用点不深入函数体
  即可逃逸分析
- `move` / `clone` / `new` / `set_field` / `set_index` 语法形态不变
- fatal 保留为编译期概念（sema 状态：将亡值必被接管），不再产生 fatal type_t 变体
- `defer` 语法/捕获语义（clone 值、LIFO、嵌套、defer 体内再登记 defer）全部保留——
  执行模型从"运行期 scope 级 defer 栈"改为"编译期展开的作用域退出点 CALL 序列"

### 5.1 P3 验收用例（defer 改写后须全绿）

现有 m6-design §14.26 的 `tests/defer_test.cpp` 21 例语义**内核不变**，逐例对齐新模型：

| 用例族 | 新模型覆盖点 |
|---|---|
| LIFO（同作用域三 defer 拼接 123→321） | 同层逆序 CALL |
| 空捕获 / clone 捕获 / 捕获后改外层不影响 | 捕获绑定在定义点，CALL 读实例 |
| 嵌套 + return 多层弹出 | RET 前逐层逆序 CALL（从内到外） |
| 循环体每轮 LIFO | 每轮块入口新实例 + 退出点 CALL |
| break / continue 都跑 | balance_scopes_out 跨出层插 CALL |
| **defer 先于 own 释放**（ref 捕获读堆块内容） | 每层 CALL 在 FREE 之前（顺序硬要求） |
| defer 体内调用函数 / 体内再登记 defer | 独立函数帧，body 内层自己的退出点 |
| switch 分支体 / 函数体顶层 defer | 各作用域退出点统一 |

运行期断言补充：`vm_pop_scope` 不再执行任何 defer（零隐式执行）；error 路径
不存在（编译期 error 即穿透，无运行期展开）。

## 6. 与 §13 销毁点编译期生成的关系

§13.3-§13.4 的"销毁点编译期生成"是本文档的特例——本模型把它从"只做销毁"推广到
**所有所有权操作**（new/move/clone/构造/参数/返回/销毁全链路编译期生成）。DEFER
的"作用域退出点 CALL 序列"与 FREE 序列同源：每层作用域的退出协议统一为
**逆序 CALL 该层 defer → FREE 该层块 → POP_SCOPE**，编译期一次性生成，运行期只执行显式指令。

---

## 7. 当前进度与交接（2026-10-18 复查）

### 7.1 已可用（有探针 + 回归测试验证、无泄漏）

| 形态 | 说明 |
|---|---|
| `own *T` / `own []T`（元素为标量） | 销毁：`FREE v` |
| **含 own 字段的 struct**（含嵌套 `A{b:B}`、`B{p:own *i32}`） | 销毁：逐 own 槽位 `FREE v.p`；`ref` 槽位**不进**序列 |
| **含 own 指针的 tag union**（`union U { p: own *i32; n: i32 }`） | 销毁：每个 own member 一条 `if (v is m) { <payload 销毁序列> }`；只释放当前 tag 的 payload，ref member 跳过；tag 宽度跨 128 边界（`PUSH_I16` / `IS_TAG`）验证过 |
| **含 own 元素的 tuple**（`<own *i32, i32>`、`<Box, i32>`） | 销毁：逐元素 `FREE t[i]`（own 元素释放、ref 元素跳过、复合元素递归）；`t[0] = …` 覆盖前也释放旧元素 |
| **含 own 元素的定长数组**（`[2]own *i32`、`[2]Box`） | 销毁：`for i in 0..len(a) { <元素销毁序列> }`（运行期 `LENGTH`、u64 计数器）；`a[0] = …` 覆盖前释放旧元素 |
| **含 own 元素的切片**（`own []own *i32`、`own []Box`、`make(own *i32, …)`） | 销毁：先逐元素循环，再 `FREE s`（顺序不能反）；切片变量重新赋值也先销毁旧值 |
| **含 own inner 的 option**（`?own *i32`、`?Box`、`?own []i32`） | 销毁：`if (v != nil) { <inner 销毁序列，作用于 v.! > }`；none 不解包、不释放。语法即用户写的 `if (a != nil)` |
| **指针指向含 own 槽位的聚合**（`own *Box`、`own *U`） | 销毁序列先解引用再逐槽位：`FREE p.*.f …` 然后 `FREE p`（嵌套指针 `own *own *T` 同理，不再双 FREE / 泄漏） |
| **堆上的 tag union**（`new U{...}` → `own *U`） | `new` 走 tag 哨兵 + `CONSTRUCT 2`；`p.field` 成员访问对 union 指针也自动解引用 |
| **含 own 聚合捕获的闭包** | 捕获槽销毁按**捕获槽的 type 对象**展开（`ast_var_def_t::resolved_type`）：`own *T` → `FREE f.cap`，聚合 → 逐槽位（union 带 `if (f.cap is m)`） |
| `move` / `clone` 聚合 | `move(OWN) → FATAL`；**聚合的 own 值必须走 move**，否则源不标 moved → 双释放 |
| 聚合的 own 形参 / `fatal` 返回 / 循环 / break / continue | 销毁序列在退出点按 type 展开（`collect_free_stmts`） |
| **字段覆盖旧值**（`s.u = .U{…}`） | 运行期先按**槽位所有权**销毁旧值（`ptr_free_slot_value`：own 叶子释放、ref 跳过），再 memcpy |
| **赋值点旧值销毁**（`p = new …` / `a[i] = …` / `p.* = …`） | sema 生成销毁序列挂在 `ast_assign_t::destroy`，compiler 在「右值求值之后、写入之前」编译（A 方案 §2.3"旧块摘挂"）：指针/切片变量、数组/切片/tuple 元素、own 指针的被指向内容都先释放旧值。**右值可能读旧值，顺序不能反**；聚合变量赋值仍走 `compound_assign_transfer`（运行期按槽位释放），不重复生成 |
| **union 变量重新赋值** | `compound_assign_transfer` 的旧值释放改为**按槽位所有权**（`ptr_free_owned_recursive` 重写）——旧 tag 是 ref payload 时不再 free 借用目标（曾 0xC0000005） |

关键实现点：
- 销毁序列**完全由 AST 生成**（`gen_destroy`，`stmt.c`），从 **type 对象**反推
  （§0.8）。槽位所有权记在字段结构体上（`struct_field_t::ownership` /
  `tuple_elem_t::ownership` / `union_member_t::ownership` /
  `array_type_t::elem_ownership` / `slice_type_t::elem_ownership` /
  `option_type_t::inner_ownership`），**参与 intern 去重键**——否则
  `union A { p: own *i32 }` 与 `union B { p: ref *i32 }`、`[2]own *T` 与
  `[2]ref *T` 会被去重成同一个类型，后定义者继承前者的所有权。
- 叶子（ptr/slice）**仅当槽位 ownership == OWN** 才 FREE；复合**无条件递归**。
- tuple / array / slice 的元素访问由 sema 现造 AST_INDEX（`t[i]` / `a[i]`）；
  option inner 用 AST_UNWRAP（`v.!`），判空用新直接形态节点 `AST_OPT_IS_SOME`
  （`!=' / == nil` 的 op 是 `token_t*`，sema 无法合成——与 `is` 同款问题）。
- 逐元素循环用新直接形态语句节点 `AST_FOR_INDEX`（`for i in 0..len(src)`）：
  计数器是 compiler 私有的 **u64** 变量（与 `LENGTH` 返回类型一致），
  名字 `__del_i_N` 由 sema 分配（`sema->destroy_seq`，块内唯一）。
- sema 生成的销毁语句带 `ast_node_t::is_generated`，Pass 3b **不再回走**
  （否则 `AST_IF` 去消费 3a 没建的作用域、`AST_IS_TAG` 不是用户表达式——
  这就是早期"栈溢出 / scope tree desync"的根因）。defer 的 `__defer_N()` 调用
  同样 tok_begin=0，但**必须继续 walk**（CTFE 依赖），故用显式标记而非 tok_begin。
- 运行期 `FREE` 用 `ptr_free_block_only`（**不递归**），被指向值的 own 槽位由
  sema 生成的 `FREE v.f` 逐条释放。运行期仍保留的递归路径（DISPOSE / 复合赋值 /
  字段覆盖）已统一改为**按槽位所有权**（own 叶子释放、ref 跳过），type 对象是唯一权威。
- 运行期类型对象也带槽位所有权：hoist 用新指令 `SET_FIELD_OWNERSHIP <own>` 在
  `DEFINE_FIELD` / `APPEND_ELEM` / `DEFINE_BOUND` / `SET_TYPE` 之后补写
  （struct/union/cunion/tuple/array/slice/option 全覆盖）。否则运行期看到的是
  缺省 NONE——具名 struct 还带 `MARK_UNIQUE`，不会去重到 sema 那份带 ownership
  的类型。`MAKE` 指令也带元素所有权立即数（运行期构造的切片类型必须与 sema 一致）。
- `CONSTRUCT` 的字段含 own 槽位 → 产 `OWN_FATAL`（`construct_result`）；
  构造/`new`/捕获/字段赋值的槽位接收规则统一在 `sema_check_slot_flow`：
  own 槽位只接 FATAL、非 own 复合槽位不得 copy OWN 聚合。
- 变量定义/捕获位还有**反向规则**（§0.4）：FATAL 不得进非 own 位置
  （`var b: Box = .Box{…}` 必须写 `own Box`）；own 聚合不得 copy。

### 7.1.1 NEW / CONSTRUCT 统一 + **扁平化 clone**（2026-10）

**NEW 只管申请内存，CONSTRUCT 负责构造**（C 后端等价的关键，理由同 FREE）：

| 指令 | 职责 |
|---|---|
| `NEW` | 弹字节数 → 申请原始块（清零）→ 压 `opaque` 句柄（C: `calloc(1, size)`） |
| `CONSTRUCT n` | `[dest, type_value, v1..vN]`：dest=`undefined` → 新建值块（栈值，C: 局部变量）；dest=`opaque` → 在 NEW 的块上构造，结果 `fatal *T`（C: 堆块） |

于是源码两种形态归一为同一条指令序列：
`new T{...}` → `PUSH_U64 sizeof(T); NEW; <type>; <fields>; CONSTRUCT n`；
`.T{...}` → `PUSH_UNDEFINED; <type>; <fields>; CONSTRUCT n`。

**clone 扁平化**：`clone(x)` 不再交给运行期按类型递归（`ptr_clone_block`），
由 sema 在**编译期**展开成显式「申请 + 赋值」序列（与销毁序列对称）：

| 类型 | 生成形态 |
|---|---|
| 无 own 槽位（标量 / cunion / 无 own 字段聚合） | 值本身（普通拷贝） |
| 叶子指针（指向无 own 槽位内容）、标量叶子 | `CLONE`（申请新块 + 拷贝字节） |
| struct / tuple / `[N]T`（含 own 槽位） | `.T{ <逐槽位 clone/拷贝> }`（构造 = 申请 + 赋值） |
| `?T`（inner 含 own 槽位） | `(v != nil) ? .?T{ clone(v.!) } : .?T{ nil }`（三元；直接形态节点） |
| union（含 own 槽位） | `(v is m1) ? .U{ .m1 = clone(v.m1) } : …`（三元链，`member_tag` 逐分支） |
| `own *U`（U 需深拷贝） | `new U{ <逐槽位 clone/拷贝> }`（`?U`/union 则为逐分支 `new`） |
| `own []T` | prelude：`var __cl_N: []T = make(T, len(v))`（MAKE 0）+ `for i … { __cl_N[i] = <元素 clone> }`，表达式 = `AST_TAKE(__cl_N)`（MOVE 移出） |

深拷贝的槽位规则与销毁一致（own 叶子才释放/克隆，ref 跳过；复合递归）。
新增直接形态节点（`token_t*` 无法合成）：`AST_CLONE`（叶子 CLONE）、
`AST_TAKE`（移出临时变量）；slice 路径用 `sema->cur_stmt->prelude`（compiler
在语句主体前、同一作用域编译）承载临时变量与循环。
运行期只剩 `CLONE / NEW / CONSTRUCT / MAKE` 这些「申请 + 拷贝」原语——
没有按类型递归的辅助函数，C 后端可逐条等价翻译。

**与 "move 不能作用于字段/元素" 的关系**（TDZ 是**符号级**的，只跟踪变量名：
`move(a.b)` 掏空字段却无处记录失效，同一字段可重复 move、读到悬垂值）：
clone 只**读**槽位（`FIELD_GET` / `INDEX_GET`）并拷贝，不改变源的绑定状态；
唯一的 move-out（`AST_TAKE`）只作用于 sema 生成的**整个临时变量**（`__cl_N`），
从不对字段/元素发 move。因此字段级 `clone(a.b)` 合法（含切片字段——走 prelude
临时变量 + TAKE），字段/元素级 `move` 仍一律拒绝（sema `ex_move` 约束 1）。

### 7.1.2 显式销毁 `_ = move(x)`（2026-10）

`_ = expr` 不再一律走运行期 `DISPOSE`（递归对象图）——sema 按来源分类，把"提前
销毁"编成与作用域退出**同一套扁平槽位展开**（`ast_assign_t::discard_mode`）：

| 写法 | 语义 / 生成 |
|---|---|
| `_ = move(x)`（x 是 own/fatal 变量） | 在**当前位置**生成 x 的销毁序列（ptr/slice → `FREE x`，聚合 → 逐槽位），x 已由 move 标 moved → 作用域退出不再释放 |
| `_ = <将亡值>`（`new` / 构造 / `clone` / 返回 fatal 的调用） | 生成临时变量 `__disc_N: own T = undefined` + `__disc_N = <expr>` + 销毁序列（临时符号标 moved，块退出不再 FREE） |
| `_ = <借用/无资源>`（ref 变量、`x.&`、字段读、标量） | 只求值 + `POP`——**不得 DISPOSE**：借用不拥有堆块，释放会破坏借用源（实测 0xC0000005） |
| `_ = x`（own/fatal 变量**不 move**） | **拒绝**：own 不可 copy；显式销毁必须写 `_ = move(x)` |
| `move(ref)` / `move(x.&)` / `move(p.*)` | **拒绝**（`ex_move` 约束 0）：借用不拥有堆块，move 会把借用当所有权 |

销毁后变量进入 TDZ，重新赋值（`p = new …`）恢复（R2）；重复 `_ = move(p)` 报
"used after move"。等价 C：`free(p); p = NULL;` / 逐槽位 `free(p->f)`。

### 7.2 仍未支持

**cunion 不参与所有权运算**（设计如此，2026-10 定）：无 tag，运行期无法判定哪个
member 活跃，销毁序列不为它生成 FREE（`gen_destroy` 走 default）。因此
`type_has_own_slots(cunion)` 恒 false：含 cunion 的聚合不产 FATAL、不生成销毁序列；
cunion 成员槽位所有权在 sema 侧恒记 `OWN_NONE`。成员**可以是指针/切片**
（类型层限制已放开），但 `new` 放进去的堆块生命周期由开发者自负（与 C union 语义
一致）——后续如何管理暂缓。

**`clone` 与 ref 槽位**：clone 只深拷贝 own 槽位（切片缓冲区总是深拷贝），
ref 槽位保持借用指针——与销毁序列同一套槽位规则。

**clone 回退路径**：闭包（`TYPE_KIND_FUNC` 值）与无语句上下文（comptime 折叠）
仍走运行期 `CLONE`（`ptr_clone_block`）；`ptr_clone_block` 因此保留但只作为回退。

### 7.3 开发环境注意事项（踩过的坑）

| 事项 | 说明 |
|---|---|
| **构建** | `cmake --build build --target clux clux_test`（clang，无 gcc） |
| **跑测试** | 必须先 `$env:TEMP="$PWD\build\tmp\gtesttemp"; $env:TMP=$env:TEMP`——否则 `fopen` 被拒（errno 13）导致 UCRT 断言、套件**提前中止**（看起来"大部分绿"其实没跑完） |
| **脚本改文件必须归一化行尾** | 仓库已统一 **LF**（`.gitattributes` 强制）。用 Python 文本模式写入会得到 CRLF（Windows 默认），造成 `+1071/−1062` 的假 diff。写文件用 `io.open(..., "w", newline="")`，或改完跑一次 `build/tmp/to_lf2.py` 风格的行尾归一化。 |
| **编码检查** | `python tools/check_encoding.py`（UTF-8 无 BOM；`third_party/` 豁免） |
| **读文件** | 用 `read` 工具或 Python `encoding="utf-8"`；**PowerShell `Get-Content`/`Select-String` 会按 GBK 解码 UTF-8 文件而显示乱码**（不是文件问题） |
| **submodule** | `third_party/gtest`、`third_party/libffi` 是 submodule，**不要修改**其内容 |
| 探针 | 放在 `build/tmp/`（git 忽略） |
