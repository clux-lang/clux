# clux M7 泛型：实现计划

> 设计依据：[m7-design.md](m7-design.md)（2026-10-17 定稿，2026-10-22 补充所有权
> 不对称推断与 `?*?` 约束通配）。
> 前置审查：[ctfe-review.md](ctfe-review.md) §3（A–E 准备清单）。
> 本文只记录**实现层**的取舍与阶段划分，不重复设计文档的语义决策；两者冲突时以设计文档为准，
> 并把差异回写到设计文档。

## 0. 现状

- **M7 已实现完成（2026-10-22）**：P0–P9 全部落地——泛型函数、泛型类型、
  泛型方法/静态泛型方法、comptime 值参数与值参数包、类型参数包（函数/别名/
  struct）与 `<...Args>`、`extends` 约束、`interface`/`implements`（含 `&`、
  `...Base[args]`）、跨模块泛型实例化与 reexport。已知边界与审视结论见
  P10。测试基线 2310 全绿。
- 语法侧为泛型预留的落点：`Name[args...]` 在 parser 里解析为 `AST_INDEX`
  （`include/parser/ast_index.h`），语义阶段按上下文区分下标 / 实例化。
- 类型系统：`type_t` 单例 + intern（`include/vm/type.h`），程序类型 id 由
  `type_id_alloc` 全 VM 分配；sema 登记表 `sema->types`（`sema_type_register`）
  驱动 compiler 的类型提升区。
- 函数流水线：`pass1_names` 注册符号并入队 `sema->funcs` → `pass1b/pass2`
  解析类型/签名 → `pass3a` 建作用域树 → `pass3b` 队列驱动 walk。队列
  **已经支持在解析中追加**（局部函数即如此），这是单态化的天然挂点。
- compiler 只认 AST：`compile_prescan_funcs` 收集 `prog->funcs` + 函数字面量，
  hoist 用 `sema->types`，运行期没有"泛型"概念。
- CTFE 已能把类型当值传递/返回（`ctfe-review.md` §2），但 §3-A/B/C/E 未做。

## 1. 实现层关键决策

### 1.1 实例 = 合成的普通声明（compiler 零感知的具体落地方式）

设计文档要求"实例化产物是普通的 `ast_func_def_t` 副本与普通的 `type_t` 实例"。
本文把它落实为：

- **函数实例**：从泛型定义的 token 区间重新解析出 AST 副本（等价克隆），
  `name` 改为 mangling 名，追加到 `prog->funcs` 尾部 + `sema->funcs` 队列，
  然后像普通函数一样解析签名、建作用域树、walk 函数体。
- **类型实例**：重新解析 `struct`/`enum`/`union` 定义（P4），名字改为 mangling
  名，追加到 `prog->funcs`；调用现有的 `sema_eval_*_def` 构造 `type_t`，
  再按普通类型走成员注册。
- **别名实例**：不合成声明，直接把右值在绑定环境下求值成 `type_t*`。

好处：compiler / VM / 字节码工具链**一行都不用改**就能编译实例；实例的方法注册、
hoist、名字绑定、销毁/clone 生成全部复用既有路径。代价是 `prog->funcs` 会增长，
需要保证 sema 的各个 pass 对"后追加的声明"是幂等/可重入的（现有销毁/clone 函数
已是"收尾追加"，走的是同一条路）。

### 1.2 泛型定义本身不进入运行时队列

泛型定义在 `pass1_names` 里只登记符号（`SEMA_SYM_FUNC` / `SEMA_SYM_TYPE` +
`is_generic = true`），**不入** `sema->funcs`、不调 `sema_eval_*_def`、不进
`sema->types`。因此：

- 没有 fid / entry_pc / type_id（与设计 §25/§26 一致：未实例化只是符号）。
- compiler 不会看到它们（`prog->funcs` 里的泛型声明由 `ast_decl_is_generic`
  跳过，见 1.3）。

### 1.3 泛型声明在 `prog->funcs` 中的存留

两种做法：

- **A（已采用，实现简单）**：保留在 `prog->funcs`；五种声明节点内嵌
  `ast_generic_params_t generic`（`count > 0` 即泛型定义），统一谓词
  `ast_decl_is_generic()` 判断。compiler 的 prescan / hoist 名字绑定 /
  struct 成员初始化 / 函数名绑定入口跳过；sema 的 `pass1b` / `pass1d`
  / `pass2` 也不把泛型定义当作可求值实体。
- B：`pass1_names` 把泛型声明从 `prog->funcs` 摘到 `sema->generics`，编译器完全看不到。
  但 formatter / 跨模块导出表 / 诊断都还要看 AST，摘链会带来额外记账。

选 A：改动集中、可 grep，且 `fmt` 不经过 sema。

### 1.4 实例的 AST 副本：**从 token 区间重新解析**（替代 AST 克隆器）

单态化的核心是"给实例一份干净的、类型参数已绑定的 AST"。实际实现**不写**
AST 深拷贝器，而是：

```
parser_t *rp = parser_create(alloc, sema->arena, sema->tokens);
rp->pos = def->tok_begin;
ast_node_t *clone = parse_func_def(rp);      /* 全新节点，无 sema 折叠状态 */
desugar_func(&ds, clone);                    /* 前脱糖（foreach 等）单独重跑 */
```

- **等价于深拷贝**：token pool 在 sema 期间一直存活；重新解析产生全新节点，
  天然不含上一轮的 `AST_TYPE_REF`/字面量折叠/符号状态。
- **类型绑定不靠替换，靠作用域**：实例化时压入一个临时 VM scope，把每个类型
  实参以 **type value** 定义成参数名（`T` → `i32`）。`resolve_type_expr` 的
  `AST_IDENT` 与 `ex_ident` 都经 `type_lookup`/`scope_lookup` 命中它——这是
  clux"类型即表达式"的既有机制。签名/变量类型槽位在正常解析中被折叠成
  `AST_TYPE_REF`，函数体在同一作用域下 walk 时同样折叠，于是 compiler 看到
  的实例 AST 已是具体类型，**不需要**语义层替换器。
- **函数体 walk 延后到 Pass 3b 队列**：实例化只做签名 + 作用域树并入队；
  `sema_generic_walk_instance` 在 walk 前重新压入绑定层。这样递归实例化
  （每次类型参数都变）只增加队列长度，不加深 C 调用栈——128 层深度上限
  由 `inst->depth` 的逻辑链检查，不会栈溢出。
- **代价**：重新解析比 memcpy 慢，但只在实例化时发生一次（缓存去重）；
  且实例 AST 保留原 token 位置，诊断仍指向定义点。

前脱糖必须重跑：parser 到 sema 之间还会展开 `foreach`。`desugar_func`
（新增的单函数入口）复用与 `desugar_program` 相同的 `ds_walk_func`。

装饰器暂不支持泛型定义（`[[...]] func f[T]` 报"未实现"），因为装饰器展开会
把一条声明改写为多条（源函数 + 包装），实例化路径尚未处理这种一对多。

### 1.5 实例化入口与时机

- **函数调用点**（`src/sema/expr_call.c` 的 `ex_call`）：
  callee 是 `AST_INDEX` 且 `object` 解析为泛型函数符号 → 求值实参类型 →
  确定类型实参（P2 只支持显式）→ `sema_instantiate_func` → 把 callee 就地改写成
  `AST_IDENT(mangling)`，随后**走原有调用路径**（签名校验、shadow 返回类型）。
- **取地址点**：`var f = map[i32, f64];` 由 `ex_index`（`expr_access.c`）识别为
  泛型实例化，返回函数值 shadow（与函数引用同型）。
- **类型槽位**：`resolve_type_expr` / `sema_resolve_type_slot` 遇 `AST_INDEX`
  且 base 是泛型类型 → `sema_instantiate_type`，返回 `type_t*`，槽位折叠为
  `AST_TYPE_REF`。
- **方法调用**：`Vec[i32]::new()` 在 `ex_call` 的 `AST_SCOPE_REF` 分支之前先解析
  `type_expr` 的泛型实例化；`v.push(...)` 的接收者类型已是实例类型，走现有
  `MEMBER_CALL`。
- **签名即时性**：调用点需要返回类型，因此函数实例的**签名同步解析**；
  函数体只入队，由 `sema_analyze` 的 3b 队列循环 walk（实例化过程中还能继续追加）。
- **作用域树**：入队前调用 `stmt_build_func.c` 的单函数建树接口（现有
  `sema_build_scope_tree` 内部逻辑抽出的 helper），保证 3b 有树可走。

### 1.6 缓存、mangling、递归保护

- 缓存挂在 sema：`sema->generics.instances`（单链表，线性扫描；实例数量通常
  很小，mangling 比较是 memcmp）。元素 `sema_generic_instance_t`：
  ```
  strslice_t name;                // mangling（arena）
  const ast_node_t *def;          // 原始泛型定义（借用）
  const type_t **type_args;       // arena 数组（类型实参）
  size_t type_arg_count;
  ast_node_t *instance;           // 合成声明（prog->funcs 中）
  sema_func_t *func;              // sema 函数记录
  const type_t *sig;              // 实例签名（类型池单例）
  uint32_t fid;
  bool in_progress;               // 函数体 walk 在途（环/递归检测）
  uint32_t depth;                 // 逻辑实例化深度（不是 C 栈深度）
  struct sema_generic_instance *next;
  ```
- mangling：`<原名>$<arg>...`；类型实参用规范名（`i32` / `*i32` / `[3]i32` /
  `Vec$i32`），值实参用十进制 / `true` / `false` / 转义字符串。
  `sema_generic_mangle_type()` 对每种 `type_kind_t` 输出稳定规范名（含
  `own`/`ref`/`fatal`、`share`/`weak` rc、数组长度、元组、函数签名）；
  **`type->name` 不足以做 key**（匿名类型名可能为空），必须按结构生成。
- 递归：深度上限 128（与 CTFE `max_depth` 一致）；`depth` 记录**逻辑链深度**
  ——实例化只做签名 + 建树并入队，函数体由 Pass 3b 队列迭代 walk，因此
  类型参数每次变化的递归实例化不会加深 C 栈，超限报诊断而不是栈溢出；
  已完成的 mangling 命中缓存 → 直接复用签名，不重复展开（设计 §7）。

### 1.7 诊断

- **当前实现**：函数体内的类型错误沿用定义点 token（实例 AST 保留原 token
  区间）；推导失败 / 实参数量不符 / 深度超限等发生在调用点的错误指向调用点。
- **待补（设计 §18）**：实例化链（"instantiated from: f[str]"）。`sema_generic_instance_t`
  已记录 `def`/`type_args`/`depth`，后续在每个实例记录调用点 loc，诊断收尾时
  作为 `note:` 逐层展开即可；CTFE review §3-E 的可定位性要求同此。

### 1.8 当前实现落点（2026-10-22）

| 文件 | 职责 |
|---|---|
| `include/parser/ast_generic_param.h` | `AST_GENERIC_PARAM` + `ast_generic_params_t`（五种声明共用） |
| `src/parser/parse_generic.c` | `parse_generic_params`：类型参数 / 默认值 / comptime 值参数 / 参数包 |
| `include/parser/ast_decl.h`、`src/parser/ast_decl.c` | `ast_decl_is_generic`（compiler 跳过泛型定义） |
| `include/sema/sema_generic.h`、`src/sema/sema_generic.c` | mangling、实例缓存、函数实例化、调用点推导、Pass 3b walk 绑定 |
| `src/sema/expr_call.c` | 泛型调用入口（显式 `f[...]` / 推导 `f(...)`）、实例 callee 改写 |
| `src/sema/expr_access.c` | 泛型函数取实例 `f[i32]`（函数值） |
| `src/sema/sema_pass.c` | 泛型声明登记为 `is_generic` 符号（不入 queue）；方法/局部函数拒绝 |
| `src/sema/sema_type.c` | 泛型类型名/实例化在 P4 前的明确诊断 |
| `src/compiler/compile.c`、`compile_func.c` | 跳过泛型定义（实例是普通声明，零感知） |
| `src/desugar/desugar.c` | `desugar_func`：重新解析出的实例 AST 单独做前脱糖 |
| `tests/generic_test.cpp` | 19 个用例：解析形状 / fmt 幂等 / 显式/推导/函数值/递归/所有权/负例 |

**当前明确边界**（都有诊断，不静默）：

- 泛型 interface 嵌入实参、跨模块实例诊断位置映射：P10；
- 泛型 interface 嵌入实参 / 默认方法 / 跨模块 interface：P6c/P7；
- 局部泛型函数 / 泛型函数字面量：报"未实现"（实例登记在全局 mangling
  命名空间，局部作用域实例化需要独立设计）；
- 泛型定义上的装饰器：报"未实现"（装饰器会一对多改写声明）；
- 泛型 enum/union/别名的递归实例化：报"未实现"（struct 支持经指针自引用）。

## 2. 阶段划分

每个阶段都以"可构建 + 全量测试全绿 + 新增回归"为完成标准；阶段之间不破坏 trunk。

### P0 前端语法（AST + parser + fmt）✅ 已完成（2026-10-22）

**改动**
- 新增 `include/parser/ast_generic_param.h`（kind `AST_GENERIC_PARAM`）：
  `name` / `is_comptime` / `is_pack` / `constraint`（`extends` 右侧 PATTERN，可 NULL）/
  `default_expr`（可 NULL）。
- 五种声明节点增加 `generic_params` / `generic_params_last` / `generic_param_count`
  （func / struct / enum / union / type def）；`is_generic` 布尔。
- 新增 `src/parser/parse_generic.c`：`parse_generic_params`，语法：
  ```
  '[' param (',' param)* ']'
  param := ['...'] IDENT [':' 'comptime' type_expr] ['extends' pattern] ['=' expr]
  ```
  `...` 只允许类型参数包；参数包必须最后；每个声明最多一个包。
- 在 `parse_func_like_ex`（名字之后）、`parse_struct_def` / `parse_enum_def` /
  `parse_union_def` / `parse_type_def`（名字之后）接入。
- `fmt`：五种声明渲染 `[T, N: comptime u64, ...Args]`；不丢约束/默认值
  （fmt 的漏分支曾造成 import/union 级数据损坏，必须配回归）。
- sema 兜底：泛型声明注册 `SEMA_SYM_GENERIC`；任何使用点（调用/类型槽位/取地址）
  报 `generics are not implemented yet`（P2/P4 会替换为真实实例化）。
  **绝不静默把泛型当普通声明处理**。

**验收**：`tests/parse_generic_test.cpp`（AST 形状：五种声明、comptime 参数、
包、约束、默认值、非法位置）；`tests/fmt_test.cpp` 增补幂等用例；
全量测试不退化。

### P1 单态化地基（无用户可见特性）✅ 已完成（2026-10-22）

**改动（实际）**
- `include/parser/ast_generic_param.h`：`AST_GENERIC_PARAM` 节点 + 参数表；
  五种声明节点内嵌 `ast_generic_params_t generic`。
- `src/parser/parse_generic.c`：`parse_generic_params`（类型参数 / `T = default` /
  `N: comptime u64` / `...Args`；`extends` 明确报未实现，P6 接入）。
- `include/parser/ast_decl.h` + `src/parser/ast_decl.c`：`ast_decl_is_generic`
  （compiler 的预扫描/名字绑定/成员初始化统一跳过泛型定义）。
- `include/sema/sema_generic.h` + `src/sema/sema_generic.c`：
  - mangling（`sema_generic_mangle_type`：类型构造递归编码，含所有权/rc/
    数组/切片/元组/函数签名）；
  - 实例表 + 缓存（线性扫描 `sema->generics.instances`，按 mangling 去重）；
  - 实例化深度（`inst->depth`，上限 128）与在途标记；
  - 函数实例化：重新解析 → 单函数前脱糖 → 签名 → 作用域树 → 入队；
  - `sema_generic_walk_instance`：Pass 3b 队列在 walk 前重绑类型参数。
- `sema_t` 增 `generics`（实例/深度）与 `prog`（实例 AST 追加到 `prog->funcs`）；
  `sema_func_t` 增 `is_walked` / `generic_inst`。
- `resolve_type_expr` 的 `AST_INDEX` 分支与 `AST_IDENT` 兜底：泛型类型名/实例化
  在 P4 前给出明确诊断。
- `ex_ident` / `ex_call`：未实例化泛型定义不是值，给"requires type arguments"
  的可操作诊断。

**验收**：`tests/generic_test.cpp` 的解析/格式化用例 + 全量测试不退化。

### P2 泛型函数（显式类型实参）✅ 已完成（2026-10-22）

**改动（实际）**
- `ex_call`：识别泛型调用（`AST_INDEX` callee 或裸 `AST_IDENT`）→ 解析类型实参 →
  `sema_generic_instantiate_func` → callee 改写为 `AST_FUNC_REF`（实例 fid）→
  `generic_call_finish` 走统一 `value_call` 校验（实参只求值一次）。
- `ex_index`：`f[i32]`（取实例/函数值）同样实例化并改写为 `AST_FUNC_REF`。
- `sema_analyze`：实例只做签名 + 作用域树并入队；3b 队列调用
  `sema_generic_walk_instance`（重绑类型参数后 walk）。逻辑深度 `inst->depth`
  与缓存去重避免递归实例化栈溢出。
- `prog->funcs` 追加合成函数；compiler 用 `ast_decl_is_generic` 跳过泛型定义。
- 错误诊断：未知类型实参、实参数量不符、递归深度超限、实例内类型错误
  （定义点位置；实例化链待补，见 §1.7）。

**验收**（`tests/generic_test.cpp` 的 `GenericRun`/`GenericReject`）：
```clux
func id[T](x: T): T { return x; }
func first[T, U](a: T, b: U): T { return a; }
func rec[T](): void { rec[own *T](); }   // 深度超限负例
```
显式实例化、嵌套调用、递归同型实例（缓存命中）、深度超限、
`var f = id[i32]` 函数值。

### P3 调用点推导 + 默认值 ✅ 已完成（2026-10-22）

**改动（实际）**
- `sema_generic_instantiate_inferred` + `infer_pattern`：把形参类型表达式当
  **类型模式**与实参 `type_t*` 逐节点匹配（`T` 是洞）；支持指针/切片/数组/
  元组/函数签名/optional/const 等构造的递归。
- 所有权不对称（用户 2026-10-22 补充，设计 §4.1）：`fatal→own`、`own/fatal→ref`；
  参数位只能 own/ref。调用点传 fatal 实参时，推导出的是 **own T**；
  传 own（未 move）按所有权规则拒绝。推导出的 T 不含形参侧所有权修饰。
- 支持部分显式 + 部分推导：`f[i32](x, y)`（显式实参占左侧）。
- 默认值：实例化前在绑定层里求值（可引用更早参数），未推导出的参数用它补齐；
  从右向左省略规则由"必须连续"自然满足。
- 方法调用 / `Type::method` 的推导：P4（方法实例）落地。

**验收**：`tests/generic_test.cpp` 的 `CallSiteInference` / `DefaultTypeArgument` /
`OwnershipInferenceIsAsymmetric` / `InconsistentInference` / `CannotDeduce`。

### P4 泛型类型（struct / enum / union）+ 方法 + 别名 ✅ 已完成（2026-10-22，方法自身类型参数除外）

**改动（实际）**
- `resolve_type_expr` 的 `AST_INDEX`：解析泛型类型符号 + 显式类型实参 →
  `sema_generic_instantiate_type`。类型上下文**不推导**（设计 §4）。
- `sema_generic_instantiate_type`：重新解析声明 → 清空 `generic`/改名 mangling
  → 绑定层 → 按 kind 走既有流水线 → 合成声明追加 `prog->funcs`：
  - **struct**：先物化开放类型对象（`type_struct_alloc_open` +
    `type_struct_mark_unique` + 登记/命名/绑 type value），再复用
    `sema_struct_compose_def` 解析字段并 seal。字段里的递归 `Node[T]` 在
    物化后命中缓存拿到开放对象——**递归 struct 可用**（按值内联未 seal
    的字段仍按既有规则拒绝）。
  - **enum / union**：复用 `sema_eval_enum_def` / `sema_eval_union_def`。
  - **type 别名**：重解析右值后 `resolve_type_expr` 得到具体类型，不产生声明。
- **成员方法**：`instantiate_struct_members`（pass1d 的实例版）为实例建
  `member_scope`，注册方法符号 + fid，在绑定层下立即解析签名、建作用域树、
  入 `sema->funcs`；方法函数记录的 `generic_inst` 指向类型实例，Pass 3b
  walk 前重绑 T。静态属性同样注册 + 求值。
- **静态访问**：`Vec[i32]::method/attr` 的 `type_expr` 是 `AST_INDEX`；
  `sema_generic_rewrite_type_expr` 实例化后改写为实例运行时名（mangling）的
  `AST_IDENT`，静态方法/静态属性/enum variant 复用既有按名解析路径。
  只对 struct 改写——enum 的 variant 分支需要保留 `AST_INDEX` 由
  `sema_resolve_type_slot` 直接实例化。
- **调用点推导**：形参模式新增 `AST_INDEX`（`Box[T]`）：按 (def,type) 从实例表
  反查类型实参并逐位匹配；`ref *Box[T]` 等包装自然递归。
- parser：`.T{...}` / `new T{...}` 的类型位支持 `Vec[i32]` 后缀；
  `Vec[i32]::member` 的 `::` 左值放行 `AST_INDEX`（sema 判定是否泛型类型）。
- CTFE：`AST_INDEX` 遇到泛型类型符号时调用 `resolve_type_expr` 并返回 type
  value——`type IntBox = Box[i32];` 等类型别名/类型计算可用。

**验收**（`tests/generic_test.cpp` 的 `GenericType.*`）：
`Box[T]` 字段+方法+静态方法、`Res[T,E]`/`Tag[T]`、`Pair[T,U]`/`Triple[T]`
别名、嵌套 `Box[Box[i32]]`、递归 `Node[T]`、静态属性、`own` 字段销毁、
`wrap[T](v:T): Box[T]` + `unwrap[T](b:Box[T])` 的推导。

**边界**：方法自身的类型参数见 P4b（已实现）；泛型 enum/union 的递归实例化
仍按既有规则拒绝（struct 经指针自引用已支持）。

### P4b 泛型方法（方法自身带类型参数）✅ 已完成（2026-10-22）

**改动（实际）**
- 方法符号：pass1d / 泛型 struct 实例的成员注册遇到 `fn->generic.count > 0`
  时只登记 `is_generic` 符号（不入 Pass 2 队列、不解析签名）。
- 调用点 `recv.method(args)`：
  - 接收者只求值一次；self 形参为指针而接收者是值时合成 `. &`（语义同
    MEMBER_CALL 的接收者转换）；
  - `sema_generic_instantiate_method_inferred`：以 `Owner$method$args` 为
    mangling 前缀，绑定外层 Owner 参数（实例 `parent` 链）+ 方法自身参数，
    从 `[接收者, 实参...]` 推导方法类型参数；
  - 调用改写成**直接 CALL**：callee = 方法实例 `AST_FUNC_REF`，
    实参 = [接收者, 原实参...]；随后 `generic_call_finish` 统一
    `value_call` 校验（方法实例签名含 self）。
- 方法实例按普通函数追加 `sema->funcs` + `prog->funcs`（mangling 名），
  compiler 零感知；deferred walk 时 `parent` 链重绑外层参数。

**验收**（`tests/generic_test.cpp` 的 `GenericMethod.*`）：
`Box[T].map_into[U]` + 函数引用、非泛型 struct 的泛型方法 + 函数字面量、
推导失败负例。

### P4c 静态泛型方法 ✅ 已完成（2026-10-22）

`Type::method(args)` 命中 `is_generic` 方法时：无接收者实参，直接以原实参
实例化方法（`Owner$method$args`），调用改写为 CALL 实例 fid。
`tests/generic_test.cpp::GenericMethod.StaticGenericMethod`。

### P5a comptime 值参数 ✅ 已完成（2026-10-22）

**改动**
- `N: comptime u64` 实参经 CTFE 求值 → `sema_generic_arg_t::value`；
  实例化时在绑定层把它定义成**真实 value**（不是 shadow），于是：
  - 类型槽位 `[N]T` 的边界经 CTFE 命中 VM scope 里的 N；
  - 函数体表达式位置的 `N` 在 `ex_ident` 里折叠成字面量（`if N > 0`、`N * 2`）。
- 显式值实参（`size_of_n[i32, 3]`）走 `sema_generic_parse_explicit_args`：
  按参数表逐位区分类型/值，值实参 CTFE 求值 + 编码 + 转换到声明标量类型
  （整型宽度截断，与 C 语义一致）。
- 推导：形参模式 `[N]T` 与实参 `[4]i32` 匹配时把数组长度绑定为 N；
  默认值按参数种类分别求值（类型 → resolve_type_expr；值 → CTFE）。
- 泛型类型同样支持值参数：`Matrix[T, N: comptime u64] { data: [N]T; }`。
- mangling 含值（`size_of_n$i32$3`），不同 N 是不同实例。

**验收**（`tests/generic_test.cpp` 的 `GenericValueParam.*`）：
显式值实参、`[N]T` 长度推导、默认值/部分显式、函数体内的 `N` 折叠、
`Matrix[i32, 3]`、非编译期常量/类型当值用的负例。

### P5b 类型参数包 ✅ 已完成（2026-10-22；`<...Args>` 类型展开、值参数包待后续）

**改动（实际）**
- parser：新 AST `AST_PACK_TYPE`（`args: ...Args`），fmt 渲染 `...Name`；
  泛型参数表已有 `...Args`（`is_pack`）。
- `sema_generic_instantiate_func`：
  - 参数表拆成 fixed + pack；实参数组 = fixed 实参 + pack 元素（按序）；
  - 实例记录 `has_pack/pack_name/pack_offset/pack_count`；
  - `materialize_pack_params` 在重新解析出的实例 AST 上：
    - 删除 `args: ...Args` 参数，按 pack 元素生成独立参数
      `__pack_arg_i: Ti`（`AST_TYPE_REF`）；
    - 在 body 入口插入 `var args: <T0,...,TN-1> = .{ __pack_arg_0, ... };`
      （空包为 `<>` / `.{}`；`agg` 不能标 `is_generated`，否则 3b 跳过注册）。
- 调用点：
  - 显式 `f[i32, str](...)`：pack 元素就是 fixed 之后的所有显式实参；
  - 推导：fixed 参数按形参类型匹配，**多余实参的类型按顺序成为 pack 元素**；
  - 显式 pack 元素数与实际变参个数不一致 → 明确诊断；
  - 空包：`f()` / `f(tag)` 均可。
- `len(args)` 扩展到 tuple（sema 检查 + VM `builtin_len`），pack 体可遍历元组
  （`args[i]` 已有 tuple 下标支持）。
- 泛型类型中的参数包明确拒绝（P5b 仅函数参数包）。

**验收**（`tests/generic_test.cpp` 的 `GenericPack.*`）：
显式/推导包、fixed 泛型参数 + 包、空包、`len(args)`、包类型不匹配/泛型类型包负例。

**仍待**：comptime 值参数包 `...N: comptime u64`、`extends <?>` 包约束（P6）。

### P5c `<...Args>` 类型展开 ✅ 已完成（2026-10-22）

- parser：元组类型元素支持 `...Args`（复用 AST_SPREAD 节点）。
- `resolve_type_expr` 的 `AST_TUPLE`：把 `...Args` 展开为当前泛型实例的
  pack 元素类型（`inst->args[pack_offset..]`），支持混合
  `<i32, ...Args>`；空包展开为 `<>`。
- 实例化在解析签名/建作用域树期间临时把 `active_inst` 指向实例，
  `sema_generic_walk_instance` 期间同样设置——函数返回类型与 body 内
  `var t: <...Args>` 都可用。
- fmt 渲染 `<...Args>` 幂等。

### P6a `extends` 结构约束 ✅ 已完成（2026-10-22）

**改动（实际，P6a ✅）**
- 新 `parser/ast_pattern.h` + `src/parser/parse_pattern.c`：专用 PATTERN 语法
  `?` / `<?>` / `?*?` / 裸 `*?` / `own|ref|fatal|share|weak *?` /
  `[]?` / `[N]?` / `<...>` / `NAME[args]` / `func(...)->...` / `const|volatile` /
  `&` / `|` / 括号；fmt 新增 `fmt_pattern.c`（逻辑节点统一加括号、幂等）。
- `sema/sema_pattern.c`：把 PATTERN 与具体 `type_t` 递归对位：
  - 所有权/RC 严格身份；`?*?` 只匹配 own/ref/fatal（不含 RC），RC 必须
    `share *?` / `weak *?`（用户 2026-10-22 补充）；
  - `?` 匹配任意类型（含 void）；`<?>` 任意元数元组；
  - 数组长度 `[N]` 或 `[?]`；元组/函数签名逐位；`const`/`volatile` 限定；
  - struct 名称按**字段表相等**（`struct_type_compatible`，§29.2）；enum/union
    与标量按类型身份（§29.6）；
  - `NAME[args]` 泛型实例：按 (泛型定义, 实例类型) 反查实例实参并逐位匹配
    （值实参只支持字面量/`?`）。
- 约束检查在实例化入口（函数/类型）执行，失败不进入函数体；诊断指向使用点。
- `extends` 不允许出现在 comptime 值参数上（parser 拒绝）。

**仍待（P6c）**：泛型 interface 嵌入的显式实参、interface 默认方法、
跨模块 interface。

### P6b `interface` ✅ 已完成（2026-10-22）

**改动（实际）**
- lexer 新关键字 `interface` / `implements`；`ast_interface_def_t`（泛型参数 +
  成员 + `...Base;` 嵌入）；struct def 增加 `implements` 类型表达式链。
- parser：`interface I[T] { var x: T; func f(self: ref *T, ...); ...Base; }`；
  struct 的 `implements I[W], J[W]` 子句（逗号分隔）。fmt 渲染 interface 体与
  implements 子句，幂等。
- sema：
  - `SEMA_SYM_INTERFACE` 符号（pass1 注册；interface 不是类型，所有值类型
    位置报错）；
  - `sema_check_implements_pass` 在 pass2 之后校验**非泛型 struct**；
    泛型 struct 在实例成员签名解析完成后逐实例校验；
  - 校验项：接口 var 成员 → struct 字段名 + 类型相等；接口 func 成员 →
    member_scope 方法 + 函数签名 `type_equal`（self 替换后严格匹配）；
  - `...Base;` 成员复制递归校验（非泛型 Base；环/深度上限 32）；
  - 通过后记入 `sema->impls`（type + interface def + 类型实参）；
  - `T extends I[T]`：PATTERN 的 NAME 指向 interface 符号时查 impl 表，
    逐位匹配实现记录的类型实参；约束环境把被约束参数名映射到当前类型实参
    （支持 `IWriter[T]`、多参数 `IBox[T, Box[T]]`）。
- interface 是纯编译期实体：无 type_t / vtable / 运行时表示；compiler 不处理。

**验收**（`tests/generic_test.cpp` 的 `Interface.*` / `InterfaceReject.*`）：
CRTP `IWriter[Writer]`、泛型 struct 实现、多参数 interface、成员复制、
fmt 幂等；缺属性/缺方法/签名不匹配/把 interface 当类型/未实现即约束 的负例。

**边界**：泛型 interface 嵌入未支持显式实参（只支持非泛型 `...Base;`）；
interface 默认方法、跨模块 interface 留待 P6c/P7。

### P7 跨模块泛型 ✅ 已完成（2026-10-22，直连 import）

**改动（实际）**
- `module_t` 保留 `src_lexer` / `src_tokens` / `src_ast`（源码缓冲区 + token
  pool + 顶层 AST；owned/借用），使导入方能在定义点 token 区间重新解析；
  `module_destroy` 统一释放。
- `sema_symbol_t` 增加 `generic_tokens`（定义模块 token pool）与
  `generic_prefix`（跨模块 mangling 前缀 `mod$Name`）；sema_generic 的
  函数/类型/方法实例重解析与 desugar 都按该 pool 进行。
- `sema_cross_generic_symbol`：扫描被导入模块导出 AST，在当前 sema 注册
  qualified 符号 `mod::Name`（并提供简单名别名供方法签名自引用）。
- 接入三条路径：
  - 类型槽位 `mod::Box[i32]`（resolve_type_expr 的 AST_SCOPE_REF + INDEX）；
  - 泛型函数调用 `mod::id[i32](...)` / `mod::id(...)`（显式/推导）；
  - 泛型函数值 `mod::id[i32]`（ex_index）。
- 泛型 struct 实例的方法、泛型方法、静态泛型方法随实例 AST（定义模块 token
  pool）在导入方 sema 生成；调用点直接 CALL 实例 fid。
- 同名泛型跨模块隔离：mangling 前缀 + 类型实例名字带前缀。

**验收**（`tests/driver_import_test.cpp` 的 `DriverImport.CrossModule*`）：
跨模块泛型 struct/函数/别名、泛型方法、两个模块同名 `Box[T]` 隔离、
`export * from` 泛型 reexport。

**边界**：`.cxs/.cxb` 是**已单态化字节码**容器——含泛型实例的程序可
`bc emit → run`（见验证）；但容器不承载泛型**定义**的 AST/源码，因此
“源码程序 import 一个预编译模块再实例化其泛型定义”不在 M5 源码 import
模型内（当前 import 只解析 `.cx` 源码）。跨模块实例的诊断位置已按定义模块
token pool 重绑。

### P8 收尾 ✅ 已完成（2026-10-22）

- **类型别名参数包**：`type Tuple[...Args] = <...Args>;`、
  `type First[T, ...Args] = T;`、空包 `Tuple[]`（parser 允许空 `[]` 下标）。
- **struct 参数包**：`struct TupleStruct[...Args] { data: <...Args>; }`，
  实例字段按包元素展开；enum/union 仍拒绝（tag 形状需定长）。
- **`implements` 组合**：按设计 §31.5 用 `&`（`IW[S] & IR[S]`），fmt 同步；
  约束 `T extends IW[T] & IR[T]` 走 PATTERN `&`。
- **泛型 reexport**：`sema_cross.c` 递归扫描 `export * from` 代理链，
  在最终定义模块取 AST/token pool。

**仍待（P10）**：跨模块 interface 的 `implements`/约束写法；泛型方法的
显式类型实参（`v.method[U](...)`）；签名期错误的实例链 note；comptime
值参数包的“裸 N 作为 tuple 值”语义（当前明确拒绝并引导 `N[i]` / `len(N)`）。

### P9 comptime 值参数包 ✅ 已完成（2026-10-22）

- parser/sema 接受 `...N: comptime <int|bool>`；显式实参逐个 CTFE + 编码 +
  转换到声明类型（`sema_generic_parse_explicit_args` 的包分支）。
- 实例 `pack_is_value` 标记；实例化校验“值包只收编译期值”；不能从运行期
  实参推导，调用点必须显式给出（空包 `f[]` 合法）。
- 消费语义：`N` 绑定为编译期 `[M]<elem>` 序列；
  - `N[i]` 在 sema 折叠为整数字面量（越界/非编译期下标报错）；
  - `len(N)` 折叠为元素个数；
  - 类型位置 `[N[0]]T` 经 CTFE 读取绑定数组；
  - 裸 `N` 不作标量。
- mangling 仍按值实参编码；fmt 渲染 `...N: comptime u64` 幂等。

**验收**（`tests/generic_test.cpp` 的 `GenericValuePack.*` /
`GenericReject.ValuePack*`）：索引/len、空包、固定+值包、数组长度、
显式实参要求、越界负例、fmt 幂等。

**仍待（P10）**：跨模块 interface 的 `implements`/约束写法；泛型方法的
显式类型实参（`v.method[U](...)`）；签名期错误的实例链 note；comptime
值参数包的“裸 N 作为 tuple 值”语义（当前明确拒绝并引导 `N[i]` / `len(N)`）。

### P10 边界审视（2026-10-22）

**已修正（设计明确要求 / 诊断质量）**
- §31.6 `...Base[args];`：parser 之前只接受裸 `...Name`，sema 按无实参
  interface 解析 → 泛型 Base 失败。现已支持 `...IWriter[T];` 与
  `...IReader[T];`（embed 存 `type_expr`，sema 在当前绑定层解析实参后递归
  检查），fmt 同步。
- **成员复制不是继承**：此前递归检查 Base 时会顺带把 Base 记入 `impls`，
  导致 `implements IRW[W]` 错误地满足 `T extends IW[T]`。现在内嵌 Base 以
  `record=false` 检查，只记录 struct 显式 `implements` 的 interface。
- §18 跨模块诊断位置：token 的 filename 原指向 driver 的临时 canonical；
  现在模块保留 token pool 后把所有 token filename 重绑到 module 自有
  canonical，实例 signature/body walk 期间 `sema->tokens` 切换到定义模块
  pool。跨模块泛型体错误现指向定义文件真实行号。
- §18 实例化链 note：实例记录 `site` + `caller`，walk 出错后按调用链输出
  `note: instantiated from '...'`（含多层嵌套；本地/跨模块均验证）。

**重新分类**
- 预编译 `.cxs/.cxb`：不是泛型缺陷。容器只承载已单态化的字节码；
  `bc emit → run .cxb` 的泛型程序已验证可运行（含跨模块实例）。仅“源码程序
  从预编译模块的泛型**定义**再实例化”不可行，而 M5 import 只解析源码，
  该场景不在当前模型内。

**新发现（2026-10-22，待修）**
- **跨模块泛型实例的模块归属错误**：当前 P7 把实例 AST 追加到**导入方**
  `prog->funcs`，生成函数在导入方字节码注册，`root_scope` = 导入方
  module scope；实例体 sema 也在导入方符号环境。结果：定义模块泛型体里的
  非限定全局变量
  - 若导入方有同名全局 → 静默绑定到导入方（错误值）；
  - 若没有 → 编译可能通过、运行期 `undefined variable`。
  正确语义应是**实例属于泛型定义模块**：body 的名字解析和函数
  `root_scope` 都指向定义模块。修复需要：
  1. 实例记录 `owner_module`（方法取 struct 所属模块；自由函数取定义模块）。
  2. 保留定义模块的 sema 全局作用域（driver 目前编译完销毁），实例
     signature/body walk 时切到该作用域；局部参数/类型参数仍就近遮蔽。
  3. 编译实例体时用 owner sema scope；实例函数运行期 `root_scope` 指向
     owner module 的 runtime global_scope（closure_scope parent 同步）。
  4. 字节码/容器记录每个实例函数的 owner module 引用，`.cxb` 加载后同样
     恢复 root_scope；模块图已有 owner。
  5. 类型实例仍是 VM 全局 `type_t`，但方法/静态属性登记与函数归属一致。
  当前跨模块泛型函数/方法的**函数调用**多数已因 `FUNC_REF fid` 正常；问题
  集中在全局变量、模块 import 别名、私有符号等词法环境。

**跨模块实例归属（2026-10-22，两阶段流水线已落地）**
- 模块编译拆为两阶段：`compile_module_impl(prepare_only=true)` 先完成所有模块
  的 lex/parse/desugar/sema，保留 `sema`/`sema_global`/tokens/AST；随后按
  `vm->module_order`（依赖先）统一 compile/注册/exports。
- 跨模块 sema 解析使用 `module_t.sema_exports`（直接导出 + `export *` 递归），
  不再依赖已执行注册段的 runtime exports；少数必须读 runtime attributes 的
  跨模块静态成员路径通过 `sema->module_finalize` 回调按需 finalize。
- 函数 id 改为 VM 全局唯一（`vm->next_program_func_id`）。
- 类型登记改为**全 VM 共享** `vm->sema_types_global`：所有模块共享同一
  `sema_type_t`/id；compiler hoist 用全局 `defined_type_ids_global` 保证每个
  程序类型只 `DEFINE_TYPE` 一次。这是跨模块泛型方法/递归类型稳定的关键。
- 泛型实例的 AST/函数/类型直接进入 **owner 模块** 的
  `prog/funcs/types`；导入方 sema 只返回签名/fid。全局 worklist
  `sema_generic_drain_pending` 交替扫描所有模块直到没有未 walk 实例。
- 已删除实例运行期的 `IMPORT + SET_ROOT_SCOPE` 桥接与相对 IMPORT 补丁；
  实例函数天然属于 owner 模块字节码。跨模块静态方法/实例方法走
  sema 符号签名（runtime attributes 仅作回退）。
- 导出全局、私有全局、定义模块 import 别名、方法体全局读写、
  `bc emit → run .cxb`（含泛型方法与 reexport）均已验证。


**已支持（2026-10-22）**
- 跨模块 interface：`implements il::IW[S]`、约束 `T extends il::IW[T]`
  （parser 支持 `::` 限定名，sema 经 `sema_exports` 解析接口符号；
  跨模块接口重解析使用 owner 模块 token pool）。
- 泛型方法显式类型实参：`v.method[U](...)` 与 `Type::method[U](...)`
  （callee `AST_INDEX(MEMBER/SCOPE_REF)` 进入方法实例化路径，
  与推导路径共用签名/实参校验）。
- `comptime if (T extends PATTERN)` 实例内特化：RHS 用 PATTERN 语法，
  CTFE 调用 `sema_pattern_match` 折叠 bool，再由 comptime if 裁剪分支
  （支持 `own *?`、`<...>`、slice/array/func 等通配）。

**仍待（需决定是否支持）**
- 签名期错误（如返回类型未知）尚未输出实例链 note；体错误已覆盖。
- comptime 值包“裸 `N` 作为 tuple 值”：设计只说“编译期 tuple 消费”，未给
  语法。当前定义为 `N[i]` / `len(N)` / `[N[0]]T`，裸 `N` 明确报错。
- 运行期可变参数：采用**编译期 AST 展开**（`args[i]`/`len`/`...args`；
  不生成 tuple 局部变量、不改 ABI），设计见
  [runtime-variadic-tuple-design.md](runtime-variadic-tuple-design.md)，
  已实现；裸 `args` 明确诊断，需要 tuple 时显式 `.{ ...args }`。



- 跨模块：`import lib; lib::Vec[i32]` 的实例化（使用方 sema 生成实例）。
- 文档：更新 `README.md` / `docs/milestones.md` 的 M7 状态；
  在 `m7-design.md` 顶部记录实现锚点（文件/函数名）。
- 示例：`examples/generics/`（容器 + 算法 + 约束）。
- 质量门：0 告警、全量测试、示例 54、format 幂等、0 泄漏、
  `.cxs/.cxb` 往返不退化（泛型不改变字节码格式）。

## 3. 测试矩阵

| 层 | 测试文件 | 覆盖 | 状态 |
|---|---|---|---|
| parser + fmt | `tests/generic_test.cpp`（`GenericParse.*` / `GenericFmt.*`） | 五种声明、参数形态、非法位置、fmt 幂等 | ✅ |
| 函数端到端 | `tests/generic_test.cpp`（`GenericRun.*`） | 显式/推导/默认/函数值/递归/所有权不对称/foreach | ✅ |
| 负例 | `tests/generic_test.cpp`（`GenericReject.*`） | 数量/冲突/无法推导/函数体错误/深度上限/局部泛型 | ✅ |
| 类型端到端 | `tests/generic_test.cpp`（`GenericType.*`） | 容器/别名/方法/嵌套/递归/推导 | ✅ |
| 泛型方法 | `tests/generic_test.cpp`（`GenericMethod.*`） | `map_into[U]`/静态泛型方法/非泛型 struct/负例 | ✅ |
| 值参数 | `tests/generic_test.cpp`（`GenericValueParam.*`） | 显式/数组长度推导/默认值/类型值参数 | ✅ |
| 值参数包 | `tests/generic_test.cpp`（`GenericValuePack.*`） | 索引/len/空包/数组长度/负例/fmt | ✅ |
| 参数包 | `tests/generic_test.cpp`（`GenericPack.*`） | 显式/推导/空包/len/`<...Args>`/别名+struct 包/负例 | ✅ |
| 约束 | `tests/generic_test.cpp`（`GenericConstraint.*` / `GenericReject.Constraint*`） | 通配符/构造/`&`/`|`/struct/const/负例/fmt | ✅ |
| interface | `tests/generic_test.cpp`（`Interface.*` / `InterfaceReject.*`） | §31 声明/implements/签名/成员复制/约束/负例 | ✅ |
| 跨模块 | `tests/driver_import_test.cpp`（`DriverImport.CrossModule*`） | 泛型 struct/函数/别名/方法/同名隔离 | ✅ |
| 回归 | 全量 `clux_test` + examples 扫描 | 不退化 |

## 4. 风险与开放问题

1. **实例 AST 的重新解析**：依赖"token 生命周期覆盖 sema 全程"与"重新解析
   与原解析等价"。缓解：实例化只在 sema 内发生（token pool 此刻必在）；
   `parse_func_def` 的失败按 internal error 报出，不静默；前脱糖单独重跑
   （`desugar_func`）。若将来 token pool 改为流式/提前释放，这里必须改回
   显式深拷贝。
2. **`prog->funcs` 追加与既有收尾逻辑**：销毁/clone 函数在 3b 收尾追加，
   实例在 3b 中追加；两者顺序与 `attach_*_fns` 的交互需要验证（尤其实例函数
   体内的 `clone`/销毁点会按需生成新函数）。
3. **compiler 对泛型定义的遗漏**：已引入统一谓词 `ast_decl_is_generic()`
   （parser/ast_decl.h），compiler 的预扫描/类型名字绑定/struct 成员初始化/
   函数名绑定四个入口已接入；新增遍历 `prog->funcs` 的代码必须继续调用它。
4. **方法实例的成员作用域**：`struct_type_t::member_scope` 是运行时作用域；
   实例方法必须在类型 seal 后、compiler 成员初始化之前注册好。
   跨模块实例（P7）会再次遇到"源模块 sema 已销毁"的回退路径。
5. **类型实例的身份**：mangling 去重 vs `type_struct` intern 去重是两层；
   命名实例应 `type_struct_mark_unique`（设计 §29.2 名义匹配要求），
   否则字段表相同的两个泛型实例会被合并。
6. **诊断位置**：克隆节点沿用定义点 token，实例化链必须补齐；否则用户只看到
   "lib.cx 内部错误"而不知道是哪个调用点实例化的。
7. **性能**：全单态化 + 深拷贝的编译期开销。缓存按 mangling 去重是最低要求；
   是否需要实例化预算（总数上限）留到 P7 评估。

## 5. 验证命令（本机）

```powershell
cmake --build build            # 不要 clean（ICU 数据文件无重建规则）
$env:TMP = (Resolve-Path build\tmp).Path; $env:TEMP = $env:TMP
.\build\clux_test.exe
```

阶段内至少跑：`--gtest_filter=ParseGeneric*:Fmt*:SemaGeneric*:*Generic*` + 全量。
