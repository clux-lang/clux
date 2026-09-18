# clux 字节码助记符（bytecode mnemonics）

clux 的 VM 以**字节码**作为可执行中间表示。编译期把 AST 降为字节码模块，运行期
由解释器直接执行。本文档是助记符的**单一事实源**（对应 `src/vm/bcode_asm_defs.c`
中的 `BCODE_ASM_TABLE`），反汇编器（`bcode_disasm.c`）与汇编器（`bcode_asm.c`）
共享同一张表，保证助记符与操作数布局完全一致。

---

## 1. 字节码模块布局

一个 `bytecode_t`（见 `include/vm/bcode.h`）只有两块产物：

1. **strtable（字符串表）**：编译期收集、运行期只读。包含变量名
   （`PUSH`/`STORE`/`DEFINE` 等的字符串索引）与字符串字面量（`PUSH_STRING` 的索引）。
2. **code（字节码流）**：`[opcode:u32][变长操作数...]` 的线性小端序列。

基本类型字面量（整数 / 浮点 / 布尔）全部**内嵌为立即数**，无独立常量池。
两块产物构成自包含可执行单元，可序列化落盘、重新加载直接执行。

---

## 2. 文本汇编格式（`.cxs`）

汇编文本（如 `examples/asm/*.cxs`）语法约定：

- **行注释**：`;` 起直到行尾。
- **标签定义**：`name:` 独占一行（如 `_start:`、`fib:`、`rec:`）。
- **标签引用**：`[name]`，用于跳转 / 函数入口操作数（如 `jz [rec]`、
  `push_function [main]`）。
- **字符串操作数**：`"..."`（C 风格转义），汇编时自动 intern 进 strtable 并写索引。
- **立即数**：按类型助记符书写，如 `push_i32 40`、`push_bool 0`、`push_string "hi\n"`。
- **助记符大小写**：本表以**全大写**登记（`ADD`、`STORE`…）；`.cxs` 示例使用小写
  （`add`、`store`…），汇编器大小写不敏感。

---

## 3. 操作数类型

| 类型 | 含义 |
|------|------|
| `NONE` | 无操作数。 |
| `STR` | strtable 索引；汇编文本内联 `"..."`，底层仍写 u32 索引。 |
| `U32` / `I8` / `I16` / `I32` / `I64` / `U8` / `U16` / `U64` / `F32` / `F64` / `BOOL` | 对应宽度的立即数，小端写入 / 读出。 |

---

## 4. 助记符总表

按功能分组；"操作数"列给出汇编文本中该指令后跟随的操作数。

### 4.1 取址 / 载值（栈：压入引用或值）

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH` | `STR` | 按名字在 scope 中查找并**借用引用**压栈。 |
| `STORE` | `STR` | 按名字 `value_assign(dst, pop())`，把栈顶值存入 dst，压回结果引用。 |
| `PUSH_STRING` | `STR` | 字符串字面量压栈。 |
| `PUSH_VALUE` | `U32` | 压入 `stack[sp-1-offset]` 的借用引用（深层取址）。 |
| `LOAD` | `STR` | 从 global scope 查 type value 压栈。 |
| `PUSH_UNDEFINED` | — | 压入 void 类型 value（"类型待推导"占位）。 |
| `PUSH_NIL` | — | 压入 nil 值（内置类型唯一值，data 为指针宽度零块 = NULL 指针；func/str 0 初始化 / 未来空指针）。 |

### 4.2 常量立即数

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_I8` / `PUSH_I16` / `PUSH_I32` / `PUSH_I64` | 对应有符号立即数 | 压入 i8/i16/i32/i64 立即数。 |
| `PUSH_U8` / `PUSH_U16` / `PUSH_U32` / `PUSH_U64` | 对应无符号立即数 | 压入 u8/u16/u32/u64 立即数。 |
| `PUSH_F32` / `PUSH_F64` | 对应浮点立即数 | 压入 f32/f64 立即数。 |
| `PUSH_BOOL` | `BOOL` | 压入 1 字节布尔立即数。 |

### 4.3 符号定义 / 类型构造

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `DEFINE` | `STR` | 弹栈定义（永远双弹 `[value, type-spec]`），绑定到名字。 |
| `PUSH_FUNC_TYPE` | — | 分配空**函数签名类型**（`func_type_t`）入 `vm->sig_types` 池，压其 type value（构造起点）。 |
| `FUNC_TYPE_PARAM` | — | 弹栈 type value → 追加为 func type 的下一参数类型。 |
| `FUNC_TYPE_RETURN` | — | 弹栈 type value → 设为 func type 的返回类型。 |
| `FUNC_TYPE_VARARG` | — | 标记 func type 为可变参数（无操作数）。 |
| `PUSH_ARRAY` | — | 分配空**数组类型**（`array_type_t`，处于开放态、暂不入池），压其 type value（构造起点）。 |
| `DEFINE_BOUND` | `U32`（长度立即数 N） | 弹栈顶元素 type value → 设为 array type 的元素类型，并把立即数 N 设为长度，构造出 `[elem; N]`。 |
| `PUSH_CONST` | — | 分配空 **const 限定类型**（`const_type_t`，开放态，sub=NULL，不入池），压其 type value（构造起点，供两遍扫描 pass 1 声明）。 |
| `PUSH_VOLATILE` | — | 分配空 **volatile 限定类型**（`volatile_type_t`，开放态，sub=NULL，不入池），压其 type value（构造起点，供两遍扫描 pass 1 声明）。 |
| `SET_TYPE` | — | 弹栈顶 type value（sub）→ 设为栈顶开放限定类型的 sub 字段（`type_qual_set_sub`，两遍扫描 pass 2 定义：`LOAD_TYPE <id>` 拉回开放对象 → `LOAD sub` → `SET_TYPE` 设 sub）。 |
| `DEFINE_TYPE` | `U32`（类型 id） | 弹栈顶 type value（**类型声明**：消费栈）→ 绑定程序 id（≥64）写入 `t->id` 并登记进 `vm->types_by_id`（幂等，多 id 别名同一类型）。声明后 `LOAD_TYPE <id>` 可拉回开放对象继续定义。 |
| `LOAD_TYPE` | `U32`（类型 id） | 从 `vm->types_by_id` 按 id 查类型并压其 type value。内建 id 0..16 预登记，程序类型 id ≥64 由 `DEFINE_TYPE <id>` 声明登记（开放构造阶段可拉回未密封对象，定义完成后 `SEAL` 密封重绑）。 |
| `LOAD_FUNCTION` | `U32`（函数 id） | 从 `vm->functions_by_id` 按 id 查函数并压其 func value（type = `fn->type` 签名，data = `func_t`）。函数值是编译期/运行期二元类型：运行期路径由本指令把真实函数值压栈（可作变量/参数/返回值传递、可调用），编译期路径由 sema 折叠为签名 shadow。内建函数（printf）创建时预登记 id（0 起），程序函数 id ≥64 由 `BIND_FUNC` 登记。与 `LOAD_TYPE` 对称：类型值/函数值都是编译期折叠 + 运行期查表加载。 |
| `SEAL` | —（无操作数） | 弹栈顶 type value（**类型定义收尾**：消费栈，类型不留栈）→ 经 `value_seal` 代理到 `data->vtable->type_seal`，按类型各自池查重 intern（首次成功入池并置基类 `sealed=true` + 布局计算；去重复用则回收开放类型并重定向栈引用）→ 密封前读开放对象自身 id（`DEFINE_TYPE` 已绑定，≥64），密封后按该 id 幂等更新登记（去重时重绑到新 intern 实例，避免登记表悬垂）。密封后的类型由后续 `LOAD_TYPE <id>` 主动拉取。 |
| `PUSH_FUNCTION` | `U32`（entry pc，标签） | 弹栈顶签名类型（`LOAD_TYPE` 拉取的 `SEAL` 产物）→ 构造 `bcode_function_t{entry_pc}`（`fn->id` 默认 0）→ func value 压栈（函数对象，非签名类型）。函数 id 由编译器分配（程序段 ≥ 64），运行时由 `BIND_FUNC` 填充 `fn->id` 并登记进 `vm->functions_by_id`。 |
| `BIND_FUNC` | `U32`（函数 id） | **peek** 栈顶 func value（不弹栈——注册段 `DEFINE` 需保留函数值）→ 填充 `fn->id = id` 并登记 `id → func` 进 `vm->functions_by_id`（幂等；程序函数 id ≥ `FUNC_ID_PROGRAM_BASE`）。id 单一来源——只在此出现一次。 |
| `SET_FUNC_NAME` | `STR` | **peek** 栈顶 func value（不弹栈）→ 拷贝函数显示名到 vm 堆（`fn->name`，`owns_name=true` 随对象释放）。仅命名函数定义写入；匿名函数表达式不写。 |
| `SET_CLOSURE` | `STR` | 弹栈顶捕获值 → clone 进**栈下**函数对象（`peek` 栈下 1 位，调用约定：定义点先 `MAKE_FUNCTION` 压函数值）的 `closure_scope` 捕获槽（scope 层 define-or-replace，`scope_set`）。闭包为 **clone 值语义**（捕获独立副本，非引用）。 |
| `MAKE_FUNCTION` | `U32`（函数 id） | 从 `vm->functions_by_id` 拉取**基底**函数对象 → 实例化**独立新实例**（共享基底 entry_pc/cfunc/type/id/name；新 `closure_scope` 按基底捕获槽名以 `PUSH_UNDEFINED` 占位）→ func value 压栈。函数定义（字面量/局部函数）每次求值生成新对象——循环内多个闭包捕获互不干扰（对标 `LOAD_FUNCTION` 的"同一对象"引用语义）。hoist 提升区构造的基底仅绑定地址；全局函数无捕获，基底即最终实例。 |

### 4.3.1 值构造与下标访问

`CONSTRUCT` / `INDEX_GET` / `INDEX_SET` 对应语言层的 `.<type>{...}` 值构造与
`a[i]` 下标读写（即 construct / set_item / get_item 协议）。三者均走 value 层
`vtable` 分派，越界/类型错误以**硬错误 value** 形式返回，由 `exec_drive` 捕获并停机。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CONSTRUCT` | `U32`（成员数量 N） | 收尾值构造：栈布局为 `…, type_value, v1 … vN`（类型在底、vN 在顶）。先逆序弹 N 个成员值，再弹类型位，按类型种类分派构造 value。当前仅实现 **array 分支**（`value_make_array`）。定长数组（len ≠ SIZE_MAX）允许**部分填充**：编译器对缺失元素补发 `PUSH_UNDEFINED` 占位，`value_make_array` 跳过 undefined 元素，剩余字节由分配块清零自动补**类型零值**（数值 0 / bool false / func nil / str NULL）；元素数超出声明长度（`N > len`）报错。非 array 类型暂报错，struct/tuple 待后续 Phase。 |
| `INDEX_GET` | — | `get_item`：`self[index]` → 元素副本。栈布局 `…, self, index`（index 在顶），弹 index、self 后分派 `vtable->get_index`。 |
| `INDEX_SET` | — | `set_item`：`self[index] = val` → 返回 self。栈布局 `…, self, index, val`（val 在顶），弹 val、index、self 后分派 `vtable->set_index`。 |

> **运行期越界检查**：数组 `INDEX_GET` / `INDEX_SET` 在执行时按数组值实际长度
> （运行时 `vec_len`）校验 `0 <= index < len`，越界（含负索引按无符号读入后
> `>= len`）一律返回硬错误 `array index N out of bounds (len=M)`，并停机。索引须
> 为整数类型，否则报 `array index must be an integer`。注意 `INDEX_GET` / `INDEX_SET`
> **消费** `self`（与 `CALL` 一致），多次访问需 `PUSH_VALUE` 复制引用。

### 4.3.2 长度查询

`LENGTH` 对应语言层的 `len(x)` / `.len` 查询，代理到 value 层的 `value_length`，
最终分派到类型 vtable 的 `length` 回调。

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `LENGTH` | — | 弹 `self`，压 `value_length(self)`（即 `vtable->length(self)`）。当前仅数组实现：返回 `u64` 元素个数（shadow 下返回 `u64` shadow）；不支持的类型返回硬错误。 |

### 4.4 算术 / 逻辑 / 位运算（均弹参、压结果，无操作数）

| 助记符 | 语义 | | 助记符 | 语义 |
|--------|------|-|--------|------|
| `ADD` | 加 | | `AND` | 逻辑与 / 位与 |
| `SUB` | 减 | | `OR` | 逻辑或 / 位或 |
| `MUL` | 乘 | | `BXOR` | 位异或 |
| `DIV` | 除 | | `SHL` | 左移 |
| `MOD` | 取模 | | `SHR` | 右移 |
| `EQ` | 相等 | | `NEG` | 取负（一元） |
| `NE` | 不等 | | `NOT` | 逻辑非（一元） |
| `LT` | 小于 | | `BNOT` | 位取反（一元） |
| `LE` | 小于等于 | | | |
| `GT` | 大于 | | | |
| `GE` | 大于等于 | | | |

> 注：助记符与 `bcode_op_t` 枚举一一对应（`BCODE_AND`→`AND`、`BCODE_OR`→`OR`、
> `BCODE_BXOR`→`BXOR`、`BCODE_SHL`→`SHL`、`BCODE_SHR`→`SHR` 等），详见
> `src/vm/bcode_asm_defs.c` 与 `include/vm/bcode.h`。

### 4.5 类型修饰 / 转换

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CAST` | — | 显式转换：类型经栈顶 type value（`LOAD` 压入），弹 type + 值后转换压回。 |
| `CREATE_CONST` | — | 弹 type value → `type_const_intern` → type value 压回（一次性快捷：开放构造+立即密封，不走两遍；编译器 hoist 区用 `PUSH_CONST`+`SET_TYPE`+`SEAL` 两遍路径，本指令保留为内联/防御分支）。 |
| `CREATE_VOLATILE` | — | 弹 type value → `type_volatile_intern` → type value 压回（同上，一次性快捷保留为内联/防御分支）。 |

### 4.6 调用 / 返回

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `CALL` | `U32`（argc） | `value_call`，被调用方在 `stack[sp-1-argc]`。 |
| `RET` | — | 返回 interrupt 哨兵，栈顶即返回值。 |

### 4.7 控制流（标签型操作数为 u32 pc）

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `JMP` | `U32`（pc，标签） | 绝对跳转到目标 pc。 |
| `JZ` | `U32`（pc，标签） | 弹引用，若 false / 0 则跳转。 |
| `JNZ` | `U32`（pc，标签） | 弹引用，若非 false / 0 则跳转。 |

### 4.8 作用域 / 栈管理

| 助记符 | 操作数 | 语义 |
|--------|--------|------|
| `PUSH_SCOPE` | — | `scope_new(alloc, current)` 压入新作用域。 |
| `POP_SCOPE` | — | 销毁当前作用域。 |
| `POP` | — | 丢弃栈顶引用（不释放，归 scope 管理）。 |
| `HALT` | — | 停止执行。 |

---

## 5. 示例

`examples/asm/fib.cxs` 片段（递归斐波那契）：

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

反汇编（经 `BCODE_ASM_TABLE` 还原）会将每条指令按其助记符与操作数类型还原为上述文本，
并把跳转 / 函数入口目标 pc 收集为 `L0`/`L1`/… 标签，保证可读性。
