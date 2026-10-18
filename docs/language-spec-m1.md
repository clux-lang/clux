# clux 语言规格 — M1

> "C, but with LUX" — 文件扩展名 `.cx`

> **阅读提示**：这是 **M1 阶段**的语言规格快照，描述当时已实现的子集与已定的词法/
> 类型/语法规则。其中**词法规则（§2）、基础类型与前导类型语法（§3.1–§3.6）、
> 运算符优先级（§4.6）、TDZ 与 nil 规则**至今有效；而"§7 M1 不支持"清单里的多数
> 条目**已在后续里程碑实现**：
>
> | 规格中的"M1 不支持" | 现状 |
> |----------------------|------|
> | 数组 `[N]T` / 元组 `<T1,T2>` / struct / union / enum / switch / do-while / 三元 / sizeof·alignof·typeof / typedef(type 别名) | M2 已实现（[m2-design.md](m2-design.md)） |
> | 指针与所有权（`own`/`ref`/`fatal`、move/clone、`new`/`.&`/`.*`、`opaque`、RC 系） | M3 已实现（[m3-design.md](m3-design.md)） |
> | 切片 `[]T` 与运行时字符串 | M4 已实现（[m4-design.md](m4-design.md)） |
> | 模块系统 | M5 已实现（[m5-design-v2.md](m5-design-v2.md)） |
> | 错误处理机制 | 未实现（M8） |
> | 预编译指令、goto、逗号运算符 | 仍未实现（设计上不做） |
>
> 另外两处 M1 特有约定已被后续里程碑**取代**：`const`/`volatile` 在 M1 "词法识别但
> 语义不生效"，M2 起是**真实类型**（§3.3 语义已生效）；`nil` 作为 func/str 的 0 初始化
> 标记已在 M2 被 **optional `?T`** 取代（运行时不再有 nil value）。
>
> 各里程碑的最终行为以对应设计文档 + 代码为准；完成状态见 [milestones.md](milestones.md)。

## 1. 概述

clux 是一门极简类 C 语言，无 OOP，纯静态类型，布局兼容鸭子类型，编译期静态所有权保证指针内存安全。

M1 目标：实现"裁剪后的 C"子集的解释器。移除预编译指令，类型系统改为 clux 模式，不支持指针和所有权。

## 2. 词法结构

### 2.1 关键字

```
func    var     return  if      else    while   for     as
break   continue true    false   void    bool    str     undefined
const   volatile
i8      i16     i32     i64     u8      u16     u32     u64
f32     f64     usize
nil     comptime extends
```

**`usize` = C 的 `size_t`（随架构变化）**：它不是独立类型，而是平台无符号整数类型的
**别名**（与 C 的 `typedef` 同义）——64 位平台就是 `u64`，32 位平台就是 `u32`
（判据是指针宽度，见 `vm_type_usize`）。因此 `usize` 与底层类型可互相赋值/比较，
`usize` 与 `u64` 之间没有隐式转换障碍；`len(...)` / `sizeof(...)` / `alignof(...)`
返回 `usize`，字面量后缀 `5usize` 也按平台宽度归一化。写可移植代码（以及 FFI 里
声明 C 的 `size_t` 形参）应当用 `usize` 而不是写死 `u64`。

### 2.2 运算符与标点

```
+  -  *  /  %             算术
== != <  >  <= >=          比较
&& || !                    逻辑
&  |  ^  ~  << >>          位运算
=  += -= *= /= %=          赋值
(  )  {  }  [  ]  <  >  ;  ,  :  .  标点
```

符号采用 **maximal munch**（最长匹配），最长 2 个字符：例如 `<<<` 切分为 `<<` 与 `<`。

`.` 是合法符号，但 M1 语法不使用它（无成员访问、无 `.5` 形式的浮点）：浮点字面量中的 `.` 由 2.3 的数字规则消费，其余位置出现的 `.` 由 Parser 报语法错误。

### 2.3 字面量

- 整数：`42`, `0xFF`, `0o77`, `0b1010`
- 整数（带类型后缀）：`1i8`, `100i32`, `42u64`, `0xFFu8`
- 浮点：`3.14`, `1.0e10`
- 浮点（带类型后缀）：`3.14f32`, `1.0e10f64`
- 字符：`'a'`, `'\n'`, `'\x41'`（值类型 u8，见 2.6）
- 布尔：`true`, `false`
- 字符串：`"hello"`（见 3.2）
- nil：`nil`（内置类型唯一值，见 4.2.1）

#### 数字字面量词法规则

- 进制前缀：`0x`/`0X`（十六进制）、`0o`/`0O`（八进制）、`0b`/`0B`（二进制）；进制字面量必须是整数形式，不能带小数点或指数
- 十进制浮点：`<int>.<frac>` 或带指数 `e[±]<digits>`；小数点和指数后必须有数字
- 类型后缀：整数形式只允许 `i8 i16 i32 i64 u8 u16 u32 u64 usize`（`usize` 按平台宽度归一化成 u64/u32），浮点形式只允许 `f32 f64`；后缀必须是上述合法集合，`1i8x`、`1f32`（整数形式带浮点后缀）均为**词法错误**
- 数字后不允许紧跟非后缀字母/下划线序列

### 2.4 注释

- 行注释：`//`（到最近的 CR 或 LF 为止，**不含**换行符本身；CRLF 源文件中 `\r\n` 整体属于空白，不被注释吞掉）
- 块注释：`/* */`（**不嵌套**，遇到第一个 `*/` 即结束——与 C 语言一致）
- 未闭合的块注释是词法错误

### 2.5 标识符

标识符遵循 Unicode 标识符规范（UAX #31 的 ID_Start / ID_Continue 属性，实现使用 ICU 的 `u_isIDStart` 与 `u_isIDPart`）：

- 首字符：具备 **ID_Start** 属性的字符，或下划线 `_`（`_` 在 Unicode 中只属于 ID_Continue，此处作为特例允许开头）
- 后续字符：具备 **ID_Continue** 属性的字符
- 因此 `变量`、`café`、`_ü`、`a1_π` 都是合法标识符
- 关键字按 2.1 的 ASCII 表识别，Unicode 标识符不会与关键字冲突
- 不具备 ID_Start 的字符（如 `€`、`©`、emoji）是**词法错误**，见 2.7

**保留标识符（2026-10）**

编译器大量使用 `__` 前缀的内部名字（生成的 `__destroy_<n>` / `__clone_<n>` /
`__cloneptr_<n>` / `__cloneslice_<n>`、闭包上下文 `__ctx`、clone 兜底
`__deep_clone`、切片临时 `__cl_<n>` 等），因此把该名字空间保留出来：

- 以 **`__` 开头**的标识符**保留**给编译器生成名与**未来的魔法方法**
  （`__init__` 一类）——用户代码不得声明**或引用**，诊断：
  `invalid <what> '__x': identifiers starting with '__' are reserved for
  compiler-generated names (and future magic methods)`
- **裸 `_`** 是丢弃标记（`_ = expr`），不能作为标识符（变量 / 参数 / 字段 /
  类型名 / 捕获名 …），诊断：`'_' is the discard marker and cannot be used as
  an identifier`
- 名字**中间**含 `__`（`a__b`）与**单下划线开头**（`_tmp`）合法

该规则在所有声明位置（变量 / 参数 / 捕获 / 函数 / 类型 / `struct` / `union` /
`cunion` / `enum` / 字段 / import 别名）与表达式引用位置统一执行。

### 2.5.1 源文件编码与 BOM

- 源文件必须是 **UTF-8**
- 文件开头的 UTF-8 BOM（`EF BB BF`）被跳过，不属于任何 token；BOM 之后的第一个字符位于第 1 行第 1 列
- 出现在其他位置的 U+FEFF 不是空白，按"无法识别字符"处理
- 换行支持 LF 与 CRLF；单独的 CR 也是空白，并且会结束行注释（见 2.4）

### 2.6 字符字面量（C 风格）

```
'a'       // 单字符
'\n'      // 转义序列
'\x41'    // 十六进制转义（值 0x41 = 'A'）
```

- 值类型为 `u8`
- 内容必须是**恰好一个字符**（原始字符或单个转义序列）；`''` 空字符、`'ab'` 多字符为编译错误
- 支持的转义序列与字符串一致：

| 转义 | 含义 |
|------|------|
| `\n` `\t` `\r` | 换行 / 制表 / 回车 |
| `\\` `\'` `\"` | 反斜杠 / 单引号 / 双引号 |
| `\0` | NUL |
| `\xHH` | 十六进制字节（1-2 位 hex 数字） |

- 未闭合的字符字面量（EOF 或裸换行）是词法错误
- 空字面量 `''`、多字符字面量 `'ab'` / `'\01'`、未知转义（`'\q'`）、`\x` 后不是十六进制数字，均为**词法错误**
- 超过 U+00FF 的字符（如 `'中'`）无法用 u8 表示，是**词法错误**

### 2.7 词法错误（fail-fast）

词法错误**不可恢复**：一旦发生，立即终止整个编译流程，输出诊断，退出码 1。不尝试跳过或继续，`_` 等恢复机制只存在于 Parser 的语法错误层面。

词法错误的完整情形：

- 无法识别的字符（含不具备 ID_Start 属性的 Unicode 字符、非法 UTF-8 产生的 U+FFFD）
- 非法数字后缀、进制前缀后无数字、小数点或指数后无数字
- 未闭合的字符串 / 字符 / 块注释，或其中的裸换行
- 空字符字面量 `''`、多字符字面量 `'ab'`
- 未知转义序列（`"\q"`、`'\u0041'`），`\x` 后缺少十六进制数字
- 无法用 u8 表示的字符字面量（> U+00FF）

## 3. 类型系统

### 3.1 基础类型

| 类型   | 大小(字节) | 说明         |
|--------|-----------|-------------|
| i8     | 1         | 有符号 8 位整数 |
| i16    | 2         | 有符号 16 位整数 |
| i32    | 4         | 有符号 32 位整数 |
| i64    | 8         | 有符号 64 位整数 |
| u8     | 1         | 无符号 8 位整数 |
| u16    | 2         | 无符号 16 位整数 |
| u32    | 4         | 无符号 32 位整数 |
| u64    | 8         | 无符号 64 位整数 |
| usize  | 指针宽度  | C 的 `size_t`：**别名**（64 位 = u64 / 32 位 = u32） |
| f32    | 4         | 32 位浮点数     |
| f64    | 8         | 64 位浮点数     |
| bool   | 1         | 布尔类型        |
| str    | —         | 编译期字符串常量 |
| void   | 0         | 无返回值        |

### 3.2 统一前导类型语法

clux 采用统一的前导类型修饰符语法，类型修饰符始终在基底类型前面，从左向右阅读：

| 语法      | 含义              | 示例               |
|-----------|-------------------|--------------------|
| `[N]T`    | 长度为 N 的数组    | `[10]i32`          |
| `[]T`     | 切片              | `[]u8`             |
| `*T`      | 指针              | `*i32`             |
| `<T1,T2>` | 元组              | `<i32, f64>`       |

组合示例：
```
*[10]i32       // 指针，指向长度为10的i32数组
[]*i32         // 切片，元素为i32指针
*[]i32         // 指针，指向i32切片
<const i32>    // 元组，含一个const i32（单元素元组退化为该类型）
<[10]i32, *u8> // 元组，含一个i32数组和u8指针
```

**非指针值语义**：clux 中所有非指针类型均采用复制语义，不存在数组退化为指针的操作。`[10]i32` 作为函数参数传递时复制整个数组，不会退化为 `*i32`。

> M1 不支持数组、切片、指针、元组，但语法规则先行声明。

### 3.3 const / volatile 修饰符

`const` 和 `volatile` 直接修饰紧随其后的类型，消除 C 语言中 const 修饰指针还是修饰指向对象的歧义：

```
const i32          // 不可变的 i32 值
const *i32         // 不可变的 i32 指针（指针本身不可变，指向的值可变）
*const i32         // 指向不可变 i32 的指针（指针可变，指向的值不可变）
const *const i32   // 不可变的指针，指向不可变的 i32（两者都不可变）
volatile i32       // 易变的 i32 值
volatile *i32      // 易变的 i32 指针
```

阅读规则：const/volatile 修饰紧随其后的整个类型构造。
- `const *i32`：const 修饰 `*i32`（指针），指针不可变，指向的 i32 可变
- `*const i32`：const 修饰 `i32`（指向的值），i32 不可变，指针本身可变

与 C 的对比：
```
// C 语言（容易混淆，从右向左读）
const int *p;       // p 是指针，指向 const int
int *const p;       // p 是 const 指针，指向 int
const int *const p; // p 是 const 指针，指向 const int

// clux（从左向右，无歧义）
const *i32 p;       // const 修饰指针 → 指针不可变，指向的 i32 可变（等同 C: int *const p）
*const i32 p;       // const 修饰 i32 → 指针可变，指向的 i32 不可变（等同 C: const int *p）
const *const i32 p; // 第一个 const 修饰指针，第二个 const 修饰 i32 → 两者都不可变
```

**结构体指针向上自动转换（2026-10 起）**：`大结构体* → 小结构体*` 只要「小结构体的
全部字段恰是大结构体字段表的前缀（名字 + 类型逐一相等）」就**隐式成立**——C 布局自左
向右分配，前缀字段的偏移与类型完全一致，同一地址按小结构体解释是安全的（即 C 的
"公共起始序列"规则）。允许 `ref *Big → ref *Small` 与 `own *Big → ref *Small`
（借用目标）；`ref *Big → ref *const Small` 同时加限定也合法。**不允许**：反向
（小 → 大，多出的字段没有存储）、前缀上名字/类型不匹配、**值**转换（值必须类型完全
相同，否则静默丢掉尾部字段）、以及丢限定（`ref *const T → ref *T` 仍须显式 `as`）。
own/share **目标**的向上转换也不隐式允许：按小结构体释放会漏掉尾部字段的 own 资源。
用途：把"派生"结构体的指针直接喂给按"基"结构体声明的接收者/参数，例如
`Base::getId(d.&)`（`d: Derived`，`Base` 是 `Derived` 的前缀）。

**结构体嵌入 / 组合（`...Base;`，2026-10 起）**：结构体体内可以写一条 `...Base;`，
把另一个结构体的字段**嵌进**本结构体：

```
struct Base {
    id: i32;
    var count: i32 = 0;
    func getId(self: ref *const Base): i32 { return self.id; }
}

struct Test { ...Base; extra: i64; }   // Test 的字段 = id, extra
```

- **语法与位置**：`...Name;` 可出现在字段之间的任意位置；它出现在**自己第几个字段
  之前**决定组合后的字段序（`struct M { x: i32; ...Base; y: i32; }` 的字段是
  `x, id, y`）。同一个结构体体内可以写多条 `...Name;`（按书写顺序依次插入）。
- **位置决定前缀兼容（不需要任何特判）**：组合后的字段表就是"手写同样字段"的字段表，
  于是既有的**结构体指针向上自动转换**规则自然生效——`...Base` 在**最前面** ⟹ Test
  的字段前缀 == Base 的字段 ⟹ `var b: ref *const Base = t.&;` 与 `t.getId()` 都成立；
  前面还有字段 ⟹ 前缀不成立 ⟹ 同一写法**自然编译失败**
  （`cannot convert ref Test to ref const Base`）。
- **attribute 覆盖（新 struct 可以重写继承来的 attribute）**：**attribute** 指结构体命名
  空间的成员 —— **字段** + `var` 静态属性 + `func` 方法。组合后的新 struct（`Test`）
  **自己声明**的同名 attribute **静默覆盖**从 `Base` 继承来的那一份：合法、不报歧义、
  不报重复。细口径：
  - **成员（`var` / `func`）**：`Test` 声明了 `var count`（或 `func f`）时，
    `Test::count` / `t.f()` 一律解析到 **Test 自己的那一份**；覆盖后二者**互相独立**
    （`Test::count` 与 `Base::count` 是两个独立的全局变量，写 `Test::count = 9` **不**
    影响 `Base::count`）；`Test` 自己**没有**声明时才继承——此时静态属性是**同一份**
    全局变量（`Test::count` 与 `Base::count` 读写互通、互为别名）。
  - **kind 组合按"名字优先、自己的胜出"裁决**：`Test` 用 `func count` 覆盖 `Base` 的
    `var count`（或反向）时以**自己**为准，**不会**静默回退到 `Base` 的那一份。若使用点
    的写法与自己的 kind 不符，就在使用点报错：自己的是 `var` 却写 `t.f()` →
    `cannot call 'f': it is a struct member variable, not a method`；自己的是 `func`
    却写 `Test::f = v` → `cannot assign to 'Test::f': it is a func member (a method,
    not a variable)`。这样"覆盖"永远是确定的（不会出现"看情况用谁的"）。
  - **字段是唯一例外**：字段**不允许覆盖**——自有字段与继承字段同名、两个嵌入字段同名
    一律**报错**（见下一条）。
- **成员继承（方法 / 静态属性不复制）**：`Test` **自己没声明**该名字时，才通过嵌入链
  **继承名字查找**：`t.getId()`、`Test::getId(t.&)`、`Test::count`（读写）都解析到
  `Base::...` 的同一份存储。查找顺序是确定性的：先 `Test::name` 自己（命中即**自己的
  胜出**，不再往下找），再按 `...` 的书写顺序**深度优先**（因此传递嵌入天然覆盖）、
  按类型去重（同一个 `Base` 经两条路径只算一次命中）；**恰好命中一个**才可用，命中
  多个 → 报歧义（`member 'x' of struct 'R' is ambiguous: found in P, Q`，**只在**
  "自己没有、而多个嵌入都提供同名 attribute"时出现），零命中 → 沿用普通"没有这个
  成员"的诊断。
- **字段不允许覆盖**：嵌入字段与自有字段重名、两条嵌入路径带来同名字段（含菱形里共享
  的 Base 实例字段），都**报错**（`field 'id' conflicts with embedded field from 'Base'
  (fields cannot be overridden by struct embedding)`
  / `duplicate field name 'v' after struct embedding: inherited from both 'P' and 'Q'`）。
  字段不参与"覆盖"语义——它是 attribute 里唯一不可被重写的成员。
- **seal 前可编辑 attribute 的不变量**：类型在 **seal 之前是可编辑的**（attribute 可以
  被改写、覆盖），**seal 之后不可变**——结构体组合与未来的装饰器都必须在这个窗口里完成。
  这条规则在实现上是硬的：VM 层的类型写入 API（`type_struct_insert_embed_fields` /
  `type_struct_set_embeds` / `type_struct_mark_unique`）对已 seal 的类型**拒绝**（返回
  失败、类型一字不改），与 `type_struct_add_field` 的"密封后静默忽略"同一口径。组合
  本身因此也发生在 seal 之前（开放类型上装好字段表 + 嵌入表，然后只 seal 一次）。
  注：clux 目前**没有** attribute / 装饰器语法，规则以不变量 + 文档 + 测试固化。
- **具名 struct 的身份不合并**：命名的 `struct A` 与 `struct B` 即使字段表逐位相同也是
  两个**不同**类型（各自有独立的成员命名空间 `A::f` / `B::f` 与嵌入表），类型构造时
  **不**去做重合并。结构兼容（鸭子类型）与指针前缀向上转换仍按字段表判定，不受影响。
- **定义顺序与环**：`...Base` 的 `Base` 声明在本文件 `Test` **之后**也可以（sema 按
  嵌入依赖顺序反复尝试到不动点）；循环嵌入（`struct X { ...X; }`、`X→Y→X`、更长的环）
  一律报 `circular struct embedding ('...' cycle involving this struct)`。
- **限制**：`...Name` 目前只支持**同一编译单元内**声明的结构体（跨 `import` 的结构体
  报"no struct type with that name in this compilation unit"）。

**递归结构体（自引用 / 互相引用，2026-10 起支持）**：字段的类型依赖图**可以成环**，只要
环**经过指针 / 切片 / 函数签名 / 限定符 / 数组**——这些包装的布局不依赖被引用类型的完整
布局（指针宽与切片头恒定；`const`/`volatile`/数组在 sub 定型后由 VM 侧同步布局）：

```
struct Node { v: i32; next: ?ref *const Node; }               // 自引用（链表/树）
struct A { b: ?ref *const B; }
struct B { a: ?ref *const A; }                                // 互相引用
struct Shape { area: func(ref *const Shape) -> i32; r: i32; } // 签名点名自身
```

- **允许**：`own */ref */fatal */share */weak *T`、切片（`own []T` / `ref []T` / `fatal []T`，
  切片头布局恒定）、函数签名里的 `T`（`func(ref *const T) -> R` 这种自引用签名也行）、
  `const T` / `volatile T`、数组 `[N]T`；
- **拒绝（按值递归 = 无限大小）**：`struct A { a: A; }`、`struct A { xs: [2]A; }`、
  `?A`（option 按值内联元素布局，没有延迟定型路径）→
  `error: struct 'A': field 'a' has incomplete type 'A': recursive struct by value
  (break the cycle with a pointer, e.g. 'ref *A')`；
- **仍然不需要前置声明**：定义顺序无关（字段可以点名**后面**才声明的结构体）；
- 只支持**同一编译单元**内声明的结构体（跨 `import` 与 `...Name` 同一限制）。

**不做隐式虚表：静态分派 + 用户侧手写虚表（2026-10 定稿，非目标）**：clux **不提供**
隐式（编译器隐藏的）虚表，**不做**自动动态分派。这是**设计原则**，不是"尚未实现"：

- **方法调用是编译期静态绑定**：`t.f()` ≡ `typeof(t)::f(t.&)` 在编译期解析到**确定的
  那一个**实现；对象布局里**没有**隐藏的 vptr/vtable 槽，调用点**不查表**，运行期无
  隐藏成本。这与结构体嵌入给出的静态子类型关系自洽：`...Base` 只是重排字段表，
  不引入任何运行期类型信息。
- **需要虚表语义时，用户自己显式实现**：把**函数类型字段**当槽位，自己填表、自己
  查表调用（形如 `var slot: func(opaque) -> i32 = impl;` … `obj.slot(obj.&)`）。
  可运行示例：[examples/manual_vtable/manual_vtable.cx](../examples/manual_vtable/manual_vtable.cx)
  （`...Shape` 公共前缀 + 每"类型"一张全局函数表 + 统一入口遍历异构集合）；
  设计说明见 [m6-design.md §14.24](m6-design.md)。
- **别与 `vtable_t` 混淆**：`include/vm/vtable.h` 的 `vtable_t` 是**类型自身的运算行为
  表**（clone / dispose / assign / eq / get_index / … 的 VM 内部函数指针表），属于类型
  系统的实现细节，与"对象多态 / 虚方法"无关。
- **用户侧模式**：表行的槽位用**函数类型字段**表达，接收者经 `opaque`（≈ C 的 `void*`）
  传入、实现里 `as` 还原具体类型——`opaque` 在这里的作用是**接收者下转型**（指针 → 指针
  的显式窄化不支持，`opaque → 指针` 才支持），**不是**为了绕开类型递归（见下条）。
- **递归结构体已支持**（2026-10，见 §3.3「递归结构体」）：自引用（`next: ref *const Node`）
  与互相引用都合法；唯一被拒的是**按值递归**（`struct A { a: A; }`、`[N]A`、`?A` 等按值
  内联自身的形态，必然无限大小），报 `has incomplete type ... recursive struct by value`。
  因此"对象持表指针 + 表行签名点名对象类型"（`area: func(ref *const Shape) -> i32`）
  也能直接写（示例为了下转型仍用 `opaque`）。

**`volatile` 在脚本态被完全忽略（2026-10 起）**：`volatile T` 一律折叠为 `T` ——
类型解析（sema）、类型构造（compiler）、CTFE 类型值求值三处都直接取 sub，因此
volatile 不参与任何类型检查：读写、运算、复合赋值、与 `T` 双向赋值、传参、比较、
取字段、作指针 base 全部放行；`const volatile T` / `volatile const T` 只保留
`const` 语义。理由：脚本/VM 全程由运行时持有数据、没有编译器的重排序优化，
`volatile` 的语义没有承载对象。AST 与 fmt 保持不变（源码里的 `volatile` 仍原样
打印，不会静默丢关键字）；VM 类型系统内部的 volatile 类型（`type_volatile_intern`）
仍保留为 API，只是源码不再产生它。
**语义（已实现）**：const/volatile 是真实类型（intern 池管理，`const i32 != i32`），可任意顺序重复修饰并位或合并（`const volatile i32`、`volatile const i32` 等价，固定组合顺序 `volatile(const(T))`）：

- **const**：声明后不可再作为赋值左值。`var x:const T = undefined` 声明后**首次赋值豁免**（TDZ 赋值 = 初始化，仅一次），此后赋值/复合赋值编译报错。读取与参与运算不受限。
- **volatile**：存取语义提示（禁止优化器重排/合并读写），不改变类型身份。读写、赋值、运算全部合法，vtable 代理子类型功能。
- **解包代理**：const/volatile 类型的全部运算（算术/比较/位运算/赋值/call/cast）解包到子类型 vtable 执行，值布局（size/align）与子类型一致。
- **复制语义**：`const T` 值可复制到 `T` 变量（`const T extends T` 身份拷贝）；类型身份判断（`type_equal`）保持严格（`const i32 == i32` 为 false）。

示例：
```
var a:const i32 = undefined;
a = 123;                // 首次赋值 = 初始化，合法
// a = 456;             // 再赋值非法（编译错误）

var b:volatile i32 = 1;
b = b + 1;              // volatile 读写全合法

var c:const volatile i32 = undefined;
c = 7;                  // 组合限定，首次赋值豁免
```

### 3.2 str 类型

`str` 是编译期字符串常量类型，具有以下特性：

- 只能通过字符串字面量 `"..."` 创建
- 不可修改，不可赋值为其他类型
- 运行时不拥有内存，编译期确定内容
- 主要用途：printf 的格式化字符串参数
- 不等同于字符数组或字符指针（M1 无这些类型）

```
var s = "hello";    // 推断为 str
var msg:str = "hi"; // 显式标注
```

字符串支持与字符字面量相同的转义序列（`\n` `\t` `\r` `\\` `\"` `\'` `\0` `\xHH`）；字符串内不允许裸换行，转义解码发生在编译期。

### 3.3 鸭子类型（布局兼容）

两个类型内存布局兼容（size + align + 内部结构一致）即视作同一类型。M1 仅涉及原始类型：kind 相同即兼容。

### 3.4 类型推断

`var` 关键字声明变量时，类型由初始化表达式推断：

```
var x = 10;       // 推断为 i32
var y = 3.14;     // 推断为 f64
var z = true;     // 推断为 bool
```

整数推断规则：字面量推断为 i32；浮点推断为 f64。

### 3.5 显式类型转换

```
<expr> as <type>
```

- 使用 `as` 关键字进行显式类型转换
- 不允许 C 风格的 `(<type>)<expr>` 语法

允许的转换：
- 整数之间：`var x:i32 = 42; var y:u8 = x as u8;`
- 整数与浮点之间：`var f:f64 = 10 as f64;`
- 整数与 bool 之间：`var b:bool = 1 as bool;`、`var n:i32 = true as i32;`

禁止的转换：
- str 与其他类型之间

### 3.6 禁止隐式跨类型转换

clux 不允许任何跨类型族的隐式转换：

- **integer <-> bool**：禁止隐式转换，可通过 `as` 显式转换
  - `if 1 {}` 编译错误，应写 `if 1 as bool {}` 或 `if 1 != 0 {}`
- **integer <-> pointer**：禁止（M1 无指针，但规则先行）
- **bool 与整数混合运算**：禁止隐式，`var x = true + 1;` 编译错误
- **整数与浮点混合运算**：禁止隐式转换，必须用 `as` 显式转换。`var x = 1 + 1.0;` 编译错误

允许的隐式行为：
- 同类型赋值：始终允许
- 布局兼容类型赋值：允许（鸭子类型）
- 整数提升：仅在同一运算的两个整数操作数之间（i8 + i32 → i32）

## 4. 语法规则

### 4.1 函数定义

```
func <name>(<params>):<return_type> {
    <body>
}
```

- 参数风格：`name:type`，逗号分隔
- 返回类型：`:type`，无返回值可省略或写 `:void`
- 函数体：块语句
- 每个程序必须有 `main` 函数作为入口

#### 4.1.1 函数字面量（表达式内函数值）

函数定义可作为表达式（函数字面量）出现，产生函数值：

```
func(<params>):<return_type> { <body> }      // 匿名
func <name>(<params>):<return_type> { ... }  // 具名（名字仅作显示名）
```

规则：
- 字面量可在任意表达式位置使用：`var f = func(x:i32):i32 { return x + 1; };`、
  高阶调用实参 `apply(func(...){...}, 1, 2)`
- 具名字面量的名字**不绑定作用域**——外部不可按名引用，仅作显示名
- 字面量 body 可访问参数、全局函数，以及**捕获列表**声明的外层局部变量（见 4.1.2）
- 字面量 body 内可定义局部函数并调用
- 与函数名作为值（`var f = add;`）等价：函数值类型即签名类型 `func(...)->...`，
  编译期统一按 fid 预扫描收集、程序头 hoist 区注册（构造**基底**对象，仅绑定地址），
  定义点（局部函数/字面量）`MAKE_FUNCTION <fid>` 按基底实例化独立新实例
- 函数定义每次求值生成**独立新实例**（共享基底 entry_pc/id/type/name，独立
  `closure_scope`）——循环内多个闭包捕获互不干扰；hoist 区基底对全局函数即最终实例

#### 4.1.2 闭包（捕获列表）

闭包语法：捕获列表紧跟 `func` 之后，以 `|` 左右分隔：

```
func |a, (b: i32 = c + d)| <name>(<params>):<return_type> { <body> }   // 语句级局部函数 / 全局函数
var f = func |a, (b: i32 = c + d)| add(x: i32): i32 { return a + b + x; };  // 函数字面量
```

捕获列表项两种形态：

- **纯 id 捕获**：`a` —— 捕获外层作用域变量 `a`（要求外层已定义且已初始化）
- **括号 VALUE DECL**：`(b: i32 = c + d)` —— 定义点在外层作用域求值构造临时捕获值
  （类型标注可选、初始值必选）

规则：

- 空捕获列表 `||` 合法（等价无捕获函数）
- 捕获是 **clone 值而非引用**：闭包创建后修改外层变量不影响闭包内捕获副本
- **闭包不参与类型**：函数签名擦除捕获列表——`func |x| ...: func(i32)->i32` 与
  无捕获的 `func(i32)->i32` 是同一类型；函数类型表达式（`func(...)->T`）带捕获列表报错
- 函数提升：捕获在**定义点**绑定（`MAKE_FUNCTION` + `SET_CLOSURE`）；
  提升后、定义点前引用/调用有捕获的局部函数 → **编译期 TDZ 报错**
  （sema 检查捕获符号未激活即报 "used before its captures are bound"，
  不静默到运行期——捕获槽运行期仍是 `undefined` 占位）；无捕获的局部
  函数提升后即可安全引用（hoist 只绑定地址，无捕获槽）
- 全局函数 / comptime 函数不允许捕获列表
- 未在捕获列表声明的外层局部变量，body 内访问仍编译期拒绝
- 函数体内读取捕获：查函数局部作用域链 → `closure_scope` → 全局

#### 4.1.3 defer（延迟执行块）

`defer` 是"在作用域退出时延迟执行的块"，语义上等价于在当前作用域里创建一个
**带闭包的无参 void 函数值**并把它压进该作用域的 **defer 栈**：

```
defer ['<a, b>] [|cap, (name: T = init)|] { <body> }
```

| 形态 | 含义 |
|------|------|
| `defer { ... }` | 无捕获（等价 `defer \|\|`） |
| `defer \|\| { ... }` / `defer \| \| { ... }` | 显式空捕获列表（与省略写法 AST 相同，`fmt` 统一渲染为省略形态） |
| `defer \|a, (b: i32 = c + d)\| { ... }` | 捕获列表：与闭包 `func \|...\|` **同一套**解析与语义（见 4.1.2） |
| `defer '<x> \|r\| { ... }` | 作用域标注：捕获 `ref`/`opaque` 时**必须**给出（与闭包同规则） |

规则：

- **捕获是 clone 值而非引用**（与闭包逐字一致）：`defer |a| { ... }` 捕获的是
  **登记时**外层变量 `a` 的值；登记之后再修改 `a` 不影响 defer 体内读到的副本。
  因此 defer 体内**不能**直接改写外层局部变量（要共享状态请用全局变量，或捕获
  `ref` 通过指针改内存）。
- 纯 id 捕获要求外层变量已定义且**已初始化**；`own`/`fatal` 裸捕获被拒（须
  `move()`/`clone()` 显式接管，如 `defer |(q: own *i32 = move(p))| { ... }`）；
  捕获 `ref`/`opaque` 必须带作用域标注（诊断与闭包同一条）。
- **执行时机**：作用域退出（`pop_scope`）时，在**内存释放之前**（`own`/`fatal` 的
  dispose、scope 销毁之前）把 defer 栈**逆序（LIFO）**弹出来逐个调用。因此
  defer 体内可以正常读到本作用域的一切，包括即将释放的 `own`/`fatal` 变量与其
  指向的堆块。
- **所有正常离开作用域的路径都执行 defer**：落到块尾、`return`（当前作用域 +
  所有被弹出作用域，内层先于外层）、`break`/`continue` 跳出该作用域、`switch`
  分支体结束、以及循环体每次迭代结束（循环内每次迭代各执行一次，迭代内 LIFO）。
  唯一不执行的是**错误/panic 值向上传播**的展开路径（引擎在错误展开时走
  `scope_destroy_subtree` 砍掉整棵子树，刻意不触发 defer；见
  `docs/m6-design.md` §14.26 与 `include/vm/scope.h` 的说明）。
- **块体就是函数体**（无参、返回 `void`）：
  - 可以引用捕获的变量、调用函数、读写字段/下标，与普通函数体同一套语义；
  - 非 void 的表达式语句须显式丢弃：`defer { _ = f(); }`（与函数体内
    "结果未使用"同一条规则）；
  - `return;` 只从该 defer 块体返回（`return expr;` 报
    `void function cannot return a value`）；
  - `break`/`continue` **不能**跨出 defer（它是另一个函数体，报
    `'break' outside loop`）；
  - 未捕获的外层局部变量在块体内不可见（与闭包同一条诊断
    "local function cannot access outer local ... (add it to the capture list)"）。
- defer 自身的返回值（含 error/panic 值）**不传给任何消费点**：作用域正在销毁，
  没有接收方。error 值（引擎级失败）会以 `defer: <消息>` 打到 stderr，便于定位
  defer 体半途失败，但不改变控制流与退出码。
- `defer` 只能出现在**函数体内**（语句位置）；顶层 `defer` 报
  `expected function definition at top level`。
- `comptime` 求值（CTFE）内的 `defer` 明确拒绝：
  `ctfe: 'defer' is not supported in comptime evaluation`（CTFE 没有"作用域退出
  钩子"这一层，静默丢弃会让 comptime 与运行期语义分叉）。

示例：

```c
func main(): i32 {
    var a: i32 = 7;
    {
        defer |a| { libc::printf("a=%d\n", a); }   // 读到登记时的 7
        defer { libc::printf("first\n"); }         // 后登记 → 先执行
        a = 1000;                                  // 不影响已捕获的副本
    }
    return 0;                                      // 输出顺序：first → a=7
}
```

### 4.2 变量声明

```
var <name>:<type> = <expr>;         // 显式类型标注 + 初始化
var <name> = <expr>;                // 类型推断 + 初始化
var <name>:<type> = undefined;      // 延迟初始化（TDZ）
```

规则：
- 每条 `var` 只能声明一个变量，不允许逗号连续声明
- 必须有初始化语句，不支持 `var x:i32;` 这种无初始化的声明
- 延迟初始化用 `undefined` 关键字，此时必须标注类型

#### TDZ（暂时性死区）

`var x:i32 = undefined;` 将变量置入 TDZ：

- 内存已分配，但未初始化
- 非赋值访问（读取）报运行时错误
- 可以赋值：`x = 10;` 赋值后退出 TDZ
- 可计算 `typeof(x)`、`sizeof(x)`、`alignof(x)`（仅基于类型信息，不访问值）

#### nil（内置类型唯一值）

> **临时实现**：nil 是 M1 的临时空值表示，后续 M2+ 将由 **optional 类型**（`?T`）替代——当前 nil 同时充当 func/str 的"0 初始化"标记与比较探测，仅覆盖基本空指针语义，不携带类型信息（optional 将把"空"与类型绑定）。

`nil` 是内置类型（与 `type` 类似）的唯一值，当前表示函数与字符串的 **0 初始化**（`*((func_t **)data) = NULL` / `*((string_t **)data) = NULL`），未来用于表示空指针。

规则：
- `nil` 是字面量值，**不是类型名**：不存在 `var a:nil = nil` 这类类型变量定义（`type_lookup("nil")` 返回 NULL）
- 函数 0 初始化：`var f:func(...) = nil` / `f = nil`（nil → func 隐式转换）
- 字符串 0 初始化：`var s:str = nil` / `s = nil`（nil → str 隐式转换，NULL 指针；数组自动 0 填充的 str 元素亦为 nil）
- 比较：`nil == nil` 恒真；`nil == f` / `f == nil` 判断函数指针是否为 NULL；`nil == s` / `s == nil` 判断字符串指针是否为 NULL（0 初始化检测）
- 显式转换（`as`）：
  - `nil as u64` → `0`
  - `nil as func(...) -> ...` → NULL 函数指针
  - `nil as str` → NULL 字符串指针
- 隐式转换（赋值）：仅允许 → func / str 类型（0 初始化）；nil → u64 仅显式

### 4.3 赋值语句

```
<name> = <expr>;
<name> += <expr>;
<name> -= <expr>;
<name> *= <expr>;
<name> /= <expr>;
<name> %= <expr>;
```

- 赋值语句返回 `void`，不允许连续赋值
- `a = b = 1;` 编译错误（`b = 1` 返回 void，不能赋给 a）
- 赋值只能作为语句，不能作为表达式嵌套

### 4.4 控制流

**if/else**
```
if <expr> {
    <body>
} else {
    <body>
}
```
else 分支可选。

**while**
```
while <expr> {
    <body>
}
```

**for**
```
for (<init>; <condition>; <update>) {
    <body>
}
```
C 风格三段式。init 可以是变量声明或赋值语句。

**break / continue / return**
```
break;
continue;
return <expr>;
return;
```

### 4.5 块语句

```
{
    <stmt1>;
    <stmt2>;
    ...
}
```
块创建新作用域。

### 4.6 表达式

**优先级（从低到高）**

| 优先级 | 运算符             | 结合性 |
|--------|--------------------|--------|
| 1      | `\|\|`             | 左     |
| 2      | `&&`               | 左     |
| 3      | `\|`               | 左     |
| 4      | `^`                | 左     |
| 5      | `&`                | 左     |
| 6      | `== !=`            | 左     |
| 7      | `< > <= >=`        | 左     |
| 8      | `<< >>`            | 左     |
| 9      | `+ -`              | 左     |
| 10     | `* / %`            | 左     |
| 11     | `as`               | 左     |
| 12     | `! ~ -`(一元)      | 右     |
| 13     | `()`(函数调用)      | 左     |

## 5. 内建函数

### len / data（序列元信息与 C ABI 适配）

```c
len(s)   // 元素个数（str 为字节数）→ usize（C 的 size_t，见 §2.1）
data(s)  // 首元素指针 → ref *T（str → ref *u8，即 C 的 const char*）
```

两者配对就是 C 的 `(T*, size_t)`：**`f(data(s), len(s))`**。

- **`len`** 接受 `[N]T` / `own|ref|fatal []T` / `str`；其它类型是编译错误。
- **`data`** 接受同样的三类序列：
  - `own|ref|fatal []T` → 指向切片首元素（切片自带缓冲区）；
  - `[N]T` → 指向数组值自身的数据块（借用，随该值生命周期）；
  - `str` → 指向首字节（空串给的是**有效的**空串指针，不是 NULL）。
  结果恒 `ref *T`（**借用、永不拥有**）；空切片可能得到 NULL，C 侧不得解引用。
- **`data` 是语言级操作（指令），不是普通函数**：内置函数的实参会被复制进被调
  作用域，函数形态只能取到"副本"的地址（调用一结束即悬垂）。它必须像 `x.&` 那样
  在调用点就地取存储身份。相应地，clux 里**没有指针算术**（`p[i]` 是编译错误）：
  指针只用于喂 C，clux 侧遍历仍用序列下标。

### printf（M6 起不再是内建）

M1 曾把 C 的 printf 硬编码成内建函数（还额外提供 `%v`"打印任意值"的扩展）。
**M6 起整体删除**：printf 就是 libc 里的普通函数，由源码用 FFI 声明后调用
（走 `BCODE_EXTERN_CALL` → libffi 精确类型调用；定参与 C 变参都支持）：

```c
extern lib "*" as libc;                    // "*" = 当前进程自身（已链接进来的库）
extern func libc::printf(format: str, ...): void;

func main(): i32 {
    libc::printf("x = %d, s = %s, f = %.2f\n", 42, "hi", 1.5);
    return 0;
}
```

- **返回值**：C 的 `printf` 返回写入字符数；只用它做输出时声明成 `void` 即可
  （clux 的表达式语句不允许丢弃非 void 结果，声明成 `i32` 就得写 `_ = libc::printf(...)`）。
- **格式符**：C 的**原生**语义（`%d %u %x %o %c %s %f %e %g %p %%`，含宽度/精度）。
  原先内建版的 `%v` 扩展**不存在了**——聚合值请逐个成员打印。
- 别名别用 `c`：`::` 左侧先按作用域解析，**局部变量会遮蔽库别名**
  （`var c: Color = ...;` 之后 `c::printf` 会被当成对变量 c 的成员访问）。
- 其余内建函数（`len` / `upgrade` / `cfunc_ptr`）保持不变。

## 6. 语义规则

### 6.1 作用域

- 全局作用域：函数定义
- 函数体：块作用域
- for 的 init 部分声明的变量作用域为整个 for（含 body）
- 内层作用域可遮蔽外层同名变量

### 6.2 类型检查

- 赋值左右类型必须兼容（相同或布局兼容）
- 赋值语句返回 void，不允许连续赋值，不能将赋值用作表达式
- 算术运算：两边必须为同一类型族的同类型，结果类型与操作数一致
  - 整数混合：较小类型隐式提升到较大类型（同族内）
  - 整数与浮点混合：禁止，必须用 `as` 显式转换
  - bool 参与算术：禁止
- 比较运算：两边类型必须兼容，结果为 bool
  - nil 可与 nil / func 比较：`nil == nil` 恒真，`nil == f` / `f == nil` 判断函数指针是否为 NULL（0 初始化检测）
- 逻辑运算：操作数必须为 bool，结果为 bool
- 位运算：操作数必须为整数类型，结果类型与操作数一致
- 条件表达式（if/while）：必须为 bool，不接受整数
- `as` 转换：允许整数之间、整数与浮点之间、整数与 bool 之间
  - `0 as bool` → `false`，非零 → `true`
  - `true as i32` → `1`，`false as i32` → `0`
  - 禁止的：仅 str 与其他类型之间的转换
- const/volatile 在 M1 中词法识别但语义不生效（无 const 赋值检查）

### 6.3 无前置声明与多遍扫描

clux 没有前置声明（forward declaration），函数和类型的定义顺序无关：

```
func main():i32 {
    return add(1, 2);    // add 在后面定义，合法
}

func add(a:i32, b:i32):i32 {
    return a + b;
}
```

编译器对顶层做三遍扫描实现：
1. **Name Collection**：收集所有顶层名称（函数名、类型名），检测重复定义
2. **Type Collection**：收集函数签名和类型定义的完整类型信息
3. **Body Processing**：处理函数体，此时所有顶层符号和类型已知

### 6.4 返回值丢弃

- 函数调用的返回值不允许隐式丢弃
- 必须用 `_ = <expr>` 显式丢弃：`_ = some_func();`
- 赋值给变量即消费返回值：`var x:i32 = some_func();`
- 返回类型为 void 的函数调用不需要丢弃

### 6.4 整数提升

- 同族整数运算中，较小整数类型提升到较大类型
- 有符号与无符号混合时，提升为无符号类型（与 C 规则一致）
- 提升仅发生在同一二元运算的两个操作数之间，不是隐式转换

## 7. M1 支持度说明

### M1 支持
- 前导类型语法的前向声明（词法识别，但数组/切片/指针/元组类型 M1 不可用）
- 字符字面量（C 风格，值类型 u8，支持转义）

### M1 忽略
- const / volatile 修饰符（词法识别但语义不生效）

### M1 不支持
- 预编译指令
- 指针和取地址/解引用
- struct / union / enum
- 数组 `[N]T`、切片 `[]T`、元组 `<T1,T2>`
- typedef
- switch / case
- goto
- 逗号运算符
- 三元运算符
- sizeof / alignof
- 所有权系统
- 模块系统
- 错误处理机制
