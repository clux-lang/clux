# clux 自举编译器 · 架构设计（第 0 篇：总纲）

> 状态：设计稿 v0.1（未实现）。本文是**设计文档**，不是规范；规范只有 `docs/whitepaper/`。
> 读者：参与自举的人。目标读者不需要读 `src/` 下任何 C 代码。

## 1. 目标与非目标

**目标**

1. `bootstrap/compiler/` 用 clux 写成，把 clux 源码编译成可执行程序（阶段一仍以 C 作为宿主后端）。
2. `std/` 是唯一的 clux 标准库，编译器只依赖它（FFI 之外不直接碰 libc）。
3. 达到**自举**：`b1` 编译自身源码得到 `b2`，`b2` 编译自身源码得到 `b3`，`b2` 与 `b3` 行为一致。
4. 全程与根 `conformance/` 对照片一致：同一份 `.cx`，C 编译器与自举编译器的**可观察行为**（诊断是否触发、程序 exit code / 输出）一致。

**非目标（本阶段）**

- 不做增量编译缓存、不做 LSP、不做多后端（只做 C 后端）。
- 不追求与 C 实现逐字节相同的 C 输出、相同的 ABI、相同的内部结构。
- 不重写 `std` 之外的生态（包管理器、注册表等）。

## 2. 与现有 C 实现的关系（硬性约束）

| 维度 | 约定 |
|---|---|
| 代码 | **零翻译**。不参考 `src/` 的 C 代码结构、命名、模块划分、算法。 |
| 语义 | 对齐白皮书；白皮书未写的地方以根 `conformance/` 的行为为准，并回填规范。 |
| 内部设计 | 完全按 clux 的语言特性重新设计（tag union、泛型、`?T`、`using`、模块）。 |
| C ABI | 自举编译器**自定义**生成代码的调用约定（不必是 C 实现的 env-first）。只要求语义等价。 |
| 诊断文本 | 不要求逐字一致；要求"同一问题是否报错"与错误类别一致。文本差异由 `std` 的诊断渲染统一。 |

一句话：**自举编译器与 C 实现是两个独立实现，共享语言规范与 conformance 套件。**

## 3. 设计原则

1. **无全局可变状态。** 每个阶段是 `Stage(input) -> Output`，上下文显式传参；阶段间只通过值传递数据。
2. **ADT 优先。** token、AST、类型、诊断、IR 全部用 tag union 表达；不存在"未知 kind → 兜底/静默忽略"的分支，未知即 panic（内部错误）。
3. **ID + arena，不用指针图。** AST/IR 用 `NodeId`/`TypeId`/`SymbolId` 索引，存储由各阶段的 arena 拥有。好处：可序列化、可比较、可缓存、无悬垂。
4. **诊断是数据。** 分析阶段只收集 `Diag`，不打印；由驱动统一渲染（文本/JSON），因此诊断可被测试直接断言。
5. **确定性输出。** 一切映射按 (模块, 名字) 排序遍历；任何输出（诊断、生成的 C、golden）不得依赖 hash 迭代顺序或符号插入顺序。
   （C 实现里"方法按声明顺序 lower 导致先调用后声明坏掉"就是这类问题；设计上直接排除。）
6. **显式 IR 分层。** 嵌套表达式在进入后端前被压平成 MIR（三地址 + 基本块）；后端只做"机械翻译"，不承担语义决策。
7. **语义决策集中在 sema/TIR。** 魔法方法派发、隐式构造、`using`/`defer` 展开、optional 包装、泛型单态化，全部在 TIR 之前定型；MIR 与 C 生成不再"猜"。
8. **std 是唯一平台边界。** 编译器代码里出现 `extern cfunc` 只允许在 `std::sys`。

## 4. 流水线分层与契约

```
source text
  │  lexer
  ▼
Token 流（含 span、字面量解码结果）
  │  parser
  ▼
CST/AST（无类型、无名字解析；节点带 span 与 NodeId）
  │  desugar（语法糖消除：复合赋值、for、三元、隐式块表达式…）
  ▼
AST'（形态收敛，仍无类型）
  │  collect + resolve（模块图、导入、名字解析、可见性）
  ▼
Symbol 图（模块 → 声明；每声明有 SymbolId 与 pre-type）
  │  sema（类型推导/检查、泛型实例化、魔法派发、隐式转换）
  ▼
TIR（有类型、已消歧、已展开生命周期）
  │  lower
  ▼
MIR（基本块 + 三地址 + 显式临时；控制流结构化）
  │  cgen
  ▼
C 源文件（含最小运行时支持）
  │  host cc
  ▼
可执行文件
```

每层的契约（写进各自的子设计文档）：

| 层 | 输入 | 输出 | 不变量 |
|---|---|---|---|
| lexer | 字节流 | `Token[]` | 只做词法；不产生诊断以外的副作用；span 精确到列 |
| parser | `Token[]` | AST 树 | 不查符号表；错误恢复产出"错误节点"而非 NULL |
| desugar | AST | AST' | 语义保持；不引入新名字（除生成名，带标记） |
| resolve | AST' + 模块图 | Symbol 图 | 两遍：先收集声明，再解析引用 |
| sema | Symbol 图 | TIR | 所有隐式行为显式化；泛型已单态化 |
| lower | TIR | MIR | 表达式已压平；生命周期已插入显式 `drop` |
| cgen | MIR | C | 不改变求值顺序；只做命名与类型映射 |

## 5. 数据模型

- **ID 与 arena**：`NodeId(u32)`、`TypeId(u32)`、`SymbolId(u32)`、`Span{file: FileId, lo: u32, hi: u32}`。AST 节点结构里只存 ID 与枚举值，不存跨阶段指针。
- **字符串驻留**：所有标识符/字面量文本进 `Interner`，产物是 `Sym(u32)`；比较用整数。
- **不可变 AST**：阶段只能新建节点，不能就地改旧节点；改写通过"重写表"（`NodeId -> NodeId`）或直接产出新树。这消除 C 实现里"多遍 walk 重复求值/重复诊断"的整类问题。
- **模块**：`Module{ id, name, files, decls, imports }`；模块图由 resolve 一次建好，后续阶段只读。

## 6. 类型系统表示（对照白皮书 §2）

```clux
tag union Type {
    Prim(kind: PrimKind, bits: u16)        // i8..i64/u8..u64/usize/isize/f32/f64/bool/void/opaque/str
    Ptr(to: TypeId, quals: Quals)          // *T / *const T
    Slice(elem: TypeId)                    // []T
    Array(elem: TypeId, n: u64)            // [N]T
    Option(inner: TypeId)                  // ?T
    Tuple(elems: []TypeId)                 // (A, B)
    Func(params: []TypeId, ret: TypeId, env: EnvKind, captures: []Capture)
    Struct(StructRef)                      // 声明 + 实例实参
    Enum(EnumRef)                          // 带底层类型
    Union(UnionRef)                        // tag union
    CUnion(CUnionRef)
    Bitfield(BitfieldRef)
    Interface(InterfaceRef)
    Alias(of: TypeId)                      // 保留名义别名（诊断用），不参与布局
    Param(name: Sym)                       // 泛型形参（只出现在实例化前的类型里）
    Instance(decl: SymbolId, args: []TypeArg) // 单态化实例（唯一化的键）
    Error                                  // 已报错后的占位，禁止进入 TIR
}
```

关键决策：

- **类型 = 值 + 唯一化表**：`TypeId` 由 `TypeTable` 唯一化（结构相等 ⇒ 同一 ID），比较 O(1)。C 实现里"两个等价 struct 实例不同指针"的问题在设计上不存在。
- **实例键**：`Instance` 以 `(decl, args)` 为键登记，递归实例先占位后填充（对照 §6.10）。
- **布局**：`layout(TypeId) -> Layout{size, align}` 单点计算，`sizeof/alignof` 只走这里（依据 §2.12）。

## 7. 诊断模型

```clux
struct Diag {
    severity: Severity        // error / warning / note
    code: DiagCode            // 稳定枚举（可测、可 JSON 化）
    span: Span
    message: Str
    notes: []Diag
}
```

- 阶段返回 `Result[Output, []Diag]` 或"带诊断累加器"的上下文；**没有** `fprintf(stderr)` 式的即时输出。
- 诊断去重规则：同一 `(code, span)` 只保留一条（C 实现里靠"位置 + 前缀比较"打补丁的做法不再需要）。
- 诊断顺序：按 `(fileId, span.lo, code)` 排序后输出，保证确定性。

## 8. 泛型与实例化

- 声明期：泛型体**不求值**，只登记 `GenericDecl{params, body}`。
- 使用期：sema 在调用点/构造点推导 `TypeArg[]`，得到 `Instance` 键；查缓存，未命中则"实例化 AST → 重新走 resolve+sema（限定在该实例上下文）"。
- 实例化是**纯函数**：`instantiate(decl, args) -> InstanceAst`，产物与定义点的可变状态无关。
- 泛型方法：实例键 = `(ownerInstance, decl, methodArgs)`；派发点在 sema 内解决，MIR 只看到具体函数。
- 自举代码自身**尽量不用 comptime**：S1–S4 只需要泛型实例化；comptime 实现放到 S5 之后（此时语言已能编译编译器）。

## 9. 魔法方法派发（对照 §3.15）

- 一张**静态表**描述所有魔法方法：`{name, arity, 形参/返回约束, 派发位置}`；表的来源是白皮书 §3.15.2，而不是 C 实现的 `k_magic[]`。
- 派发只在表达式的**前导操作数**上进行；内建类型（整数/指针/切片/optional/数组）的语义优先，不查用户魔法方法。
- 派发结果在 TIR 里已经是显式调用（`Call{callee, args}`），MIR 不再关心"这是不是魔法"。
- 初始化协议（§3.15.5）的三个钩子各管一段，sema 一次性定型：
  - `.T{args}` → `__init__`（可泛型）；
  - 同类型初始化（var/实参/返回值/字段）→ `__init_copy__`；
  - 赋值 → `__assign__`；
  - 作用域退出 / `dispose(x)` → `__dispose__`。

## 10. 生命周期：`using` / `defer` / `dispose`

- 在 TIR 层把作用域生命周期**完全显式化**：每个需要析构的绑定生成 `Drop{kind: Dispose|Defer, target}`，按逆序插入基本块出口（含 `return`/`break`/`continue` 分支）。
- MIR 里只剩显式 `Drop` 语句；cgen 把它们翻译成调用 + 置空。
- `using a = e;` 的 `e` 只求值一次（TIR 显式引入临时绑定），这是规范要求，用设计保证而不是靠后端小心。

## 11. 模块与包（对照 §8）

- 模块 = 目录/文件单元；`import` 只是名字绑定，不复制代码；`export`/`reexport` 影响可见性表。
- resolve 阶段两遍：① 收集所有模块的顶层声明（含 extern/接口）；② 解析引用（按模块可见性 + 导入链）。
- 循环导入：允许类型级互引（先占位），禁止值级循环初始化（诊断）。
- 包清单（`clux.mod`/`clux.toml`）解析放在驱动层，与编译阶段解耦；依赖只影响模块图构建。

## 12. C 代码生成（自举自己的约定）

- 一个 C 文件族：`<mod>.h`/`<mod>.c`，命名由模块路径 mangling，稳定且可读。
- 函数一律**直接 C 函数**（不使用 C 实现的 env-first 约定）；闭包用显式 `env` 结构体 + 函数指针，只在需要时生成。
- 类型映射：struct/enum/union/bitfield 一比一；`?T` → `{ bool ok; T value; }`；切片 → `{ T *data; usize len; }`。
- 求值顺序：MIR 已把可能影响顺序的表达式拆成临时，cgen 只按语句顺序输出。
- 唯一允许的编译器运行时：`std::rt`（panic、越界检查、分配器接口），实现为普通 clux 模块 + 少量 `extern cfunc`。

## 13. 验证策略

三层对照，缺一不可：

1. **规范对照**：白皮书 §1–§12 每个可执行条款对应 `conformance/` 里的 `test` 块（已有机制）。
2. **行为对照**：同一 `.cx` 分别由 C 编译器与自举编译器构建，比较 exit code 与 stdout（差异即 bug，登记到 conformance）。
3. **自举定点**：`b1 → b2 → b3`，要求 `b2` 与 `b3` 的产物行为一致（不要求字节一致）。

附加：所有生成的 C 必须能被 `-Wall -Wextra` 接受且无 warning（把"生成代码质量"纳入验收）。

## 14. 阶段与验收（代码只是按图索骥）

| 阶段 | 设计产物（先写） | 实现产物 | 验收 |
|---|---|---|---|
| D0 | 本文 + 子设计索引 | — | 设计评审通过 |
| S0 | `01-lexer.md` | `std` 骨架 + `lexer.cx` | token dump 与 oracle 一致（`01-lexical.cx`） |
| S1 | `02-ast.md` | `parser.cx` + AST printer | AST dump 一致（02–06/09–11 语法） |
| S2 | `03-sema.md` | `resolve.cx` + `sema.cx`（子集） | 子集 conformance 编译通过 |
| S3 | `04-mir.md` | `tir.cx` + `lower.cx` | IR 快照测试（golden） |
| S4 | `05-cgen.md` | `cgen.cx` | 行为对照（exit/stdout 一致） |
| S5 | `06-selfhost.md` | 全量覆盖 + 自举脚本 | `b2 = b1(b1)` 通过 |
| S6 | `07-fixpoint.md` | 定点验证 | `b3 = b2(b2)` 行为一致 |

**每个阶段先有设计文档、再有代码**；设计文档里明确"本阶段的输入契约、输出契约、错误契约、测试用例清单"。

## 15. 白皮书章节 → 子系统映射

| 规范 | 主要落在 |
|---|---|
| §1 词法 | lexer |
| §2 类型 | TypeTable / layout / sema |
| §3 声明（含 §3.15 魔法） | resolve / sema / 派发表 |
| §4 语句 | parser / desugar / TIR / lower |
| §5 表达式 | parser / desugar / sema |
| §6 泛型与接口 | 实例化器 / sema |
| §8 模块与 FFI | 模块图 / resolve / 驱动 |
| §9 元数据 | 元数据表（编译期可读的声明信息） |
| §10 位域 | TypeTable / layout / cgen |
| §11 测试 | `test` 块收集器 / 驱动 |
| §12 工具链 | 驱动 / formatter（后续） |

## 16. 待定的设计决策（逐条在子文档里定）

1. **AST 是否分 CST/AST 两套**：倾向只保留 AST（带 span 与生成标记），解析期的"错误节点"也作为 AST 的一种。
2. **desugar 的边界**：`for`/三元/复合赋值/块表达式在哪一层消掉；倾向全部在 desugar，sema 只见规范形态。
3. **TIR 的表达式形态**：是否保留嵌套；倾向保留（便于诊断），压平交给 lower。
4. **comptime 的实现路径**：解释器 vs 编译期执行 IR；倾向先做"常量折叠 + 类型级求值"的最小子集。
5. **`interface` 的派发**：静态约束（单态化）还是运行期 vtable；倾向单态化（§6.9 的精神），运行期留作后续。
6. **std 的最小 API 面**：按 S0 起逐个模块定（`string`/`vec`/`map`/`io`/`fmt`/`args`/`sys`）。
