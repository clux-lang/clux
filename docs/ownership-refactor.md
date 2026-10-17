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

### 0.7 本模型取代的机制

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
