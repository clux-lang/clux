# clux 字节码助记符（bytecode mnemonics）

clux 的 VM 以**字节码**作为可执行中间表示。编译期把 AST 降为字节码模块，运行期
由解释器直接执行。

> **事实源**：本表是派生参考，**唯一事实源是 `src/vm/bcode_asm_defs.c` 的
> `BCODE_ASM_TABLE`**（助记符名 + 操作数布局），汇编器 `bcode_asm.c` 与反汇编器
> `bcode_disasm.c` 共享该表，新增/改名/改操作数只需改表。指令语义与栈效果以
> `include/vm/bcode.h` 的 `bcode_op_t` 枚举注释与 `src/vm/exec.c` 的 handler 为准。
> 三者不一致时以代码为准，并回来修本文档。

---

## 1. 字节码模块布局

一个 `bytecode_t`（见 `include/vm/bcode.h`）只有两块产物：

1. **strtable（字符串表）**：编译期收集、运行期只读。包含变量名
   （`PUSH`/`STORE`/`DEFINE` 等的字符串索引）与字符串字面量（`PUSH_STRING` 的索引）。
2. **code（字节码流）**：`[opcode:u32][变长操作数...]` 的线性小端序列。

基本类型字面量（整数 / 浮点 / 布尔）全部**内嵌为立即数**，无独立常量池。
两块产物构成自包含可执行单元，可序列化落盘、重新加载直接执行。

执行器只移动 opcode 部分（`pc += 4` 后按 opcode 分派回调），**操作数由各指令的
回调自行消费并推进 pc**——指令天然变长，新增指令不影响执行器主干。

---

## 2. 文本汇编格式（`.cxs`）

汇编文本（如 `examples/asm/*.cxs`）语法约定：

- **行注释**：`;` 起直到行尾。
- **标签定义**：`name:` 独占一行（如 `_start:`、`fib:`、`rec:`、`L0:`）。
- **标签引用**：`[name]`，用于跳转 / 函数入口操作数（如 `jz [rec]`、
  `push_function [main]`），与裸数字 pc 区分。
- **字符串操作数**：`"..."`（C 风格转义），汇编时自动 intern 进 strtable 并写索引；
  strtable 是内部概念，文本不暴露独立字符串段。
- **立即数**：按类型助记符书写，如 `push_i32 40`、`push_bool 0`、
  `push_string "hi\n"`、`define_bound 3`。
- **助记符大小写**：本表以**全大写**登记（`ADD`、`STORE`…）；`.cxs` 示例使用小写
  （`add`、`store`…），汇编器大小写不敏感，全大写为规范输出形式。
- **伪指令**：`.byte <u32>` 兜底未知 opcode（大小写无关）；遗留 `[.section ...]`
  节标记行直接跳过（向后兼容）。
- **标签解析**：**两遍法**——第一遍登记标签 pc + 记录 fixup，第二遍回填，
  因此支持前向引用；引用未定义标签报错 `asm:<line>: error: undefined label '<name>'`。
- **反汇编标签命名**：按「跳转目标 / 函数入口锚点」集合去重、按 pc 升序确定性生成
  `L0`/`L1`/…，保证 `disasm → asm → disasm` 逐字节稳定往返。

---

## 3. 操作数类型

每条指令最多 4 个操作数槽，以 `BCODE_ASM_OP_NONE` 结束。

| 类型 | 汇编文本形式 | 含义 |
|------|--------------|------|
| `NONE` | — | 无操作数。 |
| `STR` | `"..."` | strtable 索引；文本内联字符串，底层写 u32 索引。 |
| `U32` | `<u32>` 或 `[label]` | 32 位无符号：类型 id、函数 id、成员数、pc 目标等。 |
| `U8` / `U16` / `U64` | 十进制无符号立即数 | 无符号脚整数（`U8` 用于 kind/flags 字节）。 |
| `I8` / `I16` / `I32` / `I64` | 十进制有符号立即数 | 有符号立即数，越界报错。 |
| `F32` / `F64` | 十进制浮点 | 反汇编以 `%.9g` / `%.17g` 输出。 |
| `BOOL` | `0` / `1` | 1 字节布尔立即数。 |

> 助记符常量立即数家族（`PUSH_I8` 等）的**操作数类型与后缀一一对应**；
> 类型 id 空间：内建类型预登记（如 `i32 = 2`、`void = 12`、`opaque = 17`），
> 程序类型 id ≥ 64 由 `DEFINE_TYPE` 声明分配；程序函数 id ≥ 64 由 `BIND_FUNC` 登记。

---

## 4. 助记符总表

### 4.1 取址 / 载值

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH` | `STR` | 按名字在 scope 链查找并压**借用引用**（不 clone、不转移所有权）。 |
| `STORE` | `STR` | `dst = scope_lookup(name)`，`value_assign(dst, pop())` → 压回结果引用。 |
| `STORE_NIL` | `STR` | `?T` 变量置 none：查找变量 → 置 `ok=false` → 压回 dst（**只置 tag**，不释放/清零 value 字段）。 |
| `PUSH_STRING` | `STR` | 压字符串字面量借用引用。 |
| `PUSH_VALUE` | `U32`（offset） | 压 `stack[sp-1-offset]` 的借用引用（offset=0 即 dup 栈顶；深层取址）。 |
| `LOAD` | `STR` | 从 global scope 按名查 **type value** 压栈（基本类型注册于 global scope）。 |
| `LOAD_TYPE` | `U32`（类型 id） | 从 `vm->types_by_id` 按 id 查类型并压其 type value（开放/密封皆可拉回）。 |
| `LOAD_FUNCTION` | `U32`（函数 id） | 从 `vm->functions_by_id` 按 id 查函数并压其 func value（引用语义：同一对象）。 |
| `SET_TYPE_NAME` | `STR` | 弹栈顶 type value → 设置显示名（当前无生成点，为具名类型预留）。 |
| `PUSH_UNDEFINED` | — | 压 void 类型 value（"类型待推导 / 未初始化声明"占位符）。 |

### 4.2 常量立即数

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_I8` / `PUSH_I16` / `PUSH_I32` / `PUSH_I64` | 对应宽度有符号立即数 | 压入 i8/i16/i32/i64 值。 |
| `PUSH_U8` / `PUSH_U16` / `PUSH_U32` / `PUSH_U64` | 对应宽度无符号立即数 | 压入 u8/u16/u32/u64 值。 |
| `PUSH_F32` / `PUSH_F64` | 对应宽度浮点立即数 | 压入 f32/f64 值。 |
| `PUSH_BOOL` | `BOOL` | 压入 1 字节布尔值。 |

### 4.3 类型声明 / 定义（两遍协议）

类型构造统一为**两步**：`PUSH_XXXX` 创建**开放**类型对象（未密封）并压其 type value
→ `DEFINE_TYPE <id>` **声明**（绑定程序 id、登记进 `types_by_id`，此后任何地方可
`LOAD_TYPE <id>` 拉回，实现向前引用）→ 定义阶段 `LOAD_TYPE <id>` 拉回 → 设字段
（`DEFINE_FIELD` / `DEFINE_BOUND` / `APPEND_ELEM` / `FUNC_TYPE_PARAM` / `SET_TYPE` 等）
→ `SEAL` **定义收尾**（去重 intern + 布局 + 置 `sealed`）。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_FUNC_TYPE` | — | 分配空函数签名类型（开放态）→ 压其 type value（构造起点）。 |
| `FUNC_TYPE_PARAM` | — | 弹 type value → 追加为签名下一参数类型。 |
| `FUNC_TYPE_RETURN` | — | 弹 type value → 设为签名返回类型。 |
| `FUNC_TYPE_VARARG` | — | 标记签名为可变参数（如 `printf`）。 |
| `PUSH_ARRAY` | — | 分配空数组类型（开放态，elem=NULL）→ 压其 type value。 |
| `DEFINE_BOUND` | `U32`（长度 N） | 弹栈顶元素 type value → 设元素类型 + 边界 N（**定长成形**：数组类型恰由「元素类型 + 边界」构成，不可重复）。 |
| `PUSH_CONST` | — | 分配空 const 限定类型（开放态，sub=NULL）→ 压其 type value。 |
| `PUSH_VOLATILE` | — | 分配空 volatile 限定类型（开放态，sub=NULL）→ 压其 type value。 |
| `PUSH_OPT` | — | 分配空 optional 类型（开放态，inner=NULL）→ 压其 type value。 |
| `PUSH_ENUM` | — | 分配空 enum 类型（开放态，underlying=NULL）→ 压其 type value。 |
| `ENUM_VARIANT` | `[STR][I64]` | **peek** 开放 enum → 追加 variant（名从 strtable 拷贝，值按底层宽度截断）。 |
| `MAKE_ENUM` | — | 弹 type value + 弹底层整数值 → 按底层宽度截断构造 enum 值。 |
| `PUSH_STRUCT` | — | 分配空 struct 类型（开放态，fields=NULL）→ 压其 type value。 |
| `DEFINE_FIELD` | `STR`（字段名） | 弹栈顶字段 type value → **peek** 开放 struct / cunion → 追加具名字段。 |
| `PUSH_TUPLE` | — | 分配空 tuple 类型（开放态，elems=NULL）→ 压其 type value。 |
| `APPEND_ELEM` | — | 弹栈顶元素 type value → **peek** 开放 tuple → 追加**匿名**元素（长度 = 追加次数）。 |
| `PUSH_UNION` | — | 分配空 tag union 类型（开放态，members=NULL）→ 压其 type value。 |
| `UNION_MEMBER` | `STR`（member 名） | **peek** 开放 union → 追加 member（tag = 追加序，payload_type 暂 NULL，随后配一个 `DEFINE_FIELD`）。 |
| `PUSH_CUNION` | — | 分配空 C 语义 union 类型（开放态，member 追加走 `DEFINE_FIELD`，**无 tag**）→ 压其 type value。 |
| `SET_TYPE` | — | 弹栈顶 type value（sub）→ 设为栈顶开放对象的 sub / base / inner（`LOAD_TYPE` → `SET_TYPE` → `SEAL`）。 |
| `DEFINE_TYPE` | `U32`（类型 id） | 弹 type value（**声明**：消费栈）→ 绑定程序 id（≥64）写入 `t->id` 并登记 `vm->types_by_id`（幂等，多 id 可别名同一类型）。 |
| `SEAL` | — | 弹 type value（**定义收尾**：消费栈）→ 经 `value_seal` 分派 `vtable->type_seal`：按类型各自池查重 intern、计算布局、置 `sealed`；去重命中则回收开放对象并重定向栈引用；随后按开放对象自身 id 幂等重绑登记（避免登记表悬垂）。 |
| `CREATE_CONST` | — | 一次性快捷：弹 type value → `type_const_intern` → 压回（不走向上两遍构造；编译器 hoist 区用 `PUSH_CONST`+`SET_TYPE`+`SEAL`）。 |
| `CREATE_VOLATILE` | — | 同上，`type_volatile_intern`。 |
| `IS_TAG` | `U32`（tag 立即数） | 弹 union 值 → 读 data 首部 tag 与立即数比较 → 压 bool（`x is Member` 已在编译期解析出 tag 值）。 |

### 4.4 符号绑定与函数对象

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `DEFINE` | `STR` | 弹栈定义变量：**永远双弹 `[value, type-spec]`**（type-spec 为 type value 或 `undefined`；undefined 表示从值推断类型）。函数定义亦走此指令（`PUSH_UNDEFINED` 作 type-spec）。 |
| `PUSH_FUNCTION` | `U32`（entry pc / 标签） | 弹栈顶签名类型 → 构造 `bcode_function_t{entry_pc}`（自建孤立 `closure_scope`，`fn->id` 默认 0）→ 压 func value。 |
| `BIND_FUNC` | `U32`（函数 id） | **peek** 栈顶 func value（不弹栈）→ 填 `fn->id = id` 并登记 `functions_by_id`（幂等）。id 单一来源。 |
| `SET_FUNC_NAME` | `STR` | **peek** 栈顶 func value → 拷贝显示名到 vm 堆（随对象释放）。仅具名函数定义写入，匿名函数字面量不写。 |
| `SET_CLOSURE` | `STR`（捕获名） | 弹栈顶捕获值 → **clone** 进**栈下**函数对象（`peek` 栈下 1 位）的 `closure_scope` 捕获槽（define-or-replace，替换占位 undefined）。闭包为 clone 值语义。 |
| `MAKE_FUNCTION` | `U32`（函数 id） | 从 `functions_by_id` 拉**基底**函数对象 → 实例化**独立新实例**（共享 entry_pc/cfunc/type/name/id，新 `closure_scope` 按基底捕获槽名以 undefined 占位）→ 压栈。函数定义每次求值生成新对象（循环内多个闭包互不干扰），对标 `LOAD_FUNCTION` 的"同一对象"引用语义。 |
| `DEFER` | — | 弹栈顶函数值 → 借用登记进当前作用域的 **defer 栈**（延迟执行块 `defer ... { }`，见 §4.14 与 `docs/m6-design.md` §14.26）。 |

### 4.5 值构造 / 字段与下标访问

值构造是**多步协议收尾**：类型经 `PUSH_XXXX → DEFINE_TYPE → LOAD_TYPE → … → SEAL`
构造后以 type value 留在栈顶，随后按类型字段序压入成员值，由 `CONSTRUCT N` 收尾。
`FIELD_*` / `INDEX_*` / `LENGTH` 走 value 层 vtable 分派，越界与类型错误以**硬错误
value** 返回，由 `exec_drive` 捕获停机。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CONSTRUCT` | `U32`（成员数 N） | 栈布局 `…, type_value, v1 … vN`（类型在底、vN 在顶）：先逆序弹 N 个成员值，再弹类型位，按类型种类构造 value。`array` 分支允许部分填充（缺失位为零值）；`struct` 分支字段数须一致、逐字段 `value_implicit_cast` + blit；`union` / `cunion` 分支成员数须 == 2（**显式 tag / member 下标哨兵** + payload）。 |
| `FIELD_GET` | `STR`（字段名） | 弹 self → 按名查偏移 → 压**借用引用**（is_own=false，data 指向 self 数据块内偏移，零拷贝；绑定/返回时经 clone materialize）。union 分支反查 member + **运行期 tag 校验**（不符 → 硬错误）；cunion 分支写/读 offset 0、无 tag 校验。 |
| `FIELD_SET` | `STR`（字段名） | 弹 self + val → `value_implicit_cast` 到字段类型 → dispose 旧值 → blit 新值 → **压回 self**（链式复用）。复合赋值 `p.x op= v` 编译为 `PUSH_VALUE 0` + `FIELD_GET` + op + `FIELD_SET`。 |
| `INDEX_GET` | — | `get_item`：栈 `…, self, index` → 弹 index、self → `vtable->get_index` → 压元素副本（**消费 self**）。 |
| `INDEX_SET` | — | `set_item`：栈 `…, self, index, val` → 弹三者 → `vtable->set_index` → 压回 self。 |
| `LENGTH` | — | 弹 self → 压 `value_length(self)`（`vtable->length` 代理，usize）。**当前编译器不生成此指令**：语言层 `len(x)` 编译为对内建函数 `len` 的调用（`PUSH "len"; …; CALL 1`），内建实现覆盖数组 / 切片 / str；本指令与 VM handler 保留待用。 |

> **运行期越界检查**：数组 / 切片下标按运行时长度校验 `0 <= index < len`，越界
> （含负索引按无符号读入后 `>= len`）返回硬错误 `array index N out of bounds (len=M)`
> 并停机；索引须为整数，否则报 `array index must be an integer`。
> `INDEX_GET` / `INDEX_SET` 与 `CALL` 一致**消费** `self`，多次访问需 `PUSH_VALUE` 复制引用。

### 4.6 optional 与 nil

`nil` 不是值（运行时无 nil value 类型）：`a = nil` 走 `STORE_NIL`，构造器字段 / fill
中的 `nil` 走 `PUSH_OPT_NONE`，`x == nil` 系列判定走 `OPT_IS_NONE`（**tag 比较专用
指令，非 vtable eq 分派**）。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_OPT_NONE` | `U32`（option 类型 id） | 从 `types_by_id` 查 option 类型 → 压 `ok=false` + value 全零的 `?T` 值块。 |
| `UNWRAP` | — | 弹 `?T` 值 → ok 则**借用**返回 value 字段（零拷贝）；none → panic 错误值 `unwrap '.!' on none optional value`。 |
| `OPT_IS_NONE` | — | 弹 `?T` 值 → 压 bool（`ok == false`）。`x != nil` 编译为 `OPT_IS_NONE` + `NOT`。 |

### 4.7 指针与所有权（M3）

指针类型与签名同族：`PUSH_PTR <kind> → DEFINE_TYPE → LOAD_TYPE → SET_TYPE 设 base → SEAL`。
**运行期零所有权标志**——所有权由 `type_t.ownership` 正交字段编码（own 总拥有 /
ref 总借用 / fatal 在非空时拥有 / share RC 强引用 / weak RC 弱引用），type kind
统一为 `TYPE_KIND_PTR`。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_PTR` | `U8`（ownership） | 分配空指针类型（开放态，base=NULL）→ 压其 type value。ownership ∈ `OWN_OWN / _REF / _FATAL / _SHARE / _WEAK`。 |
| `NEW` | — | 弹类型位 + 1 个成员值 → 按类型构造 `own *T` 堆块（`new T{...}`）。 |
| `PTR_GET` | — | 弹指针值 → 压被指向 `T` 的**值副本**（`r.*` 解引用 GET）。 |
| `PTR_SET` | — | 弹指针值 + val → 隐式转换后写回被指向 `T` → 压回指针（`r.* = v`）。 |
| `ADDR` | — | 弹值 → 压 `ref *T` 指针（`x.&` 后置取址，借用）。 |
| `MOVE` | — | 弹值 → 压同类型值（`move(x)`，所有权转移；语义检查在编译期，指令本身平凡传递）。 |
| `CLONE` | — | 弹值 → 压深拷贝值（`clone(x)`，`value_clone` 分派）。 |
| `DISPOSE` | — | 弹值 → 递归释放其内嵌 own 堆块（不压回）。用于 `_ = expr` 显式丢弃 fatal / own；借用（ref、`x.&`）跳过。 |

### 4.8 切片（M4）

切片是胖指针 `{ void *ptr; uint64 len; }`，三种所有权修饰（own/ref/fatal）由
`type_t.ownership` 编码，与指针对齐。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_SLICE` | `U8`（ownership） | 分配空切片类型（开放态，elem=NULL）→ 压其 type value。ownership ∈ `OWN_OWN / _REF / _FATAL`。 |
| `SLICE` | `U8`（flags） | bit0 = low 存在，bit1 = high 存在；弹 self + 按 flags 弹 high/low → 压 `ref []T` 胖指针（`s[low:high]`，**恒产 ref []T**；省略 low=0、high=len）。含运行期边界检查 `0 <= low <= high <= len`。 |
| `MAKE` | `U32`（init_count） | 弹 init_count 个初始值 + 弹 length + 弹 elem type → 分配 `len * elem_size` 堆块并逐元素布值 → 压 `fatal []T`（`make(T, N, init...)` 宏函数）。 |

### 4.9 模块（M5）

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `IMPORT` | `STR`（模块规范路径） | 查 `vm->modules` → 构造模块值（type = 模块类型，data = `module_t*`）压栈。**幂等**：同一路径返回同一模块。 |
| `GET_MEMBER` | `STR`（成员名） | 弹模块值 → 从 `module_t->exports` 按名取成员值压栈（命名空间隔离：仅导出成员可见）。 |
| `DATA` | —（无操作数） | 弹一个**序列值**（array/slice/str）→ 压 `ref *T`（T = 元素类型；str → u8），指向首元素。**C ABI 适配**：与 `len` 组成 C 的 `(T*, size_t)`。恒借用（dispose 跳过）；空序列可能为 NULL。见 §14.21。 |

### 4.9b 外部符号访问（M6 FFI）

**函数调用：**

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `EXTERN_CALL` | `STR`（`"lib::name"`）+ `U32`（argc）+ `U32`（ret_kind） | 从**当前执行模块**（`bc->module_path` → `vm->modules`）的库表（`module_t.ffi_libs`，别名 → 句柄）取句柄 → `ffi_lib_sym`（dlsym/GetProcAddress）→ 弹 `argc` 个实参 → 调类型化调用层 → 按 `ret_kind` 归一化压栈。 |

- `ret_kind`（返回类型码，`vm/bcode.h` 的 `BCODE_FFI_RET_*`）：`0` = void、
  `1` = i32、`2` = i64、`3` = 指针（opaque）、`4` = f64
  —— 以上 5 个是 v0 的原始编码（旧 `.cxb` 语义不变）；M6 补齐到**逐类型**：
  `5` = u32、`6` = u64、`7` = i8、`8` = i16、`9` = u8、`10` = u16、`11` = bool、
  `12` = f32、**`13` = str（char\*）**、**`14` = TYPED（按类型值返回）**。
  运行期按码造出**与源码声明一致的类型**（旧编码把 u64 返回造成 i64，使用点会在
  比较/赋值时撞上隐式转换）。
  **高位打包变参信息**（不新增操作数，旧产物高位恒 0）：
  `bit8` = 变参、`bits9..15` = 固定参数个数。
  `BCODE_FFI_RET_TYPED` 用于 struct/cunion/enum 与带类型的指针：指令前由
  compiler 用 `compile_type_expr` 压一个**类型值**（与 `CONSTRUCT` 同一机制），
  运行期按该类型读返回内存并造值（程序自定义类型的宽度/布局查不了小整数码）。
- **实参约定**：`ret_kind != 4` 时实参按 **int64 宽度**取出（整数符号/零扩展、指针原样）；
  `ret_kind == 4`（v0 规则：F64 返回 ⇒ 全 double 签名）时实参须为 f32/f64，整数实参明确报错而不猜着转换。
  sema 在**调用点**按同样口径校验数量与类型（整数族 ⇄ 整数族、指针 ⇄ 指针/切片/str、
  浮点只在浮点签名里、浮点变参拒绝）——见 `docs/m6-design.md` §14.18.2。
- **来源**：clux 源码里的 `extern cfunc <库>::<名>(...)` / `extern func <库>::<名>(...)`
  调用点（两者等价）；`extern lib "<路径>" as <别名>;` 提供别名与其加载路径。
  **判据是关键字，不按文件后缀判类**；路径写成 `"*"` 表示**当前进程自身**
  （RTLD_DEFAULT 语义，见 §14.18），此时不加载任何文件。
- **诊断**：库别名未加载、符号不存在、`lib::name` 畸形、实参过多/类型不符，均返回 error 值（`exec:` 前缀，含库名与符号名）。

**数据符号读写（`extern var`，动态库导出的全局变量）：**

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `EXTERN_LOAD` | `STR`（`"lib::name"`）+ `U32`（kind） | 同一条库表/`ffi_lib_sym` 通路取**变量地址**（数据符号的符号地址就是变量地址）→ 按 `kind` 读 size 字节 → 压对应类型的值（+1）。`kind = 13`（TYPED）时**指令前有一个类型值**（compiler 压入），宽度按该类型的 size。 |
| `EXTERN_STORE` | `STR`（`"lib::name"`）+ `U32`（kind） | 同路径取变量地址 → 弹栈顶值 → 按 `kind` 把 size 字节写进那块内存（**不压回**，−1）。`kind = 13`（TYPED）时栈布局是 `[type, value]`。符号解析失败时先于弹栈发生（操作数栈保持原样）。 |

- `kind` 是"宽度 + 类别"码：`0`=i8 `1`=i16 `2`=i32 `3`=i64 `4`=u8 `5`=u16 `6`=u32
  `7`=u64 `8`=f32 `9`=f64 `10`=opaque（指针宽度）`11`=bool `12`=str（char\*）
  `13`=TYPED（struct/cunion/enum：按类型值读写）。
  **必须与库里符号的真实类型一致**——读多/写多会踩到相邻符号，因此 sema 只放行
  这张表里的类型 + TYPED 覆盖的聚合类型（`ffi_data_kind_of` / `ffi_data_type_of` /
  `ffi_data_kind_needs_type` 同处实现，见 `vm/ffi_lib.c`）。
  `bool` 单独一码（宽度 1，但类型要还原成 `bool`，否则 `flag != true` 报类型不匹配）。
- **来源**：`extern var <库>::<名>: T;` 的读取点与赋值点（`lib::name = v` / `op=`）。
  复合赋值展开为 `EXTERN_LOAD → 运算 → EXTERN_STORE`。
- **不缓存地址**：与 `EXTERN_CALL` 一致，每次执行都重新 `dlsym`（语义简单、库表换绑
  后立刻生效）；需要时再引入按符号缓存。
- **实现说明**：调用路径最终走 libffi 的 `ffi_prep_cif` + `ffi_call`（Windows x64 端口
  用 MASM `ml64` 汇编 Intel 变体，见 `docs/m6-design.md` §14.12）；整数/指针/浮点
  的旧类型化层（`ffi_call_int` / `ffi_call_f64`）保留为不依赖 libffi 的等价通路。

### 4.10 算术 / 逻辑 / 位运算（均无操作数，弹参压结果）

| 助记符 | 语义 | 助记符 | 语义 |
|--------|------|--------|------|
| `ADD` | 加 | `AND` | 逻辑与 / 位与 |
| `SUB` | 减 | `OR` | 逻辑或 / 位或 |
| `MUL` | 乘 | `BXOR` | 位异或 |
| `DIV` | 除 | `SHL` | 左移 |
| `MOD` | 取模 | `SHR` | 右移 |
| `EQ` | 相等 | `NEG` | 取负（一元） |
| `NE` | 不等 | `NOT` | 逻辑非（一元） |
| `LT` | 小于 | `BNOT` | 位取反（一元） |
| `LE` | 小于等于 | | |
| `GT` | 大于 | | |
| `GE` | 大于等于 | | |

> 二元运算的类型协商（整数提升、隐式转换白名单）在 vtable 内完成；类型不兼容
> 返回 error value，由 sema 翻译为诊断（运行期不会遇到）。短路 `&&` / `||` **不用**
> 这些指令做惰性求值，而是由编译器展开为 `JZ` / `JNZ` 跳转。

### 4.11 类型修饰 / 转换

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CAST` | — | `as` 显式转换：类型经栈顶 type value 传递（`LOAD`/`LOAD_TYPE` 压入），弹 type + 值 → `value_explicit_cast` → 压结果。 |

### 4.12 调用 / 返回

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CALL` | `U32`（argc） | callee 在 `stack[sp-1-argc]`、实参紧随其后 → 经唯一入口 `value_call`（→ `func_vcall` 通用调用流程）→ 结果压栈。FFI / 内建 / 字节码函数同走此路径。 |
| `RET` | — | 栈顶即返回值；返回 interrupt 哨兵（`INTERRUPT_RETURN`），由 `bcode_call_cfunc` 子循环捕获并取出返回值。void 函数返回 `PUSH_UNDEFINED` 的 void value。 |

### 4.13 控制流（标签型操作数为 u32 绝对字节 pc）

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `JMP` | `U32`（pc） | 无条件跳转。 |
| `JZ` | `U32`（pc） | 弹引用 → 显式转 bool → 为 false 则跳转。 |
| `JNZ` | `U32`（pc） | 弹引用 → 显式转 bool → 为 true 则跳转。 |

> 跳转跨作用域时由**编译器**在跳转前补发相应数量的 `POP_SCOPE` 平衡作用域链
> （执行器对跳转无感知）；标签处操作数栈深度由编译器静态追踪校验。

### 4.14 作用域 / 栈管理

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_SCOPE` | — | `scope_new(alloc, current)` 进入新作用域。 |
| `POP_SCOPE` | — | 弹出并销毁当前作用域：**先逆序执行本作用域的 defer 栈**（见 `DEFER`），再回收本块全部临时值（`dispose owned` + 释放 vars/scope）。 |
| `DEFER` | — | 弹栈顶**函数值**（defer 的无参 void 闭包）→ **借用**登记进**当前作用域**的 defer 栈（`scope_t::defers`，登记序）。作用域退出时在 dispose/销毁**之前**逆序（LIFO）弹出逐个 `value_call`；返回值不传给消费点（error 值打 stderr）。值本体归 `owned`（`MAKE_FUNCTION` 自动 track），栈只存指针 → 无泄漏/无 double free。见 §14.26。 |
| `POP` | — | 丢弃栈顶引用（不释放，归 scope 管理）。 |
| `HALT` | — | 停止执行（注册段之后拦截落入函数体区）。 |

---

## 5. `.cxb` 二进制容器

`.cxb`（clux bytecode）与 `.cxs` 互为同一**模块图**（module graph）的两种落盘形态：
二进制直存体积小、加载快（无需解析），用于分发 / 缓存。

### 5.1 v3：模块表 + 每模块 FFI 库表（当前格式）

M5 的模块语义必须在字节码层成立：一个可执行的 `.cxb` 必须**自包含它 import 的
全部模块**，否则加载时 `IMPORT <path>` 查不到模块（依赖模块不在容器里）。因此
v2 容器从"单模块"扩展为"模块表"；M6 再补上 **FFI 库表**（v3）——同一个道理：
`module_t.ffi_libs` 是编译期扫描 AST 建的，字节码里只剩
`EXTERN_CALL "别名::符号"`，容器不携带声明的话，加载方不知道该 dlopen 什么。
格式见 `include/vm/bcode_graph.h`，**全部小端**：

```
[magic "CXBC" : 4 字节]                  /* BCODE_SERIAL_MAGIC = 0x43425843 */
[format_version : u32]                   /* BCODE_GRAPH_VERSION = 3 */
[module_count : u32]                     /* 模块表条目数（依赖优先顺序） */
[entry_index : u32]                      /* 入口模块在表中的下标 */
  重复 module_count 次：
    [import_path_len : u32][import_path] /* **源码里写的路径**（如 "./math"）；0 = 入口模块 */
    [importer_index : u32]               /* 导入者模块下标；0xFFFFFFFF = 入口模块 */
    [str_count : u32]                    /* 该模块的字符串表 */
      重复 str_count 次：[str_len : u32][bytes]
    [code_len : u64][code : code_len]    /* 该模块的字节码流 */
    [export_count : u32]                 /* 导出表 */
      重复 export_count 次：[name_len : u32][name][owner_index : u32]
    [ffi_count : u32]                    /* M6 FFI：extern lib 库表（v3 起；v2 段到此结束） */
      重复 ffi_count 次：[alias_len : u32][alias][path_len : u32][path]
```

- **模块顺序 = 依赖优先**（依赖先编译先登记，与 `vm->modules` 插入顺序一致）：
  加载方按表序逐个"执行注册段 → 重建导出表 → 绑定模块"，依赖因此先于使用者就绪，
  `IMPORT` 在注册段里即可命中。
- **存的是"源码里写的路径"，不是规范化后的宿主路径**：每个模块记录 `import_path`
  （其导入者那条 `import` 语句里的写法）与 `importer_index`（导入者下标，入口为
  无）。**规范路径不落盘**——加载时以"入口根路径（`.cxb` 文件自身的规范化路径）"
  为起点，沿导入链逐个 `module_path_resolve(import_path, 导入者的 canonical)`
  重算，得到 `vm->modules` 的 key。运行期 `op_import` 对 `IMPORT <源码路径>`
  （`bc->module_path` 为基准）用的是**同一个函数、同一条链**，因此两侧算出的 key
  必然吻合。产物由此与宿主目录布局解耦：容器搬到任何目录、改任何名字都能运行。
- **入口模块的 import_path 为空、importer 为"无"**：运行期只有 `IMPORT` 会按路径
  查表，而入口不可能被 import（那构成循环依赖，编译期已拒绝）。
- **导出表必须随容器携带**：导出名单来自 AST 的 `is_exported` 标记（不在字节码
  里）。`owner_index` 指向"值真正所在的模块"——`export * from "./x"` 代理导出的
  值借用依赖模块的 `global_scope`，编译期已解析成最终表，容器只记录值的位置，
  加载方即可原样重建（owner ≤ 自身，因此按表序解析总是可行）。
- **FFI 库表（v3）必须随容器携带**：`extern lib "<路径>" as <别名>;` 的**别名**是
  运行期 `EXTERN_CALL "别名::符号"` 的查表键，**路径**是 `dlopen`/`LoadLibrary`
  参数（判据是关键字与内容，**不按 `.dll`/`.so` 后缀判类**）。库表**按模块**存
  （依赖模块的声明归它自己），加载侧在重建 `module_t` 后按表 `ffi_lib_open`
  填 `ffi_libs`，失败即报错。缺这一段的表现是运行期
  `exec: extern call: no loaded library 'x' in this module`。
- **字符串表**长度前缀、无 NUL 终止，可承载内嵌 `\0`；加载方严格校验魔数、版本、
  各段长度与边界，任何截断 / 不一致视为**损坏并失败**（打印诊断，**不 panic**）。
- **v1/v2 兼容**：`format_version = 1` 的单模块容器（旧 `bcode_serial` 产物）与
  `= 2` 的模块表容器（无 FFI 段）仍可加载，分别按"单模块、空路径、无导出表"与
  "无 FFI 声明"处理。
- API（`include/vm/bcode_graph.h`）：`bcode_graph_serial_file` / `bcode_graph_serial_mem`
  （从已编译的 `vm` + 入口模块写整图）、`bcode_graph_serial_image_file`（`@cxs → .cxb`
  直写，owner 下标由文本携带）、`bcode_graph_from_file` / `bcode_graph_deserial`、
  `bcode_graph_disasm_to_file` / `bcode_graph_asm_from_file`（文本形态）。
- 文本形态（`.cxs`）的指令行：`MODULE "<源码路径>" <导入者下标>`（多模块）、
  `export <名字> <owner 下标>`、`externlib "<库路径>" as <别名>`（M6；**有声明才写**）。
  单模块且无 FFI 的产物与旧格式**逐字一致**（不写任何指令行），`examples/asm` 的
  手写 `.cxs` 样本与既有往返测试因此不受影响。

### 5.2 v1：单模块（历史格式，仅读）

```
[magic "CXBC" : 4 字节]
[format_version : u32]                   /* BCODE_SERIAL_VERSION = 1 */
[str_count : u32]
  重复 str_count 次：[str_len : u32][bytes]
[code_len : u64]
[code : code_len 字节]
```

- API：`bcode_serial` / `bcode_serial_mem` / `bcode_deserial` /
  `bcode_deserial_from_file`（失败时 `*out_bc = NULL`）。新代码用 v3 模块图 API。
- 版本号独立于语言版本，不匹配即拒绝加载。

---

### 5.3 `.cxs` 文本形态（模块指令）

单模块文本与旧格式一致（无反汇编之外的任何指令行）。多模块文本在每段模块
代码前插入指令，模块顺序与容器表序一致（依赖优先）：

```
MODULE "./math" 1          ; 依赖模块：**源码里写的路径** + 导入者模块下标（1 = 入口）
externlib "msvcrt.dll" as crt   ; M6 FFI：该模块的 extern lib（别名 → 库路径；有声明才写）
export add 0               ; 导出项：名字 + owner 模块下标（0 = 本表第 0 个模块）
_start:                    ; 该模块的反汇编代码（原样交给单模块汇编器）
    ...
MODULE                     ; 入口模块：裸 MODULE（无导入者、路径为空）
export main 2
    ...
```

- 汇编器接受 0 个指令行（= 单模块入口，向后兼容手写 `.cxs`）或 N 个
  `MODULE`/`export`/`externlib` 行；入口 = 首个路径为空的模块。
  指令行**只出现在行首**，且 `externlib` 归入"当前模块"（多模块下每个模块
  带自己的库表；加载侧按模块重建 `ffi_libs`）。
- 往返不变式：`.cxs → .cxb → .cxs → .cxb` **字节一致**，且
  `源码 → emit → disasm → asm` 与源容器**逐字节一致**（emit 侧渲染与
  `.cxb → .cxs` 共用同一渲染器）。含 `externlib` 的 FFI 产物同样满足该不变式。
## 6. `bc` 子命令

字节码工具链**非主线**（主线是 `build` 产出机器码，M7/M12 实现），归 `bc` 子命令：

```
clux bc emit   <file.cx>    [-o PATH]   # 源码 → .cxb
clux bc asm    <file.cxs>   [-o PATH]   # .cxs 汇编文本 → .cxb
clux bc disasm <file.cxb>   [-o PATH]   # .cxb → .cxs 汇编文本
```

- 输出路径优先级：`-o PATH` / `-o=PATH` → `--output=PATH` → 位置参数 → 按输入同名
  换扩展名（`emit`/`asm` → `.cxb`，`disasm` → `.cxs`）。`-o` 是**唯一**允许的单横线
  短选项（`cmd_args_parse` 只识别 `--`，由 `bc` handler 自行扫描）。
- 成功时 stdout 打印 `wrote <path>`。
- `run <file>` **按内容判定**输入：前 4 字节为 `CXBC` 魔数则当字节码执行，否则当源码
  编译执行；`run` 没有模式选项，也不感知 `.cxs`（`.cxs` 须先 `bc asm` 转 `.cxb`）。
- 端到端等价：`clux run <file.cx>` ≡ `clux bc emit <file.cx> && clux run <file.cxb>`；
  `.cxs → .cxb → .cxs → .cxb` 两次二进制逐字节一致。

---

## 7. 手写汇编约定

`.cxs` 手写或反汇编产物遵循与编译器一致的布局：

1. **类型提升区（hoist）两遍**：
   - pass 1 声明：`PUSH_ARRAY` / `PUSH_FUNC_TYPE` / `PUSH_CONST` / `PUSH_VOLATILE` /
     `PUSH_PTR <kind>` / `PUSH_SLICE <kind>` … → `DEFINE_TYPE <id>`；
   - （全局类型定义的名字绑定插在两遍之间：`LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "Name"`）
   - pass 2 定义：`LOAD_TYPE <id>` → `DEFINE_BOUND N` / `FUNC_TYPE_PARAM`* /
     `FUNC_TYPE_RETURN` / `DEFINE_FIELD` / `SET_TYPE` → `SEAL`（依赖后序）。
2. **函数注册段**：`LOAD_TYPE <sig_id>` + `PUSH_FUNCTION [entry]` + `BIND_FUNC <id>` +
   `SET_FUNC_NAME "name"` + `PUSH_UNDEFINED` + `DEFINE "name"`，其后 `HALT` 拦截落入函数体区。
3. **函数体区**：各函数体以 `RET` 结尾，入口由 `PUSH_FUNCTION` 记录的 pc 进入；
   函数体头部按**倒序** `PUSH_UNDEFINED; DEFINE "param"` 绑定参数（调用时实参按序
   压栈，栈顶是最后一个参数）。

---

## 8. 示例

`clux bc disasm` 的真实产物（源码：声明 libc 的 printf 后调用它，见 §14.19）：

```c
extern lib "*" as libc;
extern func libc::printf(format: str, ...): void;
func main(): void {
    var a = .[3]i32{1,2,3};
    libc::printf("%d\n", len(a));
}
```

```
externlib "*" as libc    ; ---- 容器指令行：本模块的 extern lib（M6，见 §5.3）----
PUSH_FUNC_TYPE           ; ---- hoist pass 1：声明签名类型 ----
DEFINE_TYPE 64
PUSH_ARRAY               ; 声明数组类型
DEFINE_TYPE 65
LOAD_TYPE 64             ; ---- hoist pass 2：定义签名（返回 void = id 12）----
LOAD_TYPE 12
FUNC_TYPE_RETURN
SEAL
LOAD_TYPE 65             ; 定义数组类型（元素 i32 = id 2，长度 3）
LOAD_TYPE 2
DEFINE_BOUND 3
SEAL
LOAD_TYPE 64             ; ---- 函数注册段 ----
PUSH_FUNCTION [L0]
BIND_FUNC 64
SET_FUNC_NAME "main"
POP
LOAD_FUNCTION 0          ; 签名类型值（printf 声明：func(str, ...) -> void）
PUSH_UNDEFINED
DEFINE "printf"          ; 声明绑定进模块作用域（调用点由 sema 解析）
LOAD_FUNCTION 65
PUSH_UNDEFINED
DEFINE "main"
HALT                     ; 拦截，函数体在下方
L0:                      ; ---- 函数体区 ----
PUSH_SCOPE
LOAD_TYPE 65             ; 压类型位，随后压 3 个成员值
PUSH_I32 1
PUSH_I32 2
PUSH_I32 3
CONSTRUCT 3              ; 值构造收尾 → [3]i32 数组
PUSH_UNDEFINED
DEFINE "a"
PUSH_STRING "%d\n"       ; 实参 1：格式串
PUSH "len"               ; 实参 2：len(a)（len 是内建，走 CALL）
PUSH "a"
CALL 1
EXTERN_CALL "libc::printf" 2 65792
                         ; ↑ STR 限定名 + U32 argc=2 + U32 打包(返回 void | 变参 | nfixed=1)
POP
POP_SCOPE
PUSH_UNDEFINED
RET
```

`examples/asm/fib.cxs` 片段（手写递归斐波那契）：

```
fib:
    push_undefined
    define "n"
    push_scope
    push "n"
    push_i32 2
    lt
    jz [rec]          ; 若 n < 2 不成立则跳到 rec
    ...
rec:
    push "fib"
    push "n"
    push_i32 1
    sub
    call 1            ; fib(n-1)
    ...
    add
    ret
```
