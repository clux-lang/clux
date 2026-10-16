# clux M3 设计文档：指针与所有权

## 目标

**轻量的所有权 + 指针系统**，远轻于 Rust：

- **无 borrow checker 全局推断**——没有生命周期泛型、没有全局借用求解
- **显式作用域标注**声明借用存活上限，检查是局部集合比较
- **函数边界黑盒**——调用点与函数体互不深入，逃逸检查模块化隔离
- **运行期零检查**——ref 就是裸指针，逃逸检查全编译期

所有语法决策由用户 2026-09-29 逐项确认。

> **实现状态：✅ 已完成（§3–§12 + §13.2）**。单所有权系（`own`/`ref`/`fatal`）与
> RC 系（`share`/`weak`）均已落地，测试见 `tests/driver_m3_test.cpp`；实现位置：
> `src/vm/type_ptr.c`（含 `rc_block_t`：atomic 强/弱计数 + CAS `upgrade`）、
> `src/vm/builtin_upgrade.c`（`upgrade` 内建）。
>
> **§13.3–§13.4（销毁点的编译期生成）仍未实现**：当前销毁走 VM 运行时路径
> （`scope_destroy` 遍历 owned → `ptr_free_owned_recursive` 递归释放），
> compiler 层生成销毁点、多后端统一留给 M7 C 后端 / M12 原生后端。

---

## 1. 核心检查问题

所有权系统要检查的核心问题是**引用作用域逃逸**——引用是否活得比原生长（被引用对象）更久。整个系统的规则都服务于这一靶心：**引用不得超越被引用对象的存活期**。

---

## 2. 两套相对独立的指针系统

| 系统 | 类型 | 定位 |
|------|------|------|
| 单所有权系 | `own` / `ref` / `fatal` | 已实现 |
| 引用计数系 | `share` / `weak` | 已实现（§12） |

两套系统相对独立，不是同一机制的两面。

---

## 3. 单所有权系语义

### 3.1 `own`（唯一所有权）

```
var a: own *i32 = new i32{123};
```

- **绑定作用域**：own 指针声明所在的作用域退出时**自动销毁**（无显式 drop/free）
- **递归销毁**：own 销毁沿字段递归——`own *Struct` 且 Struct 含 own 字段时，销毁递归传导
- **own 不可 copy**：只能通过 `move` / `clone` 操作（见 §7）
- **禁止全局 own**（见 §10）

### 3.2 `ref`（借用引用）

```
var b: ref *i32 = a;   // ref 由 own 隐式产生
```

- **由 own 隐式产生**：`var b: ref *i32 = a;`（`a` 是 own）——ref 是借用的表达
- **运行期零成本**：ref 就是普通指针值，逃逸检查全编译期
- **可重绑定**（参考 C 规则）：`r = c;` 改变指向；**重绑定 = 对 ref 变量的赋值**——故 `const ref *T` 自然禁止重绑定（const 变量赋值既非法，m2 §10）
- **可写性取决于被指向是否 const**：`r.* = 123` 合法（指向非 const 时）
  - **竞争写不是语言职责**：开发者用同步原语负责（未来标准库范围）
- **copy 允许**：ref 是值拷贝语义（参数传递即拷贝指针值）

### 3.3 `fatal`（将亡值）

- **move/clone 的返回值**，表示将亡值（所有权必须转移）
- **禁止命名**：`var p: fatal *i32 = ...;` 非法——fatal 不能作为变量类型标注
- **禁止丢弃**：表达式结束时若未被 own 接收 / 函数（fatal 参数）接收 / share 创建（§12.1）→ **编译错误**（不是运行时销毁，是编译期接管检查）
- **生命周期 = 一整个表达式**：fatal 值可在表达式中任意流转（含 `as` 转换），只是不能存进变量
- **sema 视角是真实匿名 value**：无名字的临时值
- **函数签名是唯一例外的 fatal 场景**（见 §4）

---

## 4. 函数边界

```
func f(x: fatal *i32): void;           // 参数可 fatal（唯一有名场景）
func f(x: ref *T): '<p> ref *T;        // 返回值 ref 必标注
```

- **参数/返回值只能 `ref` 或 `fatal`，不能 `own`**
- **fatal 参数**：唯一允许 fatal 有名字的场景；函数内**必须消费**——要么用 own 变量接收（本地接管），要么 `return` 出去（所有权继续传递）
- **fatal 参数的调用点**：必须 `move`/`clone` 传入（`free(move(a))`）
- **返回值 ref 必须标注**（§5）；返回值作用域**必然来自参数或全局**（§6）
- **参数 ref 的体内语义**：函数体内视作参数 `x` **一定存活**（整个函数体期间）；参数是 **copy 传递**，体内的 `x` 与调用点无关——体内重绑定不影响调用点，体内派生的借用默认标注当前作用域，调用点实际传入的标注只影响调用点侧检查
- **黑盒原则**：调用点与函数体互不深入——调用点只消费边界签名（参数标注 + 返回值标注），函数体只按"参数一定存活 + 签名标注"检查内部派生的借用。逃逸检查完全模块化隔离

---

## 5. 作用域标注

```
var x: '<a> i32;                 // 标注任何类型的变量
var s: '<a> MyStruct = .{...};   // 含 ref 字段的 struct 变量定义必标注
func f(p: ref *T): '<p> ref *T;  // 函数签名：返回 ref 必标注，集合 = 参数名 ∪ {*}
func g(p: ref *T): '<p> Holder;  // 返回值**含 ref 字段**（Holder 内嵌 ref）同样必标注
```

- **语法**：`'<a,b,c>`，**前导，紧跟 `:` 之后**（`var x: '<a> i32`）
- **不是类型的一部分**——是**位置信息**，绑定在 value 上
- **`'<*>` = global**（全局作用域）
- **min 语义**：`'<a,b,c>` = 变量的生命周期不得超过 min(a,b,c) 作用域
- **可化简**：`'<*,a>` 等价于 `'<a>`（global 不缩小约束，自动消除）
- **推断**：ref 变量未显式标注时推断为**当前定义作用域**；重绑定时校验是否逃逸
- **强制规则**（两侧共用同一递归判定：类型**含** ref 指针，不只是顶层）：
  - 函数**返回 ref 时必须标注**（顶层 `ref *T` / `ref []T`）
  - **返回值类型含 ref 字段时同样必须标注**（struct/tuple/union/array/option
    内嵌 ref）——聚合内 ref 字段的借源无法从构造式静态推断，不标注即**指针
    逃逸**：调用方拿到借用了被调用方局部/参数的值，却没有可判的存活上限
  - 结构体/共用体**含 ref 字段时，该类型变量定义时必须标注**（绑定在变量上）
  - 两侧都标：被调用方标返回值的借用来源（`'<p>`），调用方标自己那个含 ref
    字段的变量（`var h: '<x> Holder = f(x.&)`）
- **字段访问继承**：`var p = s.aaa;` 中 `p` 继承 `s` 的作用域绑定
- **检查算法**：ref 的作用域 ≤ own 的作用域（标注集合比较，直接映射作用域树嵌套）

### 5.1 含 ref 聚合类型的**赋值**作用域检查

声明侧的"必须标注"只保证每个含 ref 的变量**有一个承诺**；赋值时还要保证
**承诺成立**——否则用户随手写一个标注就把检查架空了。

**语义（方向极易写反，务必按这条读）**：标注 `'<A>` 不是"变量活到 A"，而是
"本变量持有的借用**至少活到 A**"，即对**被借对象**存活期的承诺。因此把值赋给
`g: '<A>` 时要求

```
借源作用域 ⊇ A          等价于 within(A, 借源作用域)
```

| 情形 | 判定 |
|---|---|
| 承诺活到全局 `'<*>`，实际只借到参数 p（比全局短） | ❌ 承诺不成立 |
| 承诺活到参数 p，实际借自**更内层的块局部** | ❌ 承诺不成立 |
| 承诺活到参数 p，实际借自**全局**（比 p 久） | ✅ 承诺成立 |
| 承诺与借源同锚点（都活到 p） | ✅ |

> `'<*>` 是**最强**承诺（"我借的东西活到程序结束"），不是最弱——这是最容易写反
> 的一处。曾经按"目标更宽就安全"实现，结果把成立/不成立判反了。

**实现**：`sema_symbol_t.borrow_set`（借源集合，固定容量 8）+ 两处接点——

| 接点 | 行为 |
|---|---|
| 变量声明 | 从 init **递归收集**借源记入符号（只记不报；"必须标注"由声明侧负责） |
| 赋值 | 从 rhs 收集借源 → 按上表校验（不通过即诊断）→ 覆盖目标的借源集合 |

**递归收集**覆盖 `move/clone`（`AST_MOVE`）、构造表达式（字段值逐个递归）、
取址（`AST_ADDR`）、以及变量引用（**继承**该变量的借源集合）。四个实现要点
（都踩过）：

- **`x.&` 的借源是 `x` 本身，且与 `x` 的类型无关**——`gx: i32` 这种平凡标量也
  必须收。若沿用只认 own/ref/opaque 的 `init_borrow_src` 过滤，`gx.&` 收不到
  任何借源、整条检查空转。
- **变量引用同样不能用 `init_borrow_src`**：它只认 own/ref/opaque，含 ref 的
  **聚合**会被滤掉，导致"经变量继承"这条路径整条失效（`g = h` 取不到源）。
- **借源侧的锚点解析必须与标注侧同源**：参数名在 `'<p>` 里解析到的是**参数
  作用域**（用符号身份比对，`sema_symbol_t` 没有 name 字段）；模块级变量的
  `own_scope` 为 **NULL**，要视作 `global_scope`。两者错一处就会误拒合法的
  同锚点/更长借源赋值。
- **目标锚点解析失败时不能 `continue`**：跳过等于放弃这条约束（实测逃逸赋值
  照样放行）。退回目标符号自身的定义作用域作保守界。

> 为什么不能只靠标注：标注是用户对"借的东西活多久"的**承诺**，借源是**事实**。
> 聚合的 ref 借源无法从类型静态推断（构造式可能多源），所以声明侧强制标注；
> 赋值侧再用 `borrow_set` 这套 provenance 校验承诺成立。顶层 ref 单源，一直用
> 单个 `borrow_src` 做同一件事。

---

## 6. 返回值作用域来源

- 函数返回值的（标注）作用域**必然来自参数或全局**
- 返回值 ref 要么借用自某参数（标注该参数名），要么借用自全局（标注 `'<*>`）
- 这使函数边界成为完整的逃逸契约：调用点只看返回值标注即可确定其存活上限

---

## 7. move / clone 原语

```
var b = clone(a);   // 深拷贝，返回 fatal，原对象不变
var b = move(a);    // 转移所有权，返回 fatal，原对象进 TDZ
```

- **own 不可 copy**，只能 move / clone
- **move**：
  - 转移所有权，返回 fatal（必须被 own 接收）
  - **move 后原对象进入 TDZ**（复用现有 TDZ/flow_init 机制）：再访问报错，除非重新赋值（`x = move(...)` 重新激活）
  - **有 ref 存活禁止 move**——防止借用悬空
  - move 后原 own 的销毁 = no-op（所有权已转移，TDZ 态退出作用域）
- **clone**：
  - **深拷贝整个对象图**（own 唯一所有权 + 递归销毁的必然推论，否则 double free）
  - **深拷贝遇到 ref 字段直接 copy**（ref 允许 copy，指向同一处，不深拷被指物）
  - **ref 不影响 clone**：有 ref 存活时仍可 clone（仅 move 受限制）
- **含 own 字段的复合类型**：struct/cunion 内有 own 字段时不可 copy，须 move/clone（own 语义沿字段嵌套传导）

### 7.1 复合类型的将亡值变体 `fatal T`（2026-10 补：跨越函数边界）

§7 的 move/clone 只规定了语义，没规定**含 own 字段的复合值如何跨函数边界**：
`func read(b: Box)`（Box 含 `own *i32`）用 `read(a)` 调用时，契约是"按值传参 =
拷贝"，而 Box 的整块 memcpy 会连裸指针一起复制 —— 源与目标各自递归销毁同一
堆块 = double free。返回值同理。§4 的"参数/返回值只能 ref 或 fatal"此前只对
指针/切片生效（`type_is_own(Box)` 为假）。

**`fatal T`**（T 为含 own 资源的 struct/tuple/array/option/union）补齐这一环：

```
struct Box { p: own *i32; n: i32; }

func take(b: fatal Box): i32 {      // 形参：将亡值，函数内必须接管
  var owned: Box = move(b);         //   move 接管 → 普通 Box
  return owned.n;
}
func mk(n: i32): fatal Box {        // 返回：把所有权交出去
  var b: Box = .Box{ .p = new i32{n}, .n = n };
  return move(b);
}

var a: Box = .Box{ .p = new i32{7}, .n = 7 };
take(move(a));                      // 调用点：move 产生将亡值
var c: Box = mk(9);                 // fatal → Box 的接管 = 转移
```

- **类型**：与 T **逐字节同布局**（ownership 不参与布局），仅
  `ownership = OWN_FATAL` 不同的独立 intern 实例（去重按「字段表 + ownership」）。
- **产出**：`move(x)` / `clone(x)`（x 为含 own 资源的复合值）；`new` / `make`
  产 fatal 指针/切片（既有）。
- **move**：整块 memcpy + 递归清空**源**顶层 own/fatal 指针（源不再持有，
  作用域退出是 no-op）。
- **clone**：递归深拷贝对象图（own 字段分配独立新堆块，ref 字段复制指针）；
  **源保持有效** —— clone 是复制语义，转移走 move / fatal 参数与返回值。
- **接管**：`fatal T → T` 的隐式转换是**转移**（memcpy + 清空源）。
- **拒绝**：目标持有 own 资源而来源不是将亡值 → 编译错误（要求 move()/clone()；
  只想借用就写 `ref *T` 形参）。
- **无资源折叠**：T 不含 own 资源时 `fatal T` 折叠为 T 本身（不引入互不兼容的
  新类型）。`own T` / `ref T`（T 非指针/切片）非法。
- **销毁**：与 T 完全相同的递归路径（按字段偏移分派，与 ownership 无关）。
- **运行期物化**：~~hoist 声明阶段 `LOAD_TYPE <id>` → `SET_OWNERSHIP FATAL` →
  `SEAL` 按 ownership 去重~~ **已废弃**（2026-10-16 重构：复合类型禁止携带
  指针/切片字段，fatal 复合变体不再存在。fatal 仅限顶层 `fatal *T` / `fatal []T`，
  构造时直接指定 ownership）。

### 7.2 所有权**扩散**：含 own 字段的复合类型整体进入单所有权体系

> **已废弃**（2026-10-16）：复合类型字段禁止携带指针/切片，本节描述的
> "含 own 字段的复合类型"场景不再存在。自引用数据结构待后续重新设计。

§7 的规则此前只落在 `own *T` 这一形态上，复合类型（struct/tuple/union/option/
array）**递归含** own 指针时没有被覆盖——四个漏水点：

| 位置 | 修复前 | 现在 |
|---|---|---|
| 构造表达式 `.T{...}` | 产出普通 T（新堆块却可被当普通值拷来拷去） | 产出 **`fatal T`**（新堆块 = 将亡值） |
| 函数参数 | `func f(b: Box)` 合法 → 按值传 = 与调用方共享内嵌堆块 → 双释放 | **只能 `fatal T`**（借用写 `ref *T`） |
| 变量声明/赋值 | `var b: Box = a` 静默放行（浅拷贝） | **禁止 copy**：只能由 fatal 将亡值初始化 |
| 参数位的 ref 字段 | 不查标注 | 与变量声明/返回值同一条：**必须标注作用域** |

规则本身一句话：**复合类型只要（递归地）含 own 指针，就整体按 `own *T` 的双规则
办事**——不能 copy（须 move/clone/接管），跨函数边界只能以 `fatal T` 出现。

```rust
struct Box { p: own *i32; n: i32; }

var a: Box = .Box{ .p = new i32{5}, .n = 6 };  // 构造产 fatal Box → own 变量接管
func mk(): fatal Box { return move(.Box{ .p = new i32{1}, .n = 1 }); }
func take(b: fatal Box): i32 { var x: Box = move(b); return x.n; }
func read(b: ref *Box): i32 { return b.n; }    // 只想借用

var b: Box = a;                                 // ❌ copy
var b: Box = move(a);                           // ✅ 转移
var b: Box = clone(a);                          // ✅ 深拷贝
take(.Box{ .p = new i32{3}, .n = 4 });          // ✅ 构造直接喂 fatal 形参
```

**sema 与运行期类型必须一致**（实现要点）：构造产 fatal 时，构造节点的**类型
槽位**也要改成 fatal 变体，让编译器发 `LOAD_TYPE <变体 id>`。只改 sema 的 shadow
类型会让运行期值带非 fatal 类型，走到 fatal 形参/返回位时"要不要转移"的判定失败、
退化成 clone → 构造临时值仍持有内嵌堆块 → 双释放（实测 access violation）。

**cunion 例外**：它的 dispose 是 no-op（无 tag、开发者自负），标成 fatal 只会让
所有权规则与销毁语义脱节。

**销毁边界（细节）**：`pop_scope` 的递归释放**只认 `OWN_OWN`**。fatal 的归宿只有
两个——被 own 变量/参数/返回值接管，或被 `_ = ` 显式丢弃；两种情况下堆块都已归
接管方或由 DISPOSE 释放，所以销毁路径碰它就是 double-free。fatal 复合类型与 base
逐字节同布局，递归销毁按字段偏移分派，与 ownership 无关 → 两者天然同路径。

**构造必须"物化即转移"**：每个构造分支把成员/字段值 memcpy 进新块后，都要对**源
临时值**做 `ptr_clear_owned_recursive`（把内嵌 own/fatal 指针置 NULL），否则源临时
值在作用域退出时先释放内嵌堆块、新块再释放同一个 → double-free。

- struct 分支：逐字段按 `field->type` 清空（既有）。
- **union 分支**：按激活 member 的 `payload_type` 清空**两处**——`casted`（异类型
  转换的产物）与原 `payload`（两者都是本次构造的临时值；同类型短路时是同一对象，
  第二次调用幂等空转）。此处曾漏清，导致 union 含 own payload 时必然 double-free
  （access violation），2026-10-13 修复。

### 7.3 推断与扩散的合流：`var a = .T{...}` 不必标注

`var` 推断时把 fatal 剥离成可用类型（**这是 17839d9 / ca00b91 的意图**，与 §7.2 的
"构造产 fatal T"是两条独立提交，必须同时成立）：

| 推断源 | 推断结果 |
|---|---|
| `fatal *T` | `own *T`（指针的所有权升级靠推断，不必写 `own *T`） |
| `fatal []T` | `own []T` |
| `fatal T`（复合） | 基础类型 `T`（OWN_NONE）——变量接管后类型就是 T |

所以下面这种写法**不需要**显式标注 `a` 是 `own Test` 还是 `Test`：

```rust
struct Test { data: own *i32; }
var a = .Test{ .data = new i32{123} };   // 推断为 Test
func mk(n: i32): fatal Test { ... }
var b = mk(7);                            // 推断为 Test（fatal 被剥离）
var p = new i32{9};                       // 推断为 own *i32
```

**两者合流的正确形态**（逐字节看过 bytecode，`repro/own-infer` 与
`tests/driver_own_infer_test.cpp` 锁定）：

```
LOAD_TYPE <fatal 变体>     ← 构造点（§7.2：产物是 fatal T）
... CONSTRUCT N            ← 运行期产出 fatal 值
LOAD_TYPE <基础类型 T>     ← DEFINE 类型槽位（推断写回的显式类型）
DEFINE "a"                 ← op_define 走 value_implicit_cast：
                              fatal T → T 的**接管转移**（堆块归变量）
```

缺任何一侧都会退化：只有推断没有"构造产 fatal"→ 接管无从发生（语义上不算转移）；
只有"构造产 fatal"没有推断写回 → 运行期值带 fatal 类型而变量声明为 T，转移判定
落空 → 变量析构不释放堆块（泄漏）或与临时值双释放。

### 7.4 fatal 变体的**身份**与 `fatal → base` 的可靠折回

`fatal T` 与 `T` 逐字节同布局，但是**不同的 `type_t` 实例**。凡需要"把 fatal 折回它
声明的类型"再比身份的地方，都必须拿到 **base 的规范身份**，否则判定必然失败。

**为什么不能"按同布局重新 intern 一个 base"**：具名 `struct` 会被标记
`unique`（`MARK_UNIQUE`，见 `struct_type_t::unique`），而 `type_struct_seal` 的
去重**跳过 unique 类型**（具名类型各有独立成员命名空间，合并会串味）。于是
`type_struct_intern_with_ownership(fatal_I, OWN_NONE)` **每次新建一个实例**，
指针永不等于原 base —— 实测 `.?I{ .I{...} }` 的 option 内层比对因此误报
`cannot initialize optional ?I with fatal I`。

**正解**：在**唯一同时握有 base 与变体**的地方登记派生关系 ——
`type_fatal_variant_of`（`src/vm/type.c`）创建变体后写 `vm->fatal_bases`
（`[variant, base, …]` 成对）；查询走 `type_fatal_base_of`，命中登记表即返回
**规范 base**，未命中才退回"按布局重新 intern"的兜底（该兜底对 unique 类型不可靠，
仅供比较参考）。

> 为什么不在 `type_t` 里加 `fatal_base` 字段：`type.c` 的内建类型用定位初始化器
> （`(type_t){…}`）逐个列出字段，加字段会给每一处带来 `-Wmissing-field-initializers`
> 噪声；放 vm 侧映射表既不动布局，也不影响类型池的生命周期（元素两两都由各自类型池
> 拥有，映射表只存借用指针）。

### 7.5 fatal 聚合类型的**返回值**路径

`func mk(): fatal [2]I` 的返回值交回调用方时会经过两处对"声明返回类型"的处理：

| 步骤 | 行为 | 对 `fatal` 声明是否正确 |
|---|---|---|
| 第 6 步：返回值 safe_cast 到声明返回类型 | `value_implicit_cast(result, sig->return_type)` | **否**——`base → fatal` 不是合法的隐式转换方向（各族的 `implicit_cast` 只认 `fatal → base` 的接管）。array/union 直接失败：`array: implicit cast only within layout-compatible array/tuple` |
| 第 7 步：fatal 返回值的**转移** | `value_move` + `value_relabel_into`（按声明类型就地重贴标签） | **是** |

所以第 6 步必须**让开**：声明返回类型 `fatal` 且携带资源时跳过它，交给第 7 步。
struct / 指针族此前靠 base→fatal 的布局兜底偶然通过，array / union 暴露了这个漏洞。

同理，**形参接收**侧（`func_vcall` 的参数循环）也在 `value_move` 之后做一次
`value_relabel_into`，保证形参运行期类型与声明的 `fatal` 类型严格一致。

### 7.6 own 指针的 move 边界：只能整体 move，闭包捕获不可 move

TDZ 是**符号级**的（`sema_symbol_t.moved`，只跟踪变量名）。这条事实推出两条 move 约束：

**约束 1 —— 聚合的 own 字段不能单独 move**

字段没有"局部 TDZ"：`move(s.p)` 把资源掏走却没有任何地方记录 `s.p` 已失效，
于是同一个字段可以重复 move、被读成空值/悬垂。

```rust
struct Box { p: own *i32; n: i32; }
var b: Box = .Box{ .p = new i32{7}, .n = 1 };
var q: own *i32 = move(b.p);   // ❌ cannot move a struct/aggregate field that owns resources
var c: Box = move(b);          // ✅ 整体 move：TDZ 标在符号 b 上，语义完整
var r: own *i32 = clone(b.p);  // ✅ clone 不改变源的绑定状态
```

判据：操作数是 `AST_MEMBER` / `AST_INDEX`（字段或元素访问）且其类型 own / fatal /
含 own 资源。递归生效（`move(o.i.p)` 同样拒绝）。

**约束 2 —— 闭包捕获（含捕获里的结构体）不可 move**

捕获作用域由闭包的**所有调用共享**：move 把捕获槽标进 TDZ 后**无法恢复**，
第二次调用就读到已转移的槽位。

```rust
var p: own *i32 = new i32{7};
var f = func |(q: own *i32 = move(p))| get(): i32 {
  var r: own *i32 = move(q);   // ❌ cannot move closure capture 'q'
  return q.*;                  // ✅ 借用读取
};
```

判据：符号带 `is_capture` 标记（由 `resolve_func_captures` 在解析捕获时置位——
捕获符号**可能没有 `own_scope`**，括号捕获 `(q: T = move(p))` 的 `q` 就是
`own_scope == NULL`，所以不能靠作用域反查，必须用符号标记）。

**边界**：闭包体内**自己定义**的局部变量（非捕获）照常可 move —— 它的 TDZ 随该次
调用消亡，不存在跨调用残留。

---

## 8. 运算符与语法

### 8.1 `new` 表达式

```
var o: own *i32 = new i32{123};
```

- **`new` 替代构造中的 `.`**——`new T{...}` 是 `.T{...}` 的堆分配形态，返回 `own *T`
- 泛化任何类型（含标量）→ `own *T`

### 8.2 后置取址 / 取值

```
var p = x.&;    // 后置取地址：由值得指针
var v = r.*;    // 后置解引用取值：由指针得值
r.* = 123;      // 解引用赋值（SET）
```

- 运算符**后置**，与字段访问 `.` 同一位置族
- **解引用 GET / SET 严格分离**：与现有 member 读写（`s.field` 读 / `s.field = v` 写）处理一致

### 8.3 可空指针

- 本语言**不需要空指针**（nil 不混入指针值域）
- 可空指针用 **option 配合指针**：`?*T` / `?own *T` / `?ref *T`
- **option 视为特殊的 struct 复合**（`?T` 是包一层 tag 的复合类型，衔接现有 ?T 语义）

### 8.4 opaque（≈ C 的 void*）

- 特殊类型 `opaque`，等价于 C 语言 `void*`
- **任何指针 → opaque 隐式转换**；**opaque → 任何指针显式转换**（`as`）
- fatal 值可 `as` 转换（仅禁止命名，不禁止表达式内流转）
- FFI 示例（FFI 暂不支持，语法先行）：
  ```
  extern func malloc(size: usize): fatal opaque;   // size_t
  extern func free(ptr: fatal opaque): void;
  ```

### 8.5 无 unsafe

- 本语言**不需要类似 Rust 的 unsafe 块**
- FFI 函数纳入本体系：所有权规则对 FFI 边界同样生效，不豁免（见上 opaque + fatal 形态）

---

## 9. 闭包

闭包是隐式 struct + 函数的语法糖：捕获列表 = struct 字段，闭包对象 = struct 实例
+ 函数指针。捕获是 clone 值（非引用），定义点求值后存入闭包独立副本。指针捕获
按 struct 字段所有权规则处理，不一刀切禁止。

### 9.1 own 捕获

- **允许 own 捕获**，但必须显式 `move()` 或 `clone()`：
  ```
  var p: own *i32 = new i32{42};
  var f = func |q = move(p)| add(x: i32): i32 { return q.* + x; };
  // q 推断 own *i32，move 转移所有权，p 进 TDZ
  ```
- **裸捕获 own 变量禁止**：`|p|`（p 是 own *T）= copy → 编译错误（own 禁止 copy，
  §3.1）。必须 `|q = move(p)|` 或 `|q = clone(p)|`

### 9.2 fatal 捕获

- **闭包内无 fatal 变量**：`|x = p|` 其中 p 是 fatal，x 接管为 own（fatal 是将亡值，
  clone/move 接管后变 own，闭包成为所有权持有者）
- 等同 own 捕获处理，x 自然是 own *T

### 9.3 ref 捕获

- **允许 ref 捕获**，视同结构体 ref 字段——闭包整体必须带作用域标注：
  ```
  var x: i32 = 42;
  var f = func '<x> |r| get(): ref *i32 { return r; };
  // r 是 ref *i32，闭包标注 '<x> = 存活不超过 x
  ```
- **强制标注**：捕获 ref 且未标注 → 编译错误（同 §5 强制规则 B：含 ref 字段的
  聚合类型变量定义必标注，闭包即聚合）
- 标注语法：`func '<a,b,c> |...| name(params): T { body }`——标注在 `func` 后、
  `|` 前
- min 语义不变：`'<a,b,c>` = 闭包存活不超过 min(a,b,c)
- 锚点解析同 §5：参数名 / 全局 / 局部变量名 / `*`

### 9.4 非指针捕获

- 标量 / 复制语义复合类型 —— 无需标注，clone 值（现有语义不变）
- **share/weak 豁免**：share/weak 是运行时普通值（§12.6），允许捕获，无需标注

### 9.5 检查规则汇总

| 捕获类型 | 允许 | 约束 |
|---------|------|------|
| own *T | ✓ | 必须 move()/clone()，裸捕获禁止 |
| fatal *T | ✓ | 接管为 own，闭包内无 fatal 变量 |
| ref *T | ✓ | 闭包必须带作用域标注 |
| opaque | ✓ | 视同 ref（借用），闭包必须带标注 |
| 标量/复合 | ✓ | clone 值，无约束 |
| share/weak | ✓ | 普通值，无约束 |

### 9.6 闭包返回捕获 ref 的标注关系

闭包返回 ref 可来自捕获变量（捕获视同特殊参数，调用闭包时闭包对象必然存活，
捕获的 ref 有效）。但返回值标注与闭包标注须满足嵌套关系：

- **返回值标注的每个锚点须嵌套于闭包标注的每个锚点**
  （返回值存活 ≤ 闭包存活 ≤ 锚点）
- 返回值标注比闭包标注更宽 → 逃逸（调用方以为返回值活到 y，实际活不过 x）→
  编译错误
- 例：`func '<inner> |r| get(): '<outer> ref *i32 { return r; }` ——
  闭包存活 ≤ inner，返回值声明存活 ≤ outer（outer 比 inner 宽）→ 拒绝

---

## 10. 全局规则

- **禁止全局 own**（含任何嵌套位置）：`var g: own *T = undefined;` **不合法**
  - 原因：全局变量初始化为 `undefined` 时，编译期控制流分析**无法保守感知何时已赋值有效**，检查器失去确定性
  - **禁止含 own 字段的聚合结构作全局变量**
- **禁止全局 share**（含任何嵌套位置）：与全局 own 同理——`= undefined` 的延迟初始化使检查器失去确定性；且 share 是**真实运行时 RC 结构**（引用计数 + 目标地址），需要**堆分配**，全局静态存储期在编译期无法完成堆分配
- 全局值域 = **复制语义**（标量 / 数组 / 仅含 ref 的复合类型）
- 全局只有 **ref**（指向全局值的引用指针，标注 `'<*>`）

---

## 11. 检查模型与架构契合

- **作用域树已存在**（sema Pass 3a 构建）：标注集合 → 检查 = "ref 的标注集合是否包含 own 的作用域"，直接映射树嵌套比较
- **TDZ/flow_init 已存在**：move 后 TDZ 直接复用
- **黑盒边界**：与现有"静态检查只交验签名"一致（不递归进函数体）
- **运行期零成本**：ref 是普通指针值；own 的销毁 = 作用域退出时递归释放（VM 层 scope 销毁钩子）
- **fatal 的编译期接管检查**：表达式结束点校验 fatal 值流向（own 接收 / 函数参数），复用现有 shadow 值求值路径

---

## 12. 引用计数系（share / weak）

与单所有权相对独立的第二套指针系统（§2）。与单所有权"编译期检查 + 运行期零成本"不同，RC 系是**真实的运行时数据结构**：

| 系统 | 检查时机 | 运行期形态 |
|------|----------|-----------|
| 单所有权 own/ref/fatal | 编译期（逃逸检查） | own/ref 退化为无标注裸指针，零成本 |
| RC 系 share/weak | 运行期（引用计数） | 真实 RC 控制块（计数 + 目标地址），atomic 计数 |

### 12.1 创建与隔离

**创建源**：RC 指针的创建入口是 **fatal 值**（`var s: share *T = <fatal 值>`）。fatal 的接管去向因此扩展为三路：

1. own 变量接收（单所有权）
2. 函数 fatal 参数接收（单所有权）
3. **share 创建 RC 指针**

move/clone 产物或 new 的分配均可流向 **share** 作为 RC 创建源。

**weak 不直接接受 fatal**：weak 只能从已有 **share** 派生（弱化）或从 weak copy 得来——创建路径强制经过 share，保证 weak 存在时必有 share 曾存活，天然避免"仅弱存活"的死对象。

**单向隔离**：**share 不能转换为 own/ref**——RC 的强引用可多个，无法安全提取唯一所有权。两系之间的唯一通道是 fatal → share 创建（§12.1）；share 到单所有权系无通道。

### 12.2 copy = 计数增加

- share 可直接 copy（`var b: share *T = a;`）——copy 就是强引用计数 +1
- weak copy 增加弱引用计数
- 全部使用正常 `=`（赋值即 copy，计数 +1）；**与单所有权不同，RC 系赋值不要求 move/clone**
- 创建后的传播（share/weak 之间）就是普通 copy

### 12.3 延迟销毁：weak 保命不保访问

- 对象生命周期 = 强引用或弱引用**任一**存活
- share 归零但 weak 仍存活 → **不销毁**（保留内存防悬空），但**禁止访问**（数据视为无效，不得解引用）
- weak 全部归零才真正释放内存
- share 数 = 访问权；weak 数 = 保命权
- 与 Rust"强归零即释放"不同，clux 用"保留但禁止访问"换取 weak 永不悬空

### 12.4 weak 是观察器：经内置函数升级

- weak 自身不能直接访问对象——访问必须先升级为 share
- 升级使用**内置函数 `upgrade`**（语义参考 Rust `Weak::upgrade()` / C++ `weak_ptr::lock()`）：检查强引用是否存活：存活 → 返回 share（计数 +1）；否则返回 nil
- 返回值类型 `?share`（option），调用形态 `upgrade(w)`
- 访问路径强制经升级接口，天然消除 §12.3"禁止访问"的运行期歧义——无强引用时升级得 nil，无从访问

### 12.5 线程安全：atomic 计数

- share 强引用计数与 weak 弱引用计数皆为 **atomic 变量**，多线程安全
- 线程安全范围仅限 RC 计数增减：share copy / weak copy / 销毁递减 / weak 升级检查（升级须原子：检查强计数 > 0 且递增，否则销毁竞态）
- **与 ref 指针无关**——ref 仍是编译期概念，运行期裸指针零成本，不承担线程安全职责
- 竞争写仍非语言职责（同步原语未来标准库范围）

### 12.6 share 是普通值

- share 允许闭包捕获、参数传递等**一切普通变量用法**（对照 §9：闭包只禁 own/ref/fatal 单所有权系指针，不禁 share）
- 复制即计数 +1，天然跨作用域/跨闭包安全
- **循环依赖由用户负责**——RC 不自动解决循环引用，打破环用 weak

### 12.7 与单所有权对照

| 维度 | 单所有权系 | RC 系 |
|------|-----------|-------|
| 检查 | 编译期逃逸检查 | 运行期引用计数 |
| 运行期成本 | 裸指针零成本 | RC 控制块 + atomic 计数 |
| 赋值 | 须 move/clone | 正常 `=`（计数 +1） |
| 闭包捕获 | own 须 move/clone，ref 须标注（§9） | share 允许 |
| 复合类型 copy | 含 own 字段不可 copy | 含 share/weak 字段可 copy（字段 copy 即计数 +1） |
| 循环依赖 | 无环（树状所有权） | 用户负责，用 weak 打破 |
| 与对方互转 | — | share → own/ref 禁止（单向隔离，§12.1）；仅 fatal → share 创建通道 |

### 12.8 开放点（无，骨架定稿）

RC 系 share/weak 语义已全部定稿（2026-09-29）：创建源 fatal、weak 只从 share 派生、copy 计数、延迟销毁、`upgrade` 内置函数、atomic 计数、share 普通值、单向隔离、禁止全局 share。

**实现状态：✅ 已实现**（本节此前写作"实现待后续里程碑"，为过期表述）。落地形态：
`src/vm/type_ptr.c` 的 `rc_block_t { atomic_uint strong; atomic_uint weak; /* payload 紧随 */ }`
与 `rc_block_new` / `rc_block_strong_inc|dec` / `rc_block_weak_inc|dec` / `rc_block_upgrade`
（CAS 递增强计数），`upgrade` 以**内建函数**提供（`src/vm/builtin_upgrade.c`，
返回 `?share *T`），类型 kind 为 `TYPE_KIND_PTR_SHARE` / `TYPE_KIND_PTR_WEAK`。

---

## 13. 销毁的编译期生成（多后端统一，2026-09-30 补充）

### 13.1 背景：多后端目标

当前 compiler 编译到 **VM 字节码**（脚本路径）。未来有**编译为 C / LLVM 的原生后端**（m3-design §后续 / M7 C 后端 / M12 原生后端）。销毁机制必须从"VM 运行时特性"上移为**编译期可生成代码**，各后端只做翻译，不各自实现运行时销毁。

### 13.2 核心原则：作用域退出点显式销毁

- **own 的销毁不在值上携带标志**——**编译期在作用域退出点生成销毁代码**（如调用 `free(ptr)` / `free(own *T)` 的递归释放）；VM 运行时路径用 scope_destroy 遍历 owned + type kind 分派（已落地运行期零标志）
- **谁拥有 = 编译期静态事实**：编译器跟踪每个 own 变量的所有权状态（move 后销毁跳过），作用域退出时生成 `free` 调用序列
- **运行期零标志（已落地）**：指针值就是裸指针（`ptr_value_t { void *ptr; }`），所有权由 type kind 区分（own/ref/fatal），无 tag、无 owns 位

### 13.3 销毁点生成规则

- **销毁点只有两个位置**（clux 无异常/throw，无栈展开路径）：
  1. **return 语句**：函数返回前，销毁当前函数所有活着的 own 变量（按定义逆序）
  2. **作用域结束**：块 / 循环体 / 函数体结束处（含 break/continue 跳出循环体时循环体作用域结束），按定义逆序生成销毁
- **move 后跳过**：`var m = move(p)` 后，p 的销毁点生成 no-op（所有权已转移）；m 的销毁点生成 `free`
- **递归销毁**：`own *T` 且 T 含 own 字段 → 生成递归释放；`clone` 深拷贝遇 ref 字段直接 copy
- **借用不销毁**：ref / `x.&` 借用 → 销毁点 no-op（指向栈值或他人堆块）
- **无异常简化**：因为不存在"异常抛出中途销毁局部变量"的路径，销毁点生成是纯静态的——每个作用域恰好一个销毁序列，控制流（if/while/break/continue）只改变"哪个作用域结束"，不改变"何时销毁"（结束即销毁）

### 13.4 对多后端的意义

- **VM 字节码**：销毁 = return / 作用域结束点发 `CALL free`（或专用 `SCOPE_EXIT` 指令序列，由解释器翻译为 free 调用）
- **C 后端**：销毁 = return / 作用域结束点生成 `free(p)` 语句（含递归释放的辅助函数）
- **LLVM 后端**：销毁 = return / 作用域结束点 IR 生成 `free` 调用
- 三种后端共享同一编译期所有权分析，销毁点唯一来源 = compiler 层

### 13.5 与当前实现的关系

- VM 指针值已落地运行期零标志（§13.2）：`ptr_value_t { void *ptr; }`，所有权完全由 type kind 区分
  （own 总拥有 / ref 总借用 / fatal ptr≠NULL 时拥有），`owns` 标志已移除
- 当前销毁仍由 `scope_destroy` 遍历 owned 列表 → `ptr_free_owned_recursive` 递归释放 +
  vtable dispose 释放 data 块（VM 运行时路径）；多后端统一待 compiler 层实现销毁点生成
- `free` 以**内建函数**形式提供（FFI §8.4 的 `extern func free(ptr: fatal opaque): void` 形态），销毁序列 = 对指针值调用 free

### 13.6 设计决策记录

- **运行期零标志已落地**：`ptr_value_t` 不再携带 `owns` 位，所有权由 type kind 编码（own/ref/fatal）
- **销毁点 = 编译期审计点**：生成处可静态检查"每个 own 恰被销毁一次"（move 转移不算销毁），泄露检查从运行时移到编译期（多后端统一待后续）
- **free 是内建**：不是用户可见的库函数，是编译器销毁序列的目标

---

## 后续（非本里程碑）

- **销毁点编译期生成（§13.3-§13.4）**：当前 VM 运行时路径（scope_destroy 遍历 owned + type kind 分派）已完整工作，多后端统一待 compiler 层实现销毁点生成（M7 C 后端 / M12 原生后端）
- **FFI 落地**：语法已定（§8.4），具体 C 互操作在后续里程碑
- **同步原语**：竞争写由开发者负责，标准库范围
