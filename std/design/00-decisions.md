# std/design/00-decisions.md — 标准库决策台账

> 规则：**未决项不写实现代码**；每条决策有编号、候选、结论、理由、影响面。
> 状态：`待定`（等意见）/ `暂定`（有倾向，待确认）/ `已定`（已拍板，进入设计）。
> 拍板来源：`[需求]` = 直接来自项目负责人的需求；`[调查]` = 从现有实现/规范查证得出。

---

# 0. 已定决策（本轮）

| 编号 | 决策 | 来源 |
|---|---|---|
| D-B1 | **分配器一律显式传递**（容器/函数形参注入），不隐式、不全局 | [需求] |
| D-A5 | **标准库以组合实现**：依赖在初始化时显式注入，不用单例/隐式上下文 | [需求] |
| D-E0 | **hash / fmt / iterator 是协议，不是语言规范**：用 interface + 普通方法，**不新增魔法方法** | [需求] |
| D-F1 | **标准库必须确定性**：任何可观察输出不依赖 hash/地址/声明顺序 | [需求] |
| D-G1 | **新增 `__call__`** 魔法方法（语言修改；之前是遗漏） | [需求] |
| D-E0b | 由 D-E0 推论：**不新增** `__fmt__` / `__hash__` / `__iter__` 魔法方法 | [推论] |
| D-E1 | interface 约束改为**结构匹配**（提供成员即满足，`implements` 变可选断言） | [需求] |
| D-G2 | 结构匹配已实现（`sema_interface_shape_check`，含缺失成员诊断） | [实现] |

---

# A 模块与组合

## D-A1 最小模块集 —— 暂定
`mem / str / string / vec / fmt / io / map / args / sys / test / diag`；通用库（math/time/rand/json）留到 `std` 独立后。
影响：S0 只需 `mem/str/string/vec/fmt`。

## D-A2 命名 —— 暂定
扁平 `std::mem`、`std::str`、`std::vec`、`std::io`、`std::fmt`、`std::map`、`std::sys`。
理由：模块数量少；分层（`std::text::str`）只增加书写成本。后期可加别名。

## D-A3 内部模块 —— 暂定
允许 `std::internal::*`，仅限 `mem` 底座与 `sys` 薄封装，文档标注"非公开、可改"。

## D-A4 平台边界 —— 已定
`std::sys` 是唯一允许 `extern lib` / `extern cfunc` 的模块。其他模块要平台能力 → 先进 `sys` 再包一层。

## D-A5 组合与依赖注入 —— 已定
**每个需要外部能力的类型，在 `new` 时显式注入依赖；不提供全局单例、不读隐式上下文。**

| 类型 | 注入的依赖 | 不注入的（有意） |
|---|---|---|
| `Vec[T]` / `String` / `Map[K,V]` | `Allocator` | — |
| `fmt.Writer` | sink（`*Sink`，interface） | 语言环境/locale |
| `io.File` | `handle` + `Allocator` | 全局 "当前目录" |
| `io.Reader`/`Writer` 适配器 | 下层 reader/writer | — |
| `diag.Renderer` | sink + 颜色开关（显式 bool） | 环境变量 |
| `Clock`（后续） | 时间源 | 系统时钟单例 |

推论（写进各模块设计）：
- 不允许 `std::mem::default()` 这类隐式全局分配器；**只能**由调用方传入 `Allocator`。
- 不允许模块级 `var` 可变状态；`std` 内唯一例外是 `sys` 的 errno 镜像（文档标注）。
- 编译器代码同样遵守：`Lexer.new(source, diag)`、`Parser.new(tokens, diag)`，阶段间显式传参。

---

# B 内存模型

## D-B1 分配器显式传递 —— 已定
- `Allocator` 是**值类型**：`struct Allocator { vtable: *AllocatorVTable; data: opaque; }`（可拷贝）。
- 所有会分配的 API 都要求传入：`Vec[i32].new(alloc)`、`String.new(alloc)`、`Map[K,V].new(alloc)`。
- 容器把 `Allocator` **存成字段**（自身不拥有分配器，分配器生命周期由调用方保证）。
- 不提供"默认堆分配器"的隐式回退；需要堆就显式写 `std::mem.heap()`（返回一个 `Allocator` 值），由调用方决定是否复用它。

## D-B2 OOM —— 暂定
`Allocator` 有两套函数指针：`alloc`（失败 panic，走 `std::rt.panic`）与 `try_alloc`（返回 `?[]u8`）。
容器方法用 `alloc`；需要处理的场景显式用 `try_*` 变体。理由：不把 OOM 塞进每个返回类型。

## D-B3 视图 vs 拥有 —— 已定（沿用语言现状）
`str` / `[]T` 是借用视图；`String` / `Vec[T]` 显式拥有。只提供 `as_str()`/`as_slice()` 显式降级，不做隐式转换。

## D-B4 释放 —— 暂定
`__dispose__` 为主（配合 `using` / `dispose(x)`），`.deinit()` 作为同义直接调用（便于显式场景）。
拥有型容器 **move-only**：提供 `__init_copy__`（移动语义），深拷贝显式 `clone()`。

## D-B5 arena —— 暂定
`std::mem.Arena` 内置（`alloc(size, align) -> []u8`、`reset()`、`__dispose__`），作为 `Allocator` 的一个实现，不设默认。
用途：编译器各阶段的批量存储。

## D-B6 全局可变状态 —— 已定
`std` 内部零全局可变状态（`sys` errno 镜像、`test` 计数器为文档化例外）。

---

# C 容器

## D-C1 第一批 —— 暂定
`Vec[T]`、`String`、`Map[K,V]`。S0/S1 再补 `Arena`（已在 B5）。

## D-C2 映射实现 —— 暂定（有序）
`Map[K,V]` = 有序映射（平衡树或有序数组），要求 `K` 满足 `std::cmp::Ordered[K]` 协议。
理由：确定性（D-F1）+ 无需新增语言协议（`__hash__` 已被 D-E0b 排除）。
键相等用 `Ordered.cmp`（`cmp == Equal` 视为同键），不单独要求 `Eq`。

## D-C3 迭代 —— 已定（沿用语言现状）
- `foreach` 走语言的"可索引序列"内建路径：容器实现 `__len__` / `__index__` 魔法方法（这两个属于**语言内建**，不是 std 协议）。
- `Map` 的遍历经 `entries()` → `[]Entry[K,V]`（按 key 全序），或后续的 `std::iter` 适配器。
- **不引入** `__iter__`/`__next__` 魔法方法（D-E0b）。

## D-C4 复制语义 —— 暂定
容器禁止隐式拷贝；`clone()` 显式深拷贝。

---

# D FFI

## D-D1 边界 —— 已定
仅 `std::sys` 写 `extern`；其余模块只调 `sys`。

## D-D2 字符串过 FFI —— 暂定
`Str` 是 `{ptr,len}`，C 需要 NUL：`sys` 提供 `CStr`（`{ptr}` 视图）类型与 `String.as_cstr(alloc)`（复制并追加 NUL）。不改变 `Str` 表示。

## D-D3 错误 —— 暂定
`sys` 暴露原始返回值 + `sys.errno()`；`std::io` 包成 `?T` 并把 errno 映射为 `tag union Error`。

## D-D4 句柄 —— 暂定
外部不透明句柄包进 `struct`（如 `File { handle: opaque; }`）并实现 `__dispose__`；不对外暴露裸 `opaque` 字段。

## D-D5 回调 —— 暂定
需要时用 `cfunc` 函数类型 + 显式 `env` 参数；S0–S4 不需要。

---

# E 协议（protocols）

## D-E0 协议 = interface + 普通方法 —— 已定
协议**不进入语言规范**，而是 `std` 里的 `interface` 声明；实现者提供**同名方法**即可。
用户原话：*"接口要求传入的类有 xxx 方法，而不是使用魔法方法完成"*。

**协议清单（建议，待你确认方法名）**

| 协议（std 模块） | 方法 | 用途 |
|---|---|---|
| `std::cmp::Eq` | `func eq(self: *Self, other: *Self) -> bool` | 断言、查表 |
| `std::cmp::Ordered` | `func cmp(self: *Self, other: *Self) -> Ordering` | `Map`、排序、诊断排序 |
| `std::hash::Hash` | `func hash(self: *Self) -> u64` | 仅当将来做哈希容器时用 |
| `std::fmt::Display` | `func fmt(self: *Self, w: *fmt.Writer) -> void` | 诊断/生成代码/日志 |
| `std::iter::Iterator` | `func next(self: *Self) -> ?Item` | 惰性适配器（后续） |
| `std::io::Sink` | `func write(self: *Self, data: []u8) -> void` | `fmt.Writer` 的下游 |

**分界（重要）**：魔法方法只用于**语法位置**（运算符、下标、切片、构造、赋值、析构、调用）。
"能做什么事"（相等、排序、打印、哈希、迭代）一律用协议（普通方法）。两条规则合起来：
- `a + b` → `__add__`（语法）；
- `fmt.write(x)` → `Display.fmt`（协议，普通方法调用）；
- `x(call_args)` → `__call__`（语法，见 D-G1）。

## D-E0b 不新增魔法方法 —— 已定
`__fmt__`、`__hash__`、`__iter__` 都不加。格式化/哈希/迭代都走协议。

## D-E1 结构匹配 vs 名义实现 —— **已定 E1.2（已实现）**
[调查] 现状：`interface` 是**名义**的——约束检查要求类型显式 `implements`；
实测：一个方法签名完全匹配但没写 `implements` 的 struct，报
`type argument 'P' does not satisfy the constraint on 'T'`。
- 候选：
  - E1.1 保持名义：std 协议要求用户写 `struct T implements std::fmt::Display { ... }`（多一行声明，但意图明确、诊断清晰）
  - E1.2 改为**结构匹配**：`extends I` 只要类型提供接口要求的全部方法即满足；`implements` 变成可选断言（写了就顺便检查）
- **已定 E1.2**：与你的表述一致（"接口是描述 T 有哪些方法的形状"）。实现：`sema_interface_shape_check`（名字 + 签名逐项比对，失败时点名第一个缺失/签名不符的成员）；`implements` 保留为可选断言，写了仍会逐项校验。
- 影响：白皮书 §6.5 措辞 + C 实现 sema 的约束检查 + 两个实现的诊断。
- 需你拍板：E1.1 还是 E1.2。

---

# F 确定性与诊断

## D-F1 确定性规则 —— 已定（写进 std 契约并被测试）
1. **容器迭代顺序**：`Map` 按 key 全序；`Vec`/`String` 按下标。禁止把哈希桶/分配地址泄漏到可观察输出。
2. **诊断排序**：按 `(模块序号, span.lo, DiagCode)` 排序后输出；同一 `(code, span)` 只保留一条。
3. **格式化**：整型十进制固定；浮点规则写死（精度/舍入/是否指数）；**不使用 locale**。
4. **禁止输出地址**：诊断/生成的 C/golden 里不得出现指针值或分配器地址（现有 C 实现的 UAF 诊断里出现过地址，正是反例）。
5. **golden 自检**：同一输入连续运行两次，输出必须逐字节一致（CI 检查）。

## D-F2 错误与诊断归属 —— 暂定
`?T` 表示失败；`tag union Error` 区分原因；`std::diag`（`Diag` + 文本/JSON 渲染）归 std，编译器与工具共用。

---

# G 语言变更清单（因 std 需求引出）

| 编号 | 变更 | 状态 | 说明 |
|---|---|---|---|
| D-G1 | 新增 `__call__` | 已实现 | 让 struct 可被调用：`x(args)` ⇒ `x.__call__(args)`。签名由实现者定（任意形参 + 任意返回）；派发只在"被调用者类型是带 `__call__` 的 struct"时生效，函数值/闭包路径不变。 |
| D-G2 | interface 结构匹配（E1.2） | 已实现 | 见 D-E1。 |
| D-G3 | ~~`__fmt__` / `__hash__` / `__iter__`~~ | 拒绝 | 走协议。 |

---

# H 待你拍板

1. **D-E1**：interface 结构匹配（E1.2）还是保持名义（E1.1）？
2. **D-C2**：`Map` 用有序映射（我暂定）还是必须做哈希映射（那就需要 `Hash` 协议 + 确定性迭代方案）？
3. **D-B2**：OOM 用"默认 panic + `try_*`"还是所有 API 返回 `?T`？
4. 协议方法名是否按 E 表（`eq` / `cmp` / `fmt` / `hash` / `next` / `write`）？还是你想要 `equals` / `compare` / `format` 这类长名？
5. `__call__` 是否**现在就实现**（含白皮书 §3.15.2 与 conformance 用例），还是与 D-G2 一起做一批？

---

# I Idea 收件箱（按维度归档，原文保留）

## A 模块清单
- （待输入）

## B 内存模型
- 显式传递分配器；组合实现；初始化显式注入依赖。（已归档 → D-B1 / D-A5）

## C 容器
- （待输入）

## D FFI
- （待输入）

## E 协议
- hash/fmt/iterator 属于协议，不是语言规范；接口要求传入类型"有 xxx 方法"，不用魔法方法。（已归档 → D-E0 / D-E0b）

## F 确定性
- 标准库必须确定性。（已归档 → D-F1）
