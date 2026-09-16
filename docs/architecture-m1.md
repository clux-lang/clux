# clux 编译器架构 — M1

## 1. Pipeline

```
clux run <file.cx>
    │
    ▼
  Driver (cmd/run → driver)
  │
  ├─ ① 加载源文件 ──→ 内存 istream（失败 → 退出码 1）
  │
  ├─ ② Lexer ──→ Token 流
  │      │  词法错误：产出 TOKEN_TYPE_ERROR 并继续；致命性由上层决定（退出码 1）
  │
  ├─ ③ Parser ──→ AST（挂 arena）
  │      │  语法错误：panic mode 恢复，收集诊断（退出码 1，不进入 sema）
  │
  ├─ ④ Semantic Analysis (shadow value 驱动)
  │      │  先构建 sema 作用域树（scope 节点带完整符号表），
  │      │  再按作用域树用 shadow value 遍历 AST 做类型检查与推导。
  │      │  所有类型验证通过后才进入字节码编译。
  │      ├─ Pass 1: Name Collection — 收集所有顶层名称(函数名)
  │      ├─ Pass 2: Type Collection — 收集函数签名(参数类型+返回类型)
  │      ├─ Pass 3a: Scope Tree Construction — 每函数构建词法作用域树+符号表
  │      └─ Pass 3b: Shadow VM Run — 严格按作用域树遍历函数体(名称解析+类型检查+结果类型推导)
  │      │  语义错误：收集诊断（退出码 1）
  │
  ├─ ⑤ Bytecode Compiler ──→ 字节码模块（func/module 两粒度）
  │      │  AST 编译为线性字节码指令序列，类型已由 sema 验证，
  │      │  编译期计算（常量折叠）在表达式粒度编译→执行→嵌入。
  │      ├─ 函数级：函数体编译为字节码，延迟到运行时执行
  │      ├─ 表达式级：常量折叠、编译期计算（编译→执行→得到常量→嵌入上层）
  │      └─ 模块级：全局初始化代码编译为模块入口字节码
  │      │  编译错误：收集诊断（退出码 1）
  │
  ├─ ⑥ Bytecode VM ──→ 执行结果（退出码 0）
  │      栈式字节码执行器，PC 指针驱动，支持暂停/恢复。
  │      运行时不再做类型检查（已由 sema 完成），执行器更精简。
  │      运行时错误（如除零、空指针）→ 退出码 2
  │
  ▼
  诊断通道：所有阶段的诊断收进公共 diag 收集器，driver 统一打印到 stderr
```

错误与退出码契约：

| 错误类别 | 恢复策略 | 退出码 |
|----------|----------|--------|
| 词法错误（Lexer） | 产出 TOKEN_TYPE_ERROR 并继续；上层决定致命性 | 1 |
| 语法错误（Parser） | panic mode 跳到语句边界，可收集多条 | 1 |
| 语义错误（Sema） | 收集诊断，不编译不执行 | 1 |
| 运行时错误（Bytecode VM） | 立即终止 | 2 |

### 1.0 为什么需要 Driver

`cmd_run` 只负责参数解析，真正的流水线编排在独立 `driver` 模块（`include/driver/driver.h`, `src/driver/driver.c`）：

- `driver_compile_file()`：加载 + lex + parse + sema，产物是挂在 arena 上的 AST 与符号表
- `driver_run_file()`：compile + bytecode compile + execute，返回进程退出码
- 未来 `clux build`（转译）复用 compile + bytecode compile 段，`clux run` 复用全流程

### 1.1 为什么需要多遍扫描

clux 没有前置声明（forward declaration），函数定义顺序无关。以下代码合法：

```
func main():i32 {
    return add(1, 2);    // add 在 main 之后定义，但可以调用
}

func add(a:i32, b:i32):i32 {
    return a + b;
}
```

因此语义分析需要对顶层做三遍扫描：

| 遍数 | 名称           | 处理内容                                           |
|------|---------------|---------------------------------------------------|
| 1    | Name Collection | 遍历顶层节点，将所有顶层名称（函数名、类型名等）注册到全局作用域（仅名称，无签名/定义） |
| 2    | Type Collection | 遍历顶层节点，将函数签名（参数类型+返回类型）和类型定义的完整信息注册到全局作用域 |
| 3a   | Scope Tree Construction | 遍历每个函数体，构建词法作用域树（block 结构 → scope 树），每个 scope 节点注册完整符号表 |
| 3b   | Shadow VM Run | 按预建作用域树严格对应地遍历函数体，用 shadow value 执行类型检查与结果类型推导 |

三遍扫描的好处：
- 消除前置声明的需求，函数和类型定义顺序自由
- Pass 1 快速检测重复定义
- Pass 2 建立完整的类型信息，使 Pass 3 中的函数调用能正确匹配签名
- Pass 3 拆分为 3a/3b 两个子阶段：作用域结构与类型检查彻底分离。3a 只建结构（scope 树 + 符号表，不做类型检查），3b 按树遍历做类型检查（scope 管理与检查逻辑不纠缠）
- 后续里程碑加入 struct/union 等类型定义后，Pass 1 同样收集类型名，Pass 2 处理类型内部结构

## 2. 模块职责

### 2.1 Lexer (include/parser/lexer.h, src/parser/lexer.c)

输入源文件字符流，输出 Token 流。**Lexer 只负责切分 token，不解析值**：数值的大小、转义序列的解码留给 Parser（T3/T4 的 literal 解析），token 文本是对源 buffer 的零拷贝切片。Lexer 只校验字面量的**形状**（是否闭合、后缀是否合法、转义是否被允许、字符字面量是否恰好一个字符）。

- `lexer_create(allocator_t*, istream_t*, filename)` → `lexer_t*`
- `lexer_next(lexer_t*)` → `token_t*`（调用者所有，需 `token_free`）
- 支持所有 M1 关键字、运算符、字面量（数字含进制与类型后缀、C 风格字符/字符串字面量、行/块注释）
- 每个 Token 携带源码位置（文件/行/列）
- 标识符按 Unicode ID_Start / ID_Continue 判定（ICU `u_isIDStart` / `u_isIDPart`；`_` 作为特例允许开头）
- 跳过文件开头的 UTF-8 BOM；行注释止于 CR 或 LF，CRLF 的 `\r\n` 整体归为空白
- 字面量形状校验：数字后缀白名单、转义序列白名单（含 `\xHH` 的 1-2 位 hex）、字符字面量恰好一个字符且可表示为 u8

**Token 池（上层流水线负责）**：Lexer 只暴露 `lexer_next`，不内置 peek / checkpoint / rewind / 错误状态。上层按 `lexer_next` 把 token 灌入一个普通 `vec<token*>`（`owns_element=true`，随 `vec_free` 自动释放每个 token）；Parser 按索引随机访问该池，可自由前进或回退，无需重新词法分析、无前瞻缓冲。

**词法错误由上层处理（不 fail-fast）**：遇到无法识别字符、非法数字后缀、未闭合字符串/字符/块注释等，Lexer 产出**一个** `TOKEN_TYPE_ERROR` token（错误信息通过 `token_get_error_message` 携带于该 token 上），随后**继续**词法分析（错误输入已被消费）。Lexer 自身不记录错误、不因错误停止——是否就此终止完全是流水线的决定。典型策略：Parser 拉到 `TOKEN_TYPE_ERROR` → 用 `token_get_error_message` 记入诊断 → 置 fatal → 立即终止解析。

### 2.2 Token（定义在 include/parser/lexer.h，src/parser/lexer.c）

> `token_t` 为**不透明类型**（字段私有），无独立的 `token.h` / `token.c`。

```c
typedef enum {
    TOKEN_TYPE_ERROR,      // 词法错误：不可识别输入（消息由 token_get_error_message 携带，仅一个）
    TOKEN_TYPE_IDENTIFIER,
    TOKEN_TYPE_CHARACTER,  // 字符字面量 'a'（值类型 u8；文本含引号，转义原样）
    TOKEN_TYPE_STRING,     // 字符串字面量 "abc"（文本含引号，转义原样）
    TOKEN_TYPE_NUMERIC,    // 整数/浮点字面量，含进制与类型后缀（文本为原始切片）
    TOKEN_TYPE_KEYWORD,
    TOKEN_TYPE_SYMBOL,     // 运算符与标点（maximal munch，1-2 字符）
    TOKEN_TYPE_COMMENT,           // 行注释 // ...
    TOKEN_TYPE_MULTILINE_COMMENT, // 块注释 /* */（可嵌套）
    TOKEN_TYPE_WHITESPACE,        // 合并的空白
    TOKEN_TYPE_EOF,
} token_kind_t;

typedef struct {
    token_kind_t kind;
    location_t   loc;
} token_t;  // 仅示意：真实 token_t 为不透明类型，字段私有；
            // 文本/位置/错误消息经 token_get_text / token_get_location /
            // token_get_error_message 访问。
```

Token 文本语义约定：

| 种类 | token 文本内容 |
|------|----------------|
| NUMERIC | 原始切片，如 `42`、`0xFF`、`3.14e10f64`（含后缀） |
| STRING / CHARACTER | **含引号**的原始切片，如 `"ab\n"`、`'a'`；转义序列保持原样 |
| SYMBOL | 1-2 个字符的运算符/标点 |
| COMMENT / MULTILINE_COMMENT | 含注释标记，如 `// x`、`/* y */` |

### 2.3 Parser（include/parser/parser.h, src/parser/parser.c + src/parser/parse_*.c）—— 已实现（T4）

递归下降解析器，Token 流 → AST。

- `parser_create(allocator_t*, lexer_t*)` → `parser_t*`
- `parser_parse(parser_t*)` → `ast_node_t*`（程序根节点）
- 表达式解析用 Pratt parsing（绑定力表驱动）
- 语法错误恢复：panic mode（跳到语句边界 `;` `}` EOF）
- **词法错误处理**：Parser 遍历 token 池时遇到 `TOKEN_TYPE_ERROR` → 用 `token_get_error_message` 记入诊断 → 置 fatal 标志 → 立即终止解析（不做 panic recovery），driver 据此直接失败
- `parser_error(parser_t*)` → 是否已发生错误（语法或词法）

### 2.4 AST (include/parser/ast.h, src/parser/ast_node.c) —— 已实现

**结构范式：公共头 + 子类化**（chibicc / lcc 范式）。所有节点共享公共头，具体节点通过 kind 区分、按子类大小分配；AST 整体挂在一个 arena 上，随编译单元释放，不做逐节点 free。

**位置信息**：节点不嵌入 `location_t`（~56 字节），而是存储 token pool 下标（`tok_begin` / `tok_end`，共 8 字节）。诊断时通过 `vec_get(tokens, node->tok_begin)->location` 取得完整源码位置。

#### 2.4.1 公共头

```c
typedef struct ast_node {
    ast_kind_t       kind;        // 节点种类
    uint32_t         tok_begin;   // token pool 起始下标（inclusive）
    uint32_t         tok_end;     // token pool 结束下标（exclusive）
    struct ast_node *parent;      // 父节点（构建时填充）
    struct ast_node *next;        // 兄弟链（语句列表/参数/实参）
} ast_node_t;
```

- `tok_begin` / `tok_end` 是 Parser 持有的 token pool（`vec_t*`）中的下标
- 诊断时：`vec_get(tokens, node->tok_begin)` → `token_t*` → `token_get_location()` → `location_t`
- `uint32_t` 足够（单文件不可能超过 4G tokens）

#### 2.4.2 节点种类

```c
typedef enum {
    // --- 顶层 ---
    AST_PROGRAM,         // 函数定义列表
    AST_FUNC_DEF,        // func name(params):type { body }

    // --- 语句 ---
    AST_VAR_DEF,         // var name[:type] [= init];
    AST_ASSIGN,          // name = expr; / name += expr;
    AST_IF,              // if cond { then } [else { else_body }]
    AST_WHILE,           // while cond { body }
    AST_FOR,             // for (init; cond; update) { body }
    AST_RETURN,          // return [expr];
    AST_BREAK,           // break;
    AST_CONTINUE,        // continue;
    AST_BLOCK,           // { stmts... }
    AST_EXPR_STMT,       // expr;（表达式作为语句）
    /* AST_DISCARD removed: _ = expr is AST_ASSIGN, discard semantics in Sema */

    // --- 表达式 ---
    AST_BINARY,          // lhs op rhs
    AST_UNARY,           // op expr
    AST_CALL,            // name(args...)
    AST_INT_LIT,         // 整数字面量（原始文本切片，含后缀）
    AST_FLOAT_LIT,       // 浮点字面量（原始文本切片，含后缀）
    AST_BOOL_LIT,        // true / false
    AST_STRING_LIT,      // "..."（原始文本含引号，转义原样）
    AST_CHAR_LIT,        // 'a'（原始文本含引号，转义原样）
    AST_IDENT,           // 标识符引用
    AST_CAST,            // expr as type

    AST_ERROR,           // 解析错误恢复节点（记录错误位置，占位）

    AST_KIND_COUNT,      // 哨兵值，用于数组索引
} ast_kind_t;
```

#### 2.4.3 子类定义

**顶层节点**：

```c
typedef struct {
    ast_node_t  base;
    ast_node_t *funcs;       // 函数定义兄弟链首
    ast_node_t *funcs_last;  // O(1) 追加
} ast_program_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;        // 函数名
    ast_node_t *params;      // AST_VAR_DEF 兄弟链
    ast_node_t *params_last; // O(1) 追加
    strslice_t  return_type; // 返回类型文本（空切片 = void）
    ast_node_t *body;        // AST_BLOCK
} ast_func_def_t;
```

**语句节点**：

```c
typedef struct {
    ast_node_t  base;
    strslice_t  name;        // 变量名
    strslice_t  type_name;   // 类型标注（空切片 = 推断）
    ast_node_t *init;        // 初始化表达式（AST_UNDEF = 未初始化声明）
} ast_var_def_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;        // 赋值目标标识符
    int         op;          // '=' / '+=' / '-=' / '*=' / '/=' / '%='
    ast_node_t *value;       // 右值
} ast_assign_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *cond;
    ast_node_t *then_body;   // AST_BLOCK
    ast_node_t *else_body;   // AST_BLOCK 或 NULL
} ast_if_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *cond;
    ast_node_t *body;        // AST_BLOCK
} ast_while_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *init;        // AST_VAR_DEF / AST_ASSIGN / NULL
    ast_node_t *cond;        // 表达式 / NULL
    ast_node_t *update;      // AST_ASSIGN / NULL
    ast_node_t *body;        // AST_BLOCK
} ast_for_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *value;       // NULL = return;
} ast_return_t;

// AST_BREAK / AST_CONTINUE — 无额外字段

typedef struct {
    ast_node_t  base;
    ast_node_t *stmts;       // 语句兄弟链首
    ast_node_t *stmts_last;  // O(1) 追加
} ast_block_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *expr;
} ast_expr_stmt_t;
```

**表达式节点**：

```c
typedef struct {
    ast_node_t  base;
    int         op;          // 运算符（对应 token symbol 文本）
    ast_node_t *lhs;
    ast_node_t *rhs;
} ast_binary_t;

typedef struct {
    ast_node_t  base;
    int         op;          // '!' / '~' / '-'
    ast_node_t *operand;
} ast_unary_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;        // 函数名
    ast_node_t *args;        // 实参兄弟链
    ast_node_t *args_last;   // O(1) 追加
} ast_call_t;

typedef struct {
    ast_node_t  base;
    strslice_t  text;        // 原始切片（含后缀，如 "42u64"）
} ast_int_lit_t;

typedef struct {
    ast_node_t  base;
    strslice_t  text;        // 原始切片（含后缀，如 "3.14f32"）
} ast_float_lit_t;

typedef struct {
    ast_node_t  base;
    bool        value;
} ast_bool_lit_t;

typedef struct {
    ast_node_t  base;
    strslice_t  text;        // 含引号的原始切片
} ast_string_lit_t;

typedef struct {
    ast_node_t  base;
    strslice_t  text;        // 含引号的原始切片
} ast_char_lit_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;        // 标识符文本
} ast_ident_t;

typedef struct {
    ast_node_t  base;
    ast_node_t *expr;
    strslice_t  target_type; // 目标类型文本
} ast_cast_t;

typedef struct {
    ast_node_t  base;
    strslice_t  message;     // 错误描述信息
    ast_node_t *node;        // 发生错误的子节点（可为 NULL）
} ast_error_t;
```

#### 2.4.4 工厂与辅助

```c
// 根据 kind 分配正确大小的子类，填充 kind + tok_begin/tok_end
ast_node_t *ast_new(arena_t *arena, ast_kind_t kind,
                    uint32_t tok_begin, uint32_t tok_end);

// 追加 node 到 *head / *last 兄弟链尾，设置 node->parent
void ast_append(ast_node_t **head, ast_node_t **last,
                ast_node_t *parent, ast_node_t *node);

// 查询
const char *ast_kind_name(ast_kind_t kind);
size_t      ast_kind_size(ast_kind_t kind);
```

#### 2.4.5 设计约束

1. **禁止** 在 AST 节点中嵌入 `location_t`——位置通过 token 下标间接访问
2. **禁止** 逐节点 free——AST 挂 arena，整体释放
3. **禁止** 在 Parser 阶段解码字面量值——保留原始 `strslice_t` 文本，解码是 Sema 的事
4. **禁止** 在 `ast_node_t` 中放 `last` 指针——尾指针属于容器节点（block/func_def/call/program）
5. 赋值节点 (AST_ASSIGN) 返回 void，不允许连续赋值，不能作为表达式嵌套
6. `AST_BREAK` / `AST_CONTINUE` 无子类结构，仅需 kind + tok range
7. `AST_ERROR` 是完整的错误报告节点，含 `message`（错误描述）和 `node`（发生错误的子节点范围），用于 panic mode 恢复时占位及后续诊断

### 2.5 VM 核心 (include/vm/*.h, src/vm/*.c) —— 已实现

VM 是 clux 的值计算引擎，贯穿语义分析和字节码执行两个阶段。M1 阶段 VM 的定位是**表达式求值器和作用域/变量生命周期管理工具**。

#### 2.5.1 类型系统 (include/vm/type.h, src/vm/type.c)

类型不是独立的 sema 子系统，而是 VM 的一部分。每个类型携带 vtable（虚表），类型行为通过 vtable 函数指针分派。

```c
typedef struct type {
    const char       *name;
    uint64_t          size;
    uint64_t          align;
    const vtable_t   *vtable;
} type_t;
```

基础类型单例：`type_i8()` … `type_i64()`, `type_u8()` … `type_u64()`, `type_f32()`, `type_f64()`, `type_bool()`, `type_str()`, `type_void()`。

每个类型通过 vtable 暴露行为：

```c
typedef struct vtable {
    /* 生命周期 */
    void    (*dispose)(vm_t *vm, value_t *v);
    value_t *(*clone)(vm_t *vm, value_t *v);
    value_t *(*assign)(vm_t *vm, value_t *dst, value_t *src);
    /* 类型转换 */
    value_t *(*implicit_cast)(vm_t *vm, value_t *v, const type_t *target);
    value_t *(*explicit_cast)(vm_t *vm, value_t *v, const type_t *target);
    /* 二元运算 */
    value_t *(*binary)(vm_t *vm, int op, value_t *lhs, value_t *rhs);
    /* 一元运算 */
    value_t *(*unary)(vm_t *vm, int op, value_t *v);
} vtable_t;
```

- `assign`：原地赋值，向左值类型 implicit_cast + memcpy，不涉及类型协商/promote
- `implicit_cast`：安全隐式转换（如 i8→i32 宽化）
- `explicit_cast`：显式转换（`as` 运算符），允许窄化
- `binary`：二元运算，内部经 promote → implicit_cast → safe_cast 协商后执行
- vtable 槽为 NULL 表示该类型不支持此操作，调用时返回 error

类型实现各自独立文件：`type_int.c`（signed/unsigned 共享 assign）、`type_float.c`、`type_bool.c`、`type_str.c`、`type_func.c`、`type_error.c`、`type_void.c`、`type_type.c`。

#### 2.5.2 值 (include/vm/value.h, src/vm/value.c)

`value_t` 为**不透明类型**，`struct value_t` 定义仅在 `value.c` 中，外部通过访问器操作：

```c
const type_t *value_type(const value_t *v);
void         *value_data(const value_t *v);
```

值使用 `void *data` 指向按 `type->size` 分配的堆内存（非 union），支持任意宽度类型。VM 全局持有所有 type 和 function 对象。

#### 2.5.3 作用域 (include/vm/scope.h, src/vm/scope.c)

统一所有权模型：`vars` 做名称→值的借用映射，`owned` 向量管理值生命周期。

- `scope_define(vm, scope, name, v)`：定义变量，重复定义返回 error
- `scope_lookup(scope, name)`：查找变量（遍历父链）
- `scope_push(vm, parent)` / `scope_pop(vm, scope)`：进入/退出作用域
- `value_clone` / `value_make` 自动 track 到 `vm->current_scope->owned`
- 退出作用域时统一释放 owned 值，禁止手动 dispose

#### 2.5.4 函数 (include/vm/function.h, src/vm/function.c)

`func_t` 为透明类型（后续需继承），支持 FFI 和用户函数：

```c
struct func_t {
    cfunc_t         cfunc;          /* C 函数指针（FFI） */
    scope_t        *closure_scope;
    scope_t        *root_scope;
    const type_t  **params;
    size_t          param_count;
    const type_t   *return_type;
    strslice_t      name;
    bool            is_variadic;    /* FFI 可变参数（如 printf） */
};
```

- `func_vcall`：函数调用，非 variadic 函数检查参数数量，variadic 函数允许 `argc > param_count`
- 短路运算符 `&&` / `||` 不走 vtable binary 分派，在调用层按 op token 做惰性求值

### 2.6 Shadow Value (include/vm/value.h) —— 已实现（2026-09-08）

语义分析阶段使用 shadow value 做类型检查与推导。Shadow value 复用 VM vtable 运算路径，不引入独立的 sema 类型系统。

**核心机制：**

```c
struct value_t {
    const type_t *type;
    void         *data;       /* shadow 时为 NULL */
    bool          is_shadow;  /* true = 只做类型计算，无实际数据 */
    /* ... */
};
```

- `is_shadow=true` 时 `data=NULL`，所有运算只进行类型计算不操作实际数据
- `value_make_shadow(vm, type)` 构造器：分配 value_t，设 type，data=NULL，is_shadow=true，auto-track 到当前 scope
- `value_is_shadow(v)` 访问器（value_t 为不透明类型，外部经访问器判断）
- shadow 标志放在 `value_t` 内部（非 type_t 层面）
- clone 槽显式实现（int/float/bool/str/type 各有 clone），value_clone 对 shadow 直接返回新 shadow；NULL clone 槽 = error

**传播规则：**

| 运算 | 结果 |
|------|------|
| shadow ⊕ normal | shadow（结果只有类型，无数据） |
| shadow ⊕ shadow | shadow |
| assign/cast 对 shadow | 只检查类型兼容性，不拷贝数据 |
| clone 对 shadow | 返回新的 shadow（不分配 data） |
| dispose 对 shadow | 跳过 data 释放（data==NULL） |

**传播位置：** 在 **vtable 函数内部**（非 DISPATCH 宏层）。Shadow value 需要经历完整的类型协商流程（VTABLE_BINARY 的 promote、implicit_cast、safe_cast），在类型检查/协商完成之后、实际读写 data 之前检查 `is_shadow`。如果是 shadow 则用结果类型构造 shadow 返回值，不分配/拷贝 data。类型协商错误（如 i32 + f64 禁止隐式）由 vtable 返回 error value，sema 捕获后转为诊断。

**执行流水线中的位置：**

```
源码 → AST → 语义分析（shadow value 类型检查/推导）→ 字节码编译 → 字节码执行
```

- 每次执行脚本都先走语义分析，不是可选的
- 语义分析用 shadow value 遍历 AST，验证类型合法性、推导结果类型、检查变量定义
- 全部通过后才编译为字节码执行
- 运行时不再做类型检查，字节码执行器更精简

### 2.7 字节码 IR —— 已实现（bytecode 容器 + 执行器 + 编译器全链路）

放弃 AST 直走解释，改为编译 AST 到线性字节码后执行。

**核心动机：**
- AST 直走解释无法做到暂停/恢复（如 generator、async/await、debugger 断点）
- 线性字节码是状态机，可以保存 PC 指针随时暂停恢复
- 编译期计算需要复用同一套执行引擎

**编译流程三粒度分级：**

| 粒度 | 编译时机 | 用途 |
|------|----------|------|
| 表达式级 | 编译过程中 | 常量折叠、类型推导等编译期计算 |
| 函数级 | 延迟到运行时 | 函数体编译为字节码 |
| 模块级 | 模块加载时 | 全局初始化、模块顶层代码 |

为什么需要分级：编译期计算意味着编译过程中需要执行部分字节码。表达式级编译 → 执行 → 得到常量结果 → 嵌入上层字节码。函数级编译 → 延迟到运行时执行。模块级编译 → 模块加载时执行全局初始化代码。

**与现有 VM 的关系：**
- 字节码执行时仍可通过 vtable 分派类型运算，或编译时静态分派（类型已由 sema 确定）
- shadow value 解决类型层面编译期计算，字节码解决值层面执行和运行时
- 两者互补：shadow value 先做（语义分析基础设施），字节码 IR 后做（大工程）

#### 2.7.1 指令编码：扁平字节流 + 回调驱动 PC

字节码是**字节流**，非定长结构体数组：

```
[opcode: u32][变长操作数...]
```

- **执行器只移动 opcode 部分**：读 u32 opcode → `pc += 4` → 按 opcode 分派回调。
- **操作数由指令回调自己消费、自己推进 pc**（只有指令自己清楚自己有几个操作数），执行器不感知任何指令语义。
- 指令天然变长；新增指令只需注册回调，执行器零改动。

**操作数读取（read 函数族）**：指令回调按操作数类型从字节流读对应宽度的**立即数**，读多少由指令自己决定：

| read 函数 | 宽度 | 用途 |
|-----------|------|------|
| `read_u8` / `read_i8` | 1 字节 | 小整数、类型索引等 |
| `read_u16` / `read_i16` | 2 字节 | 中整数 |
| `read_u32` / `read_i32` | 4 字节 | 大整数、strtable 索引、目标 pc |
| `read_u64` / `read_i64` | 8 字节 | 64 位整数立即数 |
| `read_f32` / `read_f64` | 4 / 8 字节 | 浮点立即数 |
| `read_bool` | 1 字节 | 布尔立即数 |

基本类型字面量全部**直接内嵌为立即数**（数字、布尔按宽度读），**常量池只存放字符串**（变长，无法直接内嵌）。

示例 `0[push]4[0]8`（push 变量指令，操作数为 strtable 索引）：

```
pc=0: exec 读 u32 → opcode=push        pc += 4 → pc=4
      dispatch → op_push 回调
op_push: read_u32(bc, 4) → 0           *pc += 4 → pc=8
      压入 scope_lookup(strtable[0]) 的借用引用
返回 exec：pc=8，继续读下一条 opcode
```

#### 2.7.2 操作数栈 = 借用引用

- **value 本体归 scope 管理**（现有 scope 模型不动：`scope_define` 存入、`scope_destroy` 释放）。
- 操作数栈只放 **`value_t*` 借用引用**：`PUSH` 压入 scope 里那个 value 的指针，**不 clone、不转移所有权**。
- 运算结果由 vm API 新建并自动 track 到当前 scope，栈再压入它的借用引用。
- **执行器不释放任何 value**：它只是栈指针 + PC 驱动器，所有 value 生命周期由 scope 统一管理。

#### 2.7.3 产物结构：字符串表 + 字节码流

字节码编译结束的产物是**两块**：

1. **字符串表（strtable）**：编译期收集的字符串，运行时只读。包含两类字符串：
   - **变量名**：`PUSH`/`STORE` 指令参数 = 变量名字符串表索引（运行时 `scope_lookup` 按名查找）
   - **字符串字面量**：`PUSH_STR` 指令参数 = 字符串字面量索引（压入借用引用）
2. **字节码流（code）**：`[opcode: u32][变长操作数...]` 线性序列。操作数全部是平凡类型——**基本类型字面量直接内嵌为立即数**（见 2.7.1 read 函数族），索引类操作数（strtable 索引、目标 pc、argc）也是平凡整数。

产物**只有这两块**，无独立的"常量池 value 数组"：整数字面量、浮点字面量、布尔字面量全部内嵌在字节码流中由 read 函数读取；唯一的例外是字符串（变长，无法内嵌），收进字符串表。

**字节码自包含、可落盘重载**：strtable + code 流构成**完整可执行单元**，不含任何指向 AST / sema 符号表 / 函数表等中间产物的引用——`bcode_function_t` 由 `PUSH_FUNCTION` 在**执行时**用内嵌入口 pc 构造，函数值随 scope 走，无编译期预建表。编译完成后前面所有流程的中间产物（AST、符号表等）**可以移除**；字节码可序列化落盘（strtable 字符串数组 + code 字节数组 + 指令表），之后**重新加载直接执行**。

#### 2.7.4 执行器核心循环

```c
void exec_run(exec_t *e) {
    while (!e->halted) {
        uint32_t op = read_u32(e->bc, e->pc);
        e->pc += 4;                        /* exec 只移 opcode */
        value_t *r = e->bc->handlers[op](e, e->bc, &e->pc); /* 回调移操作数 */
        if (r) stack_push(e, r);           /* 结果压栈（借用引用） */
        if (value_is_interrupt(r)) {       /* 引擎级控制流信号（RET 等） */
            exec_handle_interrupt(e, r);   /* 消费点：bcode_call_cfunc 子循环捕获 */
        }
        if (value_is_error(e->vm, r)) {    /* 引擎级硬错误 → 停止 */
            e->halted = true;
            e->result = r;                 /* 错误传播到调用方 */
        }
    }
}
```

错误检查在执行器统一出口（error 是引擎级硬错误，所有 value 操作后必须检查向外传播）；interrupt 哨兵（引擎级控制流信号，目前仅 RETURN）也在统一出口识别处理；指令语义（add/store/call 各自干什么）完全在回调内。

#### 2.7.5 指令集

| 指令 | 参数 | 回调语义 | 使用 vm API |
|------|------|----------|-------------|
| `PUSH` | strtable 索引 | `scope_lookup` 按名查，借用引用压栈 | `scope_lookup` |
| `STORE` | strtable 索引 | `dst = lookup(name); src = stack.pop(); return value_assign(dst, src)` —— `=` 是普通操作符号，执行器不感知赋值 | `scope_lookup` + `value_assign` |
| `PUSH_STR` | 字符串表索引 | 压入字符串字面量借用引用 | — |
| `PUSH_I8 I16 I32 I64` | 对应宽度立即数 | `read_iN` 读立即数 → 构造 int value 压栈 | `value_make_int` |
| `PUSH_U8 U16 U32 U64` | 对应宽度立即数 | `read_uN` 读立即数 → 构造 uint value 压栈 | `value_make_uint` |
| `PUSH_F32 F64` | 对应宽度立即数 | `read_fN` 读立即数 → 构造 float value 压栈 | `value_make_float` |
| `PUSH_BOOL` | 1 字节立即数 | `read_bool` → 构造 bool value 压栈 | `value_make_bool` |
| `PUSH_VALUE` | offset | 压入 `stack[sp-1-offset]` 的借用引用（offset=0 即 dup 栈顶一份） | — |
| `LOAD` | strtable 索引 | 从 global scope 按名查 **type value** 压栈（类型注册见 2.7.5 下注） | `scope_lookup` |
| `PUSH_UNDEFINED` | — | 压入 void 类型 value，标记"类型待推导" | `value_make_void` |
| `DEFINE` | strtable 索引 | 弹栈定义变量：**栈顶为 type value（`LOAD` 压入）或 void/undefined（`PUSH_UNDEFINED` 压入）时作为类型说明符再弹一个值；否则栈顶即值本身（函数参数定义场景）**；无初始值（值为 void）以声明类型**零值占位**定义（TDZ 检查由 sema 编译期完成） | `scope_define` |
| `PUSH_FUNC_TYPE` | — | 分配空 `func_type_t`（开放类型，暂不入池）并压其 type value 到操作数栈，作为签名构造起点（见 2.7.6）。`sealed` 标志已上移至基类 `type_t`，构造期（seal 前）可分步 set | `func_type_push` |
| `FUNC_TYPE_PARAM` | — | 弹栈 type value → 追加为栈顶 func type 的下一参数类型（`func_type_add_param`） | `func_type_add_param` |
| `FUNC_TYPE_RETURN` | — | 弹栈 type value → 设为栈顶 func type 的返回类型（`func_type_set_return`） | `func_type_set_return` |
| `FUNC_TYPE_VARARG` | — | 标记栈顶 func type 为可变参数（`func_type_set_variadic`，M1 无用户变参函数省略） | `func_type_set_variadic` |
| `DEFINE_TYPE` | 类型 id | 弹栈顶 type value（**类型声明**：消费栈）→ 绑定程序 id（≥64）写入 `t->id` 并登记进 `vm->types_by_id`（幂等，多 id 别名同一类型）。声明后 `LOAD_TYPE <id>` 可拉回开放对象继续定义 | `vm_type_bind` |
| `SEAL` | —（无操作数） | 弹栈顶 type value（**类型定义收尾**：消费栈，类型不留栈）→ 经统一 `value_seal` 代理到 `func_type_seal`：计算规范名 `func(...)`、按签名查重 intern（首次成功才 `vec_push(vm->sig_types, ...)` 入池并置 `sealed=true` + 布局计算）；若已有完全一致的实现则手工回收当前开放类型并改写操作数栈中对它的引用为缓存类型（无悬空、零泄漏）→ 密封前读开放对象自身 id（`DEFINE_TYPE` 已绑定，≥64），密封后按该 id 幂等更新登记（去重时重绑到新 intern 实例，避免登记表悬垂）。密封后的类型由后续 `LOAD_TYPE <id>` 主动拉取。`type_func_sig` 仍保留为 C 侧一次性快捷（不向 VM 栈压 type value） | `value_seal` → `func_type_seal` / `type_func_sig` |
| `LOAD_TYPE` | 类型 id | 从 `vm->types_by_id` 按 id 查类型并压其 type value（`DEFINE_TYPE <id>` 声明登记的产物；开放构造阶段可拉回未密封对象继续定义）。内建 id 0..16 预登记 | — |
| `PUSH_FUNCTION` | 入口 pc | 读入口 pc 立即数 → **`bcode_function_new` 构造 `bcode_function_t{entry_pc}`**（自封装：建孤立 closure_scope + 注册 vm->functions 池，`fn->id` 默认 0）→ 弹栈顶签名类型（`LOAD_TYPE` 拉取的 `SEAL` 产物）→ 组装 **func value** 压栈（函数定义模板见 2.7.6）。函数 id 由编译器分配（程序段 ≥ 64），运行时 `BIND_FUNC` 填充并登记 | `bcode_function_new` |
| `BIND_FUNC` | 函数 id | **peek** 栈顶 func value（不弹栈——注册段 `DEFINE` 需保留函数值）→ 填充 `fn->id = id` 并登记 `id → func` 进 `vm->functions_by_id`（幂等）。id 单一来源——只在此出现一次 | `vm_func_bind` |
| `SET_FUNC_NAME` | strtable 索引 | **peek** 栈顶 func value（不弹栈）→ 拷贝函数显示名到 vm 堆（`fn->name`，`owns_name=true` 随对象释放）。仅命名函数定义写入；匿名函数表达式不写 | — |
| `ADD SUB MUL DIV MOD` | — | 弹两引用 → 运算 → 压结果引用 | `value_add` 等 |
| `EQ NE LT LE GT GE` | — | 同上 | `value_eq` 等 |
| `AND OR` | — | 同上 | `value_band` 等 |
| `BXOR SHL SHR` | — | 同上 | `value_bxor`/`value_shl`/`value_shr` |
| `NEG NOT BNOT` | — | 弹一引用 → 一元运算 → 压结果 | `value_neg`/`value_lnot`/`value_bnot` |
| `CAST` | —（无操作数） | 类型经**栈顶 type value**（`LOAD` 压入）传递：弹 type value → 弹被转换值 → `value_explicit_cast(vm, value, *(const type_t**)data)` | `value_explicit_cast` |
| `CREATE_CONST` | — | 弹栈顶 **type value** → `type_const_intern` intern（按 sub 去重）→ 构造 const type value 压回（`const i32 != i32`，真实类型） | `type_const_intern` |
| `CREATE_VOLATILE` | — | 弹栈顶 **type value** → `type_volatile_intern` intern（按 sub 去重）→ 构造 volatile type value 压回（固定组合顺序 `volatile(const(T))`） | `type_volatile_intern` |
| `CALL` | argc | callee 在 `stack[sp-1-argc]`、实参 `args=&stack[sp-argc]`（调用点先 `PUSH "name"`）→ **args 复制进 VLA**（`value_call` 内压栈可能 realloc 使栈缓冲悬垂）→ 清理 callee+实参 → **`value_call(vm, callee, args, argc)`**（scope/frame 由 `func_vcall` + `bcode_call_cfunc` 完成，见 2.7.6）→ 结果压栈 | `value_call` |
| `RET` | — | 栈顶即返回值，返回 **interrupt 哨兵**（`INTERRUPT_RETURN`），由 `bcode_call_cfunc` 子循环捕获 | `value_make_interrupt` |
| `JMP` | 目标 pc | `*pc = read_u32(...)`（绝对字节偏移） | — |
| `JZ JNZ` | 目标 pc | 弹引用 → **`value_explicit_cast(v, bool)`**（失败/TDZ 返回 error）→ 读 `*(bool*)data` 判断跳转（clux 严格 bool，无 truthy 概念） | `value_explicit_cast` |
| `PUSH_SCOPE` | — | `scope_new(alloc, current)` 压入 | `scope_new` |
| `POP_SCOPE` | — | 弹出并销毁当前 scope（回收本块全部临时值） | `scope_destroy` |
| `POP` | — | 丢弃栈顶引用（不释放，归 scope） | — |
| `HALT` | — | 停止执行 | — |

运算中间结果由 current_scope **匿名绑定**（`value_make`/vtable 自动 track），块退出 `POP_SCOPE` 一次性回收。

**变量定义编译模板**（`DEFINE` 弹栈约定：栈顶为类型说明符（type value / void value）则再弹一个作为值；否则栈顶即值）：

```
var a:i32 = 1;      =>  PUSH_I32 1;  LOAD "i32";  DEFINE "a";
var b = 1;          =>  PUSH_I32 1;  PUSH_UNDEFINED;  DEFINE "b";
var c:i32;          =>  PUSH_UNDEFINED;  LOAD "i32";  DEFINE "c";   /* 无初始值 = undefined */
```

- **undefined 视为 void 类型的变量**：不引入新类型，`PUSH_UNDEFINED` 构造 type_void 的 value，作为"类型待推导 / 未初始化声明"标记。`DEFINE` 遇 void 类型说明符时从初始值推断实际类型；值为 void（`undefined` 初始化）则变量以**声明类型零值**占位定义（运行时无未初始化状态，读取/参与运算均合法——是否可读由 sema 确定性赋值分析在编译期裁决）。
- **TDZ（确定性赋值分析）**：未初始化检查是 **sema 编译期数据流分析**（符号表 `flow_init` 字段 + if 合并点 meet 语义），VM 值层完全不感知。`var x:i32 = undefined` 定义时 VM 存零值占位，读取报"used before initialization"是编译错误而非运行时错误。
- **基本类型注册 global scope**：VM 初始化（`vm_new`）时把 i8..i64/u8..u64/f32/f64/bool/str/void/type/func 等基本类型以 **type value** 注册进 global scope（复用 `g_type_type`，VTABLE_TYPE，data 存 `const type_t*`，见 vm.c `vm_register_builtin_types`），`LOAD "i32"` 即按名查出的 type value。

#### 2.7.6 函数调用与 interrupt 哨兵

```
CALL argc 执行（回调内）：
  callee = stack[sp-1-argc];  args = &stack[sp-argc]   -- 调用点先 PUSH "name" 再压实参
  清理栈上 callee+实参（截断到 sp-argc-1）
  r = value_call(vm, callee, args, argc)               -- 唯一调用入口，scope/frame 全在回调内
  if (r) 压栈

RET 执行（回调内）：
  返回值引用保留在栈顶（return 指令前已就位）
  返回 interrupt 哨兵（类型 INTERRUPT_RETURN），由 bcode_call_cfunc 的执行循环捕获
```

**scope/frame 处理交由基类回调，CALL 指令只调 `value_call`**。`bcode_function_t` 继承 `func_t`（基类）：

```c
typedef struct bcode_function_t {
    func_t   base;      /* 基类：cfunc（= bcode_call_cfunc）、closure_scope、root_scope、name */
    uint32_t entry_pc;  /* 字节码入口偏移 */
} bcode_function_t;
```

`value_call` → vtable 分派 `func_vcall`（src/vm/type_func.c），**通用调用流程全部复用**：

1. 保存现场（caller_root / caller_scope）
2. 切函数 root_scope；**临时接线孤立 closure_scope → root_scope**（函数体可查看到模块变量），进入 closure_scope
3. push 函数体匿名局部作用域
4. 参数 safe_cast + clone 到 local_args（类型兜底，sema 已静态校验）
5. 调 `base.cfunc`（= `bcode_call_cfunc`）
6. 返回值 safe_cast + clone 到调用方作用域
7. 平衡作用域栈（正常 pop / error 砍子树）
8. 恢复现场（含 closure_scope 临时 parent 接线恢复）

**`bcode_call_cfunc`（bcode_function_t 的执行回调，src/vm/bcode_function.c）**：

```
bcode_call_cfunc(vm, fn, argc, local_args):
  local_args 按序压操作数栈（压栈顺序 a, b → 栈顶是 b）
  保存 vm->pc / vm->halted；vm->halted = false
  r = exec_drive(vm, vm->bc, entry_pc)   -- 与主循环共用的指令驱动循环
  恢复 vm->pc / vm->halted
  RET 哨兵：弹哨兵，栈顶即返回值（借用引用，由 func_vcall clone 回 caller）
  error：弹 error 引用后传播（func_vcall 走 error 平衡路径）
  函数体走完未 RET（编译错误）：无返回值 → NULL
```

**interrupt 哨兵消费点移到 bcode_call_cfunc 子循环**：函数调用被 `func_vcall` 包裹成同步调用，`RET` 只返回哨兵（`value_make_interrupt(INTERRUPT_RETURN)`），由执行回调的循环捕获取返回值；弹帧/恢复现场由 `func_vcall` 的保存/恢复逻辑完成，不再需要独立 frame 栈。`exec_drive`（主循环与函数子循环共用）统一出口：error / interrupt 直接返回（结果已压操作数栈）。未来 BREAK/CONTINUE 哨兵同理由执行回调捕获。

**调用模型统一（FFI / vm 预注册 / 手工注册 / 字节码函数）**：`CALL` 指令永远走 `value_call` → `func_vcall`，通用流程（作用域切换、参数 clone、返回 clone、错误平衡）对**所有**函数实现共享：

- **FFI 函数**（C 函数指针，可变参数如 printf）：`func_t.cfunc` 直调，is_variadic 签名
- **vm 预注册 / clux 手工注册的函数**：注册时构造 `func_t`（cfunc = 用户回调）挂到 global scope，调用点 `PUSH "name"` 取函数值 → `CALL` 走同一 `value_call`
- **字节码函数**：`bcode_function_t`（继承 func_t）的 `base.cfunc = bcode_call_cfunc`，驱动字节码体

调用点无需区分函数来源——只要函数值是 `func_t` 体系（data 指向 func_t 或其子类），`value_call` 即正确分派。这也是 `CALL` 只保留 `value_call` 一个入口的根本原因。

**函数定义编译模板**（产物布局：类型提升区 → 函数注册段 → `HALT` → 函数体区；函数体在 `HALT` 之后，不顺序执行，只经 `PUSH_FUNCTION` 记录的入口 pc 进入；`L_FUNC_START` 为地址标签，编译期回填绝对 pc）：

```
func add(a:i32, b:i32):i32 { return a + b; }
func main():void { }

; ---- 类型提升区（hoist，产物最前）----
; pass 1 声明所有类型（含函数签名）：PUSH_FUNC_TYPE 创建开放对象 → DEFINE_TYPE 登记
  PUSH_FUNC_TYPE                  ; 分配空 func type + 压其 type value（开放对象）
  DEFINE_TYPE 64                  ; 声明：弹栈顶签名类型 → 绑定 id 64 + 登记进 types_by_id（此后可 LOAD_TYPE 拉回）
  PUSH_FUNC_TYPE                  ; main 签名（开放对象）
  DEFINE_TYPE 65                  ; 声明登记 id 65
; pass 2 定义所有类型（依赖后序）：LOAD_TYPE 拉回 → 设字段 → SEAL 封闭
  LOAD_TYPE 64                    ; 拉回 add 签名开放对象（定义起点）
  LOAD "i32"                      ; 参数 a 类型
  FUNC_TYPE_PARAM                 ; 追加为参数
  LOAD "i32"                      ; 参数 b 类型
  FUNC_TYPE_PARAM                 ; 追加为参数
  LOAD "i32"                      ; 返回值类型
  FUNC_TYPE_RETURN                ; 设为返回类型
  SEAL                            ; 定义收尾：弹栈顶签名类型 → value_seal 代理 func_type_seal，去重 intern 入池并置 sealed + 布局计算，密封后按自身 id 64 重绑登记
  LOAD_TYPE 65                    ; 拉回 main 签名开放对象
  LOAD "void"
  FUNC_TYPE_RETURN
  SEAL                            ; 密封重绑 id 65
; ---- 函数注册段（hoist 区之后）----
L_FUNC_START:                     ; = bcode_function_t.entry_pc（函数体区）
  DEFINE "b"                      ; 弹栈顶实参定义参数（倒序：后压先弹）
  DEFINE "a"
  ...函数体...
  RET
  ...
  HALT                            ; 注册段之后停机，拦截落入函数体区
  ; ---- 函数体区（产物最后，各函数体以 RET 结尾） ----
  LOAD_TYPE 64                    ; 主动从 types_by_id 拉回密封签名类型压栈（签名已由 hoist 区构造，注册段不内联构造）
  PUSH_FUNCTION L_FUNC_START     ; 构造 bcode_function_t{entry_pc}（fn->id 默认 0）+ 弹栈顶签名类型 → func value
  BIND_FUNC 64                   ; peek 栈顶填充 fn->id=64 + 登记 id→func 进 functions_by_id（不弹栈）
  SET_FUNC_NAME "add"            ; peek 栈顶写入函数显示名（不弹栈）
  PUSH_UNDEFINED                 ; 函数定义无类型说明符 → push_undefined
  DEFINE "add"                   ; 单弹 value（函数名固定，绑定到名字）
  LOAD_TYPE 65                   ; main 签名（hoist 区已密封）
  PUSH_FUNCTION ...main_body...
  BIND_FUNC 65
  SET_FUNC_NAME "main"
  PUSH_UNDEFINED
  DEFINE "main"
```

（注：函数 id 由编译器按声明顺序分配（`func_id_next` 从 `FUNC_ID_PROGRAM_BASE`=64 起递增），不写回 AST——运行时 `BIND_FUNC` 填充 `fn->id` 并登记进 `vm->functions_by_id`。id 单一来源：只出现在 `BIND_FUNC` 一处，`PUSH_FUNCTION` 不再携带，与类型 id 机制对称但分配在 compiler 侧。）

（注：注册段先于函数体编译，`PUSH_FUNCTION` 的入口 pc 先写占位，函数体区编译完成后回填；无 JMP 守卫，类型提升区即产物开头，顺序执行直达注册段。）

**类型提升区（hoist）两遍扫描**：编译期遍历 sema->types（程序类型登记表，**函数签名类型亦登记于此**——签名本质是普通类型，且函数指针作参数时签名引用签名，须纳入提升区两遍构造），分两遍生成构造字节码——**pass 1 声明所有类型**：数组 `PUSH_ARRAY → DEFINE_TYPE <id>`、签名 `PUSH_FUNC_TYPE → DEFINE_TYPE <id>`、限定符 `PUSH_CONST / PUSH_VOLATILE → DEFINE_TYPE <id>` 创建开放对象并登记进 `types_by_id`（不设字段；内建别名 `LOAD_TYPE <内建 id> → DEFINE_TYPE <id>`），完成后所有程序类型 id 在表中都有登记（开放或密封），后续任何类型字段构造都可 `LOAD_TYPE <id>` 拿到对象（**向前引用安全**，为未来 struct 字段引用后声明类型 / 指针自引用铺路）；**pass 2 定义所有类型**：逐个 `LOAD_TYPE <id>` 拉回开放对象 → 设字段（`DEFINE_BOUND N` / `FUNC_TYPE_PARAM·RETURN` / `SET_TYPE`）→ `SEAL` 封闭算布局（统一置 `sealed`），**依赖后序**（递归 + done 去重共享依赖：数组 SEAL 需 elem 已密封、const/volatile 拷贝 size/align 需 sub 已密封、签名需参数/返回已构造，故先定义依赖再定义自身）。注册段不再内联构造签名类型，函数槽位 `LOAD_TYPE <sig_id>` 直接查表拉回。

- **参数不按名绑定，由函数体内弹栈 DEFINE**：`bcode_call_cfunc` 把 local_args 按序压操作数栈（压栈顺序 `a, b` → 栈顶是 `b`），函数体头部编译期生成倒序 `DEFINE "b"; DEFINE "a"` 依次弹栈定义。参数名只在编译期用于生成 DEFINE 指令，运行时函数值不含参数名。
- **func type 构造在 hoist 提升区（签名纳入两遍扫描）**：`PUSH_FUNC_TYPE` 分配空 `func_type_t`（开放类型，暂不入池）并压其 type value；pass 1 `DEFINE_TYPE <sig_id>` 弹栈声明——绑定程序 id + 登记进 `vm->types_by_id`（此后 `LOAD_TYPE <sig_id>` 可拉回开放对象）；pass 2 `LOAD_TYPE <sig_id>` 拉回开放对象作为定义起点；随后每参数 `LOAD "T"; FUNC_TYPE_PARAM` 按声明顺序追加为参数类型；`LOAD "T"; FUNC_TYPE_RETURN` 设为返回类型（依赖后序：参数/返回可为另一签名，递归先定义再引用）；`FUNC_TYPE_VARARG` 标记可变参数（M1 无用户变参函数省略该指令）；`SEAL`（无操作数）经统一 `value_seal` 代理到 `func_type_seal`，计算规范名 `func(...)`、按签名查重 intern（首次成功才入 `vm->sig_types` 池并置 `sealed=true` + 布局计算；去重复用则回收开放类型并重定向栈引用）→ 弹栈（消费类型位），密封前读开放对象自身 id（`DEFINE_TYPE` 已绑定），密封后按该 id 幂等更新登记（去重时重绑新 intern 实例，避免登记表悬垂）。**注册段**（hoist 区之后）`LOAD_TYPE <sig_id>` 主动拉回密封签名类型压栈，供 `PUSH_FUNCTION` 弹栈顶组装 func value（签名类型不再残留在栈上）。`type_func_sig` 仍保留为 C 侧一次性快捷构造（不向 VM 栈压 type value，供 builtin_printf/sema/测试使用）。
- **void 函数返回 undefined**：为统一性，void 类型函数实际 `return undefined`——函数体末尾（或显式 `return;`）编译为 `PUSH_UNDEFINED; RET;`。`RET` 语义统一为"栈顶即返回值"：非 void 函数返回表达式求值结果，void 函数返回 void 类型的 undefined value。`return expr;` => `...expr...; RET`。
- **clux 函数不支持可变参数**（可变是 FFI 的）：`argc` 固定等于签名参数个数，sema 已静态校验，运行时无需变参处理。
- **PUSH_FUNCTION 构造 bcode_function_t**：操作数为**入口 pc 一个立即数**（编译期把 `L_FUNC_START` 标签回填为绝对字节偏移），回调调 **`bcode_function_new(vm, sig, entry_pc, vm->root_scope)`**（bcode_function 模块自封装创建：`base.cfunc = bcode_call_cfunc`、自建孤立 closure_scope、注册 `vm->functions` 池，`fn->id` 默认 0），再**弹栈顶签名类型**（`LOAD_TYPE <id>` 拉取的 `SEAL` 产物）组装 func value（type = 签名类型，data = bcode_function_t）压栈，随后 `BIND_FUNC <id>` 填充 `fn->id` 并登记 id 表、`SET_FUNC_NAME "add"` 写入显示名、`PUSH_UNDEFINED; DEFINE "add"` 把函数注册进当前 scope——函数是一等值。
- **函数定义用 DEFINE（无 DEFINE_FUNCTION）**：函数值本身携带签名类型（value.type），且函数名固定不可重命名（定义语句非变量赋值），所以注册段压 `PUSH_UNDEFINED`（无类型说明符）+ `DEFINE "name"` 单值弹栈；普通变量定义走 value, type 双弹。

`bcode_function_t` **不是编译期预建的表**，由 `PUSH_FUNCTION` 执行时构造——除基类 `func_t` 外只多一个入口字节偏移 `entry_pc`，不含参数名、不含 AST 节点指针；函数 id 与显示名存于基类 `func_t`（`id` 由 `PUSH_FUNCTION` 立即数写入，`name` 由 `SET_FUNC_NAME` 写入）。区别于运行时 `func_t`（C 函数值）与 AST 层 `ast_func_def_t`（AST 节点）。`CALL` 只需 `value_call`，scope/frame 处理在 `func_vcall` 通用流程 + `bcode_call_cfunc` 执行回调内。

**函数对象生命周期统一归 vm（`vm->functions` 池）**：所有函数值共享同一 `func_t*`（`func_clone` 浅拷贝指针），因此 `func_t` 不能由某个 value dispose 释放（double free）——`func_dispose` 为空操作（`value_dispose` 只释放 data 块与 value_t 结构体），`func_t` 本体注册进 `vm->functions`，`vm_destroy` 遍历统一释放（`bcode_function_t` 自建的 `owns_closure_scope` 先 `scope_destroy` 再 `func_destroy`）。`func_t.owns_closure_scope` 区分自建 scope（bcode_function）与调用方传入（func_new，不拥有）。

**closure_scope = 孤立作用域（parent=NULL）**：clux 用**显式闭包捕获**——函数对象创建时 `scope_new(alloc, NULL)` 建孤立 scope，不挂任何作用域树（不随定义点作用域销毁）；`func_vcall` 调用期间临时让 `closure_scope->parent = root_scope` 使函数体可查看到模块变量，调用结束恢复原 parent。该作用域跟随函数对象销毁（`vm_destroy` 释放 functions 时处理）。

**exec_run 只注册不执行（clux 无顶层语句）**：`exec_run` 只驱动类型提升区 + 函数注册段（提升区：`PUSH_ARRAY / PUSH_FUNC_TYPE / PUSH_CONST / PUSH_VOLATILE` → `DEFINE_TYPE <id>` 声明、`LOAD_TYPE <id>` → `DEFINE_BOUND N / FUNC_TYPE_PARAM* / FUNC_TYPE_RETURN / SET_TYPE` → `SEAL` 定义；注册段：`LOAD_TYPE <sig_id>` / `PUSH_FUNCTION` / `BIND_FUNC` / `SET_FUNC_NAME` / `DEFINE` 序列），入口函数由调用方在 `exec_run` 之后 `scope_lookup(vm->current_scope, "main")` + `value_call(vm, fn, NULL, 0)` 显式触发（driver 职责）。

#### 2.7.7 控制流编译模板

```
if (c) A else B:          while (c) B:            a && b:
  ...cond...                L_cond:                  PUSH a
  JZ L_else                 ...cond...               JZ L_false
  ...A...                   JZ L_end                 PUSH b
  JMP L_end                 ...B...                  JZ L_false
  L_else:                   JMP L_cond               PUSH_BOOL true
  ...B...                   L_end:                   JMP L_end
  L_end:                                          L_false:
                                                  PUSH_BOOL false
                                                  L_end:
```

`for` desugar 成 init + while。短路运算符在编译期展开成跳转（不做运行时惰性求值）。跳转目标 pc 为绝对字节偏移，编译期 emit 时记录当前位置、回填目标值。

**跳转跨作用域必须显式平衡（关键规则）**：跳转指令（`JMP`/`JZ`/`JNZ`）会破坏 scope 状态——块内若有 `PUSH_SCOPE` 压入的子作用域，跳转离开该块时不会经过对应 `POP_SCOPE`，导致 scope 链泄漏（子作用域的 owned 值永不回收）。因此**编译器在 emit 任何跳出 N 层嵌套块的跳转指令前，必须先发 N 个 `POP_SCOPE`** 平衡掉这些块作用域，再 emit 跳转。典型场景：

- `if` 分支体开块作用域后 `return`/`break`/`continue` 提前离开 → 跳转前弹掉分支块
- `while` 循环体开块作用域后 `continue` 跳回 `L_cond` → 跳转前弹掉循环体块
- 短路 `&&`/`||` 的 `JZ L_false` 若跨越表达式内联块 → 同样先弹

编译器需跟踪**当前已压栈的块作用域计数**（scope_depth），emit 跳转时按 `depth` 生成 `POP_SCOPE` 序列；标签目标侧（`L_end` 等）的栈深度与源侧一致，保证执行器 `POP_SCOPE` 与 `PUSH_SCOPE` 严格配对。该平衡只发生在编译期生成字节码时，执行器本身对 scope 不感知跳转（执行器只按指令流机械 push/pop）。

**操作数栈深度同样需编译器追踪平衡（悬垂警告）**：跳转使多个控制流路径在标签处汇合，汇合点的操作数栈深度必须对所有路径一致——否则执行器在汇合点后按统一假设消费栈（如 `POP`/`STORE`/二元运算弹 N 个操作数）时，某条路径栈深度偏深/偏浅会取错值或栈下溢 panic。因此编译器**静态追踪每个 emit 点的操作数栈深度（stack_depth）**：

- 每条指令对栈的净影响已知（`PUSH_*`/`LOAD`/`CALL` 结果 +1，`POP`/`DEFINE`/`STORE`/二元运算 -N，`JMP`/`JZ`/`HALT` 0 等），emit 时逐条累加
- **每个标签（跳转目标）记录入栈深度**：首次定义时记录当前深度，后续有跳转指向该标签时校验与记录值一致
- `JZ`/`JNZ` 自身弹 1（条件值），汇合前各分支须先把各自栈上"多余的临时值"消费干净（`POP` 或参与运算），使跳转指令执行后的栈深度一致
- 深度不一致 → 编译器发**悬垂警告/错误**（dangling stack depth）：如 `if(c) a else b` 中某分支压栈未消费、`while` 循环体在 `continue` 前残留表达式结果等
- 该检查与 scope_depth 平衡是同一编译器状态机的一部分：**scope_depth 管 PUSH/POP_SCOPE 配对，stack_depth 管操作数栈配对**，两者在 emit 跳转时同时校验

#### 2.7.8 生命周期闭环

| 值的来源 | 谁拥有 | 何时释放 |
|---|---|---|
| 变量值 | scope（`scope_define`） | `scope_destroy` |
| 表达式中间结果 | 当前 scope（匿名 track） | `POP_SCOPE` |
| 字符串表字符串 | 字符串表（编译期收集，只读） | `bytecode_destroy` |
| 栈元素 | **借用，无人拥有** | 随 scope 销毁自然失效 |
| 函数对象 `func_t` | **`vm->functions` 池**（`func_dispose` 空操作，value 只释放 data 块） | `vm_destroy` 遍历释放 |
| 签名类型 `func_type_t` | **`vm->sig_types` 池**（按签名去重 intern） | `vm_destroy` 释放 |
| 孤立 closure_scope | 函数对象（`owns_closure_scope`） | 随函数对象销毁（`vm_destroy`） |

### 2.8 诊断 (include/diag/diagnostic.h, src/diag/diagnostic.c) —— 已实现

**公共模块**：Lexer、Parser、Sema、Bytecode Compiler、Bytecode VM 共用同一个收集器，driver 统一在出口打印，而不是边错边打。

```c
typedef enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE } diag_level_t;

typedef struct {
    diag_level_t level;
    location_t   loc;
    char        *message;
} diagnostic_t;

typedef struct diag_buf {
    allocator_t   *alloc;
    diagnostic_t  *items;   // 动态数组
    size_t         count;
    size_t         capacity;
} diag_buf_t;

void diag_error(diag_buf_t *db, location_t loc, const char *fmt, ...);
void diag_warning(diag_buf_t *db, location_t loc, const char *fmt, ...);
void diag_note(diag_buf_t *db, location_t loc, const char *fmt, ...);
void diag_print_all(const diag_buf_t *db);   // 统一格式化输出到 stderr（Rust 风格源码片段 + ^ 标记）
bool diag_has_error(const diag_buf_t *db);
size_t diag_count(const diag_buf_t *db);
const diagnostic_t *diag_items(const diag_buf_t *db);
```

输出格式：Rust 风格——首行 `<file>:<line>:<col>: error: <message>`，随后打印源码行与 `^` 标记定位到出错列。

### 2.9 语义分析 (include/sema/sema.h, symbol.h + src/sema/*.c)

语义分析分两个阶段：**先构建 sema 作用域树**（scope 节点带完整符号表），**再按作用域树用 shadow value 遍历 AST** 做类型检查与推导。作用域结构与类型检查彻底分离。

**核心契约：** sema 是处理类型错误的最后一个阶段。sema 通过后，字节码编译器和 VM 可以假定一切类型正确，运行时不再做类型检查。vtable 运算返回的 error value 被 sema 捕获并转为诊断，不传播到下游。

**类型解析预留：** 所有类型解析收敛到独立入口 `resolve_type(sema, name)`，M1 内部为 `type_find(vm, name)` 查表。未来类型本身是编译期表达式（`[N]T` / `[]T` / `*T` / `<T1,T2>` / const 修饰），需要替换为类型表达式求值器（type_expr AST → type_t），该接口形态保证调用方不变。

#### 2.9.1 模块结构

```
include/sema/
  sema.h       — sema_t 上下文 + 公共 API (sema_create / sema_analyze / sema_destroy)
                 + internal 段（resolve_type / sema_loc / sema_expr 等跨文件声明）
  symbol.h     — sema_symbol_t, sema_scope_t（sema 侧符号表）

src/sema/（按语法节点类别拆分）
  sema.c       — 上下文管理 + 三遍编排 + Pass 1/2（函数名收集 + 类型解析）
  stmt_build.c — Pass 3a 作用域树构建 + 控制流分析（返回路径完整性 / unreachable）
  stmt.c       — Pass 3b 语句 walker（shadow value 运行 + 确定性赋值分析）
  expr.c       — 表达式 walker（shadow value 求值）
  symbol.c     — sema 侧符号表实现
```

sema 侧符号表是**纯编译期元数据**（名字 → 类型 + 定义节点）：符号真正重要的是"名字"，名字是符号表映射的 key。**运行态（shadow value）的 lookup/define 不经过符号表**——通过与 sema 作用域树**同构的 VM scope 树**（`scope_t::vars`）完成，名字从定义节点 ast 提取（`ast_var_def_t::name` / `ast_func_def_t::name`），保证两棵作用域树严格对齐。

TDZ（确定性赋值分析）与遮罩机制的归属分工：
- **TDZ（flow_init）** → sema 符号表编译期数据流状态：`var x = expr` 定义即初始化（`flow_init=true`）；`var x:T = undefined` 未初始化（`flow_init=false`）；简单/复合赋值成功退出未初始化；if 合并点两分支都初始化（meet AND 语义）才视为确定初始化，循环体内赋值不提升（保守）。读取时 `flow_init=false` → "used before initialization" 编译错误。**VM 值层不感知未初始化状态**。
- **遮罩** → VM scope 链（`scope_lookup` 沿 parent 取第一个命中）；sema 侧 `is_active` 与 VM 定义点同步激活，保证 `var x = x + 1` 自引用解析到外层

VM scope 树与 sema 作用域树**逐节点同构**：函数级 scope、块 scope、if/while body scope、for scope 全部成对 push/pop（`vm_push_scope` / `vm_pop_scope`），变量定义 `scope_define` 到当前 VM scope、变量读取 `scope_lookup` 沿链查找。

#### 2.9.2 数据结构

```c
typedef enum {
    SEMA_SCOPE_GLOBAL,    /* 全局作用域（函数名） */
    SEMA_SCOPE_FUNCTION,  /* 函数作用域（参数 + 函数体顶层变量） */
    SEMA_SCOPE_BLOCK,     /* 块作用域 */
    SEMA_SCOPE_FOR,       /* for 作用域（init 变量） */
} sema_scope_kind_t;

typedef struct sema_symbol_t {
    const type_t *type;       /* 已解析类型；NULL = 待推断（shadow VM 阶段填充）。
                                 函数符号 = 签名类型（func_type_t，vm 池 intern） */
    ast_node_t   *ast;        /* 定义节点（借用，arena 管理）：函数 = AST_FUNC_DEF */
    bool flow_init;           /* 确定性赋值分析（Pass 3b）：变量是否确定已初始化。
                                 false = 未初始化（TDZ），读取时编译错误
                                 "used before initialization"。仅变量符号有意义 */
    bool is_active;           /* 符号是否已定义到 VM scope（运行时可见）。函数/内置
                                 符号注册即激活；变量在 shadow_var_def 定义时激活 */
} sema_symbol_t;

typedef struct sema_scope_t {
    struct sema_scope_t *parent;
    vec_t              *children;   /* sema_scope_t* 子作用域（按出现顺序） */
    strmap_t           *symbols;    /* name -> sema_symbol_t* */
    sema_scope_kind_t   kind;
} sema_scope_t;

typedef struct sema_func_t {
    ast_node_t   *def;    /* AST_FUNC_DEF（借用） */
    sema_scope_t *scope;  /* 函数作用域树（Pass 3 填充） */
    strslice_t    name;   /* 函数名（诊断用） */
} sema_func_t;

typedef struct sema_t {
    vm_t         *vm;                 /* 复用 VM 类型注册表 + vtable + shadow value */
    diag_buf_t   *diag;               /* 诊断收集器 */
    sema_scope_t *global_scope;       /* 全局作用域树根 */
    vec_t        *funcs;              /* sema_func_t* 函数队列（sema 拥有，可增长） */

    /* 函数上下文（Pass 3b 时设置） */
    const type_t *func_return_type;   /* NULL = void */
    bool          func_has_return;

    /* 循环上下文 */
    int           loop_depth;         /* 0 = 不在循环中 */
} sema_t;
```

符号表只负责名字解析：函数符号 `sym->ast` 指向定义节点、`sym->type` 存签名类型；函数自身状态（作用域树）由 `sema_func_t` 承担，统一登记在 `sema->funcs` 队列——为局部函数提升与泛型单态化预留（解析中发现的新函数追加到队尾，Pass 3 队列驱动按序处理）。

作用域树示例（`{ if(cond) {} else{} }`）：

```
block_scope
├── then_block_scope    (block1)
└── else_block_scope    (block2)
```

#### 2.9.3 Pass 1/2：Name / Type Collection

- **Pass 1**：遍历 `AST_PROGRAM` 顶层 `AST_FUNC_DEF` 兄弟链，函数名注册到 global scope（检测重复定义），定义成功后创建 `sema_func_t` 追加到 `sema->funcs` 队列
- **Pass 2**：遍历 `sema->funcs` 队列，解析参数类型与返回类型（经 `resolve_type`），注册签名类型 `type_func_sig` 填入 `sema_symbol_t.type`，`sema_symbol_t.ast` 指向定义节点。参数类型未知 → 诊断

#### 2.9.4 Pass 3a：作用域树构建

遍历 `sema->funcs` 队列，对每个函数按词法块结构建树（作用域树存入 `sema_func_t.scope`）。只注册符号（名字 + 声明类型），**不做类型检查**。推断类型的变量 `type=NULL`，留给 Pass 3b 填充。

```c
static void build_func(sema_t *sema, sema_func_t *sf) {
    ast_func_def_t *fn = (ast_func_def_t *)sf->def;
    sema_scope_t *fscope = sema_scope_new(SEMA_SCOPE_FUNCTION, sema->global_scope);
    sema_scope_add_child(sema->global_scope, fscope);
    sf->scope = fscope;

    /* 注册参数（已解析类型；运行时值在 Pass 3b 进入函数时定义到 VM scope） */
    for (ast_node_t *p = fn->params; p; p = p->next) {
        ast_var_def_t *vd = (ast_var_def_t*)p;
        sema_scope_define(fscope, vd->name, &(sema_symbol_t){
            .type = resolve_type(sema, vd->type_name),
        });
    }

    /* 函数体 block 直接用 fscope（不再嵌套一层） */
    build_block(sema, (ast_block_t*)fn->body, fscope);
}
```

块内语句的 scope 构建规则——只在创建子作用域的节点上递归：

| AST 节点 | 作用域动作 |
|----------|-----------|
| `AST_VAR_DEF` | 注册符号到当前 scope（`type==NULL` 表示待推断） |
| `AST_BLOCK` | 新建子 scope，递归构建 |
| `AST_IF` | then_body / else_body 各建一个子 scope（else 为嵌套 if 时同层递归） |
| `AST_WHILE` | body 建一个子 scope |
| `AST_FOR` | 建 for scope（init 变量注册到 for scope），body 建 for scope 的子 scope |
| 其他语句 | 不创建作用域 |

#### 2.9.5 Pass 3b：Shadow VM 运行

遍历 `sema->funcs` 队列，对每个函数按预建作用域树（`sema_func_t.scope`）严格对应地遍历 AST。**队列驱动**：`len` 每次重取，解析过程中队列增长（局部函数提升 / 泛型单态化追加到队尾）自动被后续迭代覆盖；增长函数的 `scope==NULL` 时先补建树再 walk。

核心机制：

**子作用域迭代器（child_idx）**：两个阶段遍历同一棵 AST，子作用域出现顺序一致。Shadow VM 用 `size_t child_idx` 按序取子作用域（`vec_get(scope->children, child_idx++)`），保证作用域严格对应。

**VM scope 链遮罩**：变量遮罩与自引用由 VM scope 链天然提供——定义**先求值 init、后 `scope_define`**，因此 init 求值时新变量尚未入 VM scope，`scope_lookup` 沿 parent 链解析到外层同名变量；`scope_define` 后才可见并遮罩外层：

```
var x:i32 = 1;           // outer x 定义到函数级 VM scope
{
    var x = x + 1;       // init 的 x：内层尚未 define → lookup 到 outer x（i32）
                         // init 求值后 define inner x 到块级 VM scope（遮罩 outer）
    x = x + 2;           // 此处的 x 是 inner x（块级 scope 先命中）
}
x = x + 3;               // 块级 VM scope 已 pop → outer x
```

```c
/* 变量读取（expr.c AST_IDENT）：从 VM scope 链 lookup shadow value，
   未初始化检查走符号表 flow_init（确定性赋值分析，VM 值层不感知） */
value_t *v = scope_lookup(sema->vm->current_scope, n->name);
if (!v)        { /* undefined variable 诊断 */ }
sema_symbol_t *sym = sema_lookup(scope, n->name);
if (sym && !sym->flow_init) { /* used before initialization 诊断 */ }
return value_make_shadow(sema->vm, value_type(v));
```

**未初始化变量处理**（状态在 sema 符号表 flow_init 上，**VM 值层零感知**）：`var x:i32 = undefined;` 是唯一未初始化声明语法（要求显式类型标注，undefined 无类型可推断），定义时 `flow_init=false` 并构造声明类型 shadow value 存入 VM scope；运行时 op_define 以**声明类型零值**占位。读取时 `flow_init=false` → 编译期报"used before initialization"；简单/复合赋值成功 → `flow_init=true`（退出未初始化）；if 合并点 meet（AND）两分支都初始化才视为确定初始化（`if(c){a=1;}else{}` 保守报错），循环体内赋值不提升确定性。

#### 2.9.6 表达式 walker（shadow value 求值）

每个表达式节点返回一个 **shadow value**（只有类型，data=NULL）。shadow value 经 vtable 运算路径，类型协商结果即为推导结果类型。

```c
static value_t *sema_expr(sema_t *sema, ast_node_t *node, sema_scope_t *scope);
```

| AST 节点 | 处理 |
|----------|------|
| `AST_INT_LIT` | 解析后缀定类型（`42i8`→i8，`42`→i32 默认） |
| `AST_FLOAT_LIT` | 解析后缀定类型（`3.14f32`→f32，`3.14`→f64 默认） |
| `AST_BOOL_LIT` | shadow bool |
| `AST_STRING_LIT` | shadow str |
| `AST_CHAR_LIT` | shadow u8 |
| `AST_IDENT` | `scope_lookup`（VM scope 链）取 shadow value → shadow(类型)；未定义 → 诊断；符号 `flow_init=false` → "used before initialization" 诊断 |
| `AST_BINARY` | lhs/rhs shadow 求值 → vtable 分派；短路 `&&`/`||` 特殊处理（操作数必须 bool，结果 bool） |
| `AST_UNARY` | 操作数 shadow → vtable 一元分派 |
| `AST_CALL` | 构造 shadow callee（`value_make_shadow(vm, sym->type)`）→ `value_call` 分派 func_vcall shadow 分支校验签名 → 返回 return_type shadow |
| `AST_CAST` | `resolve_type` 目标 → `value_explicit_cast` 校验转换合法性 |

二元运算（非短路）走 vtable，类型不兼容时返回 error value，sema 转为诊断：

```c
static value_t *shadow_binary(sema_t *sema, ast_binary_t *node, sema_scope_t *scope) {
    if (is_short_circuit(node->op)) {
        value_t *lhs = sema_expr(sema, node->lhs, scope);
        check_bool(sema, node->lhs, lhs, "logical operator");
        value_t *rhs = sema_expr(sema, node->rhs, scope);
        check_bool(sema, node->rhs, rhs, "logical operator");
        return value_make_shadow(sema->vm, sema->vm->type_bool);
    }

    value_t *lhs = sema_expr(sema, node->lhs, scope);
    value_t *rhs = sema_expr(sema, node->rhs, scope);
    value_t *result = dispatch_binary(sema->vm, node->op, lhs, rhs);

    if (value_is_error(sema->vm, result)) {
        diag_error(sema->diag, loc(node), "type mismatch: %s %.*s %s", ...);
        return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    return result;  /* shadow in → shadow out */
}
```

#### 2.9.7 函数调用（shadow 版本）

**签名在 func value 的 type 上**：sema 符号表统一记录定义 AST 节点（`sym->ast`，函数符号 = `AST_FUNC_DEF`，借用不拥有，arena 管理），签名类型存于 `sym->type`（`func_type_t`，按签名去重 intern 到 vm 类型池）。调用点用 `value_make_shadow(vm, sym->type)` 构造 shadow callee——value 严格存活于 shadow 态（data=NULL，只带签名类型），auto-track 到 vm scope，由 scope 统一管理生命周期，sema 不手动释放。

**校验单一来源在 func_vcall**：shadow callee 经 `value_call` 分派到 `func_vcall` 的 shadow 分支（`func_shadow_call`），数量检查 + 逐参数 `value_implicit_cast` 验证兼容性与真实调用完全一致，不执行 cfunc。校验失败返回的 error value 由 sema 翻译为带位置的诊断。

```c
case AST_CALL: {
    /* 符号查找：global scope 只含函数符号，sym->ast 为 AST_FUNC_DEF（借用） */
    if (!sym || !sym->ast) { /* undefined function 诊断 */ }

    /* 逐个实参 shadow 求值（错误恢复产物保留为 void shadow，由
       func_shadow_call 跳过，避免级联二次诊断） */
    size_t argc = sema_count_siblings(call->args);
    value_t *arg_shadows[argc > 0 ? argc : 1];
    ast_node_t *arg = call->args;
    for (size_t i = 0; arg; arg = arg->next, i++)
        arg_shadows[i] = sema_expr(sema, arg, scope);

    /* shadow callee：data=NULL 只带签名类型（sym->type），受 vm scope 管理 */
    value_t *callee_shadow = value_make_shadow(sema->vm, sym->type);
    value_t *result = value_call(sema->vm, callee_shadow, arg_shadows, argc);

    /* error → 翻译为诊断（消息由 func_shadow_call 生成） */
    if (value_is_error(sema->vm, result)) {
        error_data_t *ed = (error_data_t *)value_data(result);
        const char *msg = ed && ed->message ? string_cstr(ed->message)
                                            : "function call failed";
        diag_error(sema->diag, sema_loc(sema, &call->base), "%s", msg);
        return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    return result; /* shadow in → shadow out（return_type shadow） */
}
```

#### 2.9.8 语句 walker 与控制流分析

语句不返回值，但有副作用（定义变量、检查赋值规则、验证控制流）。每个语句返回 `block_result_t{bool definitely_returns}` 用于返回路径完整性分析。

```c
static block_result_t sema_stmt(sema_t *sema, ast_node_t *stmt,
                                 sema_scope_t *scope, size_t *child_idx);
```

| 语句 | 处理要点 |
|------|---------|
| `AST_VAR_DEF` | 构造 shadow value：`AST_UNDEF` init（`var x:T = undefined`，要求显式类型）→ `flow_init=false` + 声明类型 shadow；普通 init → 先求值（未 define → 自引用解析到外层），`flow_init = !init_bad`，显式类型经 `value_assign` 校验 init / 推断类型写回 `sym->type`。最后 `scope_define` 到当前 VM scope 并激活符号（`is_active=true`） |
| `AST_ASSIGN` | `_ = expr` 为显式丢弃；`scope_lookup` 取左值 shadow → `value_assign` 单一校验点（error → 诊断，成功 → 符号 `flow_init=true` 退出未初始化）；复合赋值 `x op= rhs` 展开为 `x = x op rhs`（shadow 走 vtable 协商）后再 `value_assign` 赋回，成功同样置 `flow_init=true` |
| `AST_BLOCK` | 取子 scope（`child_idx++`），递归遍历 |
| `AST_IF` | 条件必须 bool；then/else 各取子 scope；`definitely_returns = then && else` |
| `AST_WHILE` | 条件必须 bool；`loop_depth++` 后遍历 body；不贡献 definitely_returns（循环体可能不执行） |
| `AST_FOR` | init 在 for scope；条件必须 bool；body 是 for scope 的子 scope；`loop_depth++` 遍历 |
| `AST_RETURN` | 校验值类型可赋给返回类型；void 函数禁止返回值；返回 `definitely_returns=true` |
| `AST_BREAK/CONTINUE` | `loop_depth == 0` → "break/continue outside loop" 诊断 |
| `AST_EXPR_STMT` | 结果必须为 void，否则必须用 `_ = ...` 显式丢弃 |
| `AST_BLOCK`（顶层） | `definitely_returns` 传递到函数级：非 void 函数所有路径必须 return |

#### 2.9.9 类型解析与赋值兼容性

```c
/* 类型解析唯一入口：M1 内部 type_find；未来替换为类型表达式求值器 */
const type_t *resolve_type(sema_t *sema, strslice_t name);
```

**赋值兼容性单一校验点在 `value_assign`**（不再有独立的 `sema_type_assignable`）：sema 构造声明类型 shadow 作为 dst，调用 `value_assign(vm, dst, src)` 分派到 vtable assign 槽——各类型 assign 的 shadow 分支只做类型协商（int：`value_implicit_cast` 向左值类型转换、bool：类型必须匹配、str：类型必须匹配），返回 error 即不兼容，由 sema 翻译为带位置诊断。变量定义初始化、简单赋值、复合赋值赋回、return 类型校验全部走此通道，行为与真实 VM 赋值完全一致。

M1 无 const/volatile（不在 M1 阶段实现），符号表与类型检查不含 const 规则。

#### 2.9.10 与 VM 的集成

| VM 设施 | sema 中的用途 |
|---------|-------------|
| `type_find(vm, name)` | `resolve_type` 内部实现 |
| `value_make_shadow(vm, type)` | 为每个表达式构造类型标记 |
| `value_add/sub/mul/...` | 二元运算类型推导（shadow 输入 → shadow 输出） |
| `value_implicit_cast` | 函数参数兼容性检查（func_shadow_call 内）；int_assign 内部向左值类型转换 |
| `value_explicit_cast` | as 转换合法性检查 |
| `value_assign` | 赋值兼容性单一校验点（变量初始化 / 简单赋值 / 复合赋值赋回 / return 校验） |
| `sema_symbol_t.flow_init` | 确定性赋值分析（编译期数据流）：变量读取检查 / 定义置位 / 赋值退出 / if 合并点 meet（AND） |
| `scope_define` / `scope_lookup` | 变量 shadow value 定义/查找（VM scope 链与 sema 作用域树同构，天然遮罩） |
| `vm_push/pop_scope` | 与 sema 作用域树同构的 VM scope 树（函数级 / 块 / if / while / for 成对 push/pop） |
| `type_func_sig` | Pass 2 注册签名类型到 vm 池（存 `sema_symbol_t.type`） |
| `value_call` → `func_vcall` | 函数调用签名校验（shadow 分支 `func_shadow_call`，单一校验来源） |

**不使用的 VM 设施**：`func_new` / `func_t` 引擎字段（sema 不构造 func value，只经签名类型做静态校验）。

#### 2.9.11 公共 API 与 driver 集成

```c
sema_t *sema_create(vm_t *vm, diag_buf_t *diag);
bool    sema_analyze(sema_t *sema, ast_node_t *program);  /* 三遍扫描 */
void    sema_destroy(sema_t **sema);
```

```c
if (parser_error(parser)) return 1;          /* 语法错误不进 sema */

sema_t *sema = sema_create(vm, diag);
if (!sema_analyze(sema, ast)) {
    diag_print_all(diag);                    /* 语义错误 */
    return 1;
}
/* sema 通过 → 进入字节码编译 */
```

作用域树是持久化数据，sema 结束后不销毁，交由字节码编译器复用（变量类型静态分派、作用域结构定位 load/store、TDZ 初始化信息）。

### 2.10 Driver（include/driver/driver.h, src/driver/driver.c）—— 已实现（完整流水线 lex → parse → sema → bytecode compile → execute）

流水线编排者，见本文档第 1 节。当前落地阶段 ①（加载源码）与 ②（词法分析），并直接完成 ③（输出单词表）：

```c
// 加载源码到内存缓冲（allocator 管理，data 在 allocator 存活期间有效）。
int driver_load_source(allocator_t *alloc,
                       const char *path,
                       const char **out_data,
                       size_t     *out_len);

// 加载 + 词法分析 -> token 池（vec<token_t*>, owns_element=true）。
// 返回 0 成功 / -1 文件无法打开；词法错误以 TOKEN_TYPE_ERROR 留在池中。
int driver_lex_file(allocator_t *alloc, const char *path, vec_t **out_pool);

// 顶层入口：加载 -> 词法 -> 输出单词表。
// 返回退出码：0 成功，1 编译错误（文件打不开 / 词法错误）。
int driver_run_file(const char *path);
```

`cmd_run`（src/cmd/run.c）只做参数解析（取首个位置参数作为文件路径），其余编排下沉到 `driver_run_file`。

**单词表输出格式（阶段 ③）**：每个 token 独占一行（EOF 作为终止符跳过不打印），打印其源码位置范围与转义后的文本：

```
<TOKEN_KIND> L<begin.line>:<begin.col>-<end.line>:<end.col> <escaped-text>
```

示例：`KEYWORD L1:1-1:5 func`；`STRING L2:10-2:15 \"ab\n\"`。控制字符转义为字面 `\n`/`\r`/`\t`/`\\`/`\"`，其他不可打印字节转 `\xHH`，**不输出真实换行/制表符**。词法错误行额外追加 ` error: <message>`，并同时向 stderr 打印诊断 `<file>:<line>:<col>: error: <message>`，退出码置 1。

**内存源约束**：Lexer 要求内存直读源（`istream_data != NULL`），而 `stream_source_file` 的 `data()` 为 NULL，因此阶段 ① 先把文件读入 allocator 缓冲，再用 `stream_source_mem(allocator, buf, len, owns_data=true)` 建内存源——缓冲由 istream/lexer 生命周期自动释放。token 文本切片在 lexer 存活期间有效，故单词表在 `lexer_close` 之前打印完毕。

`driver_compile_file` / `driver_run_file` 已接入完整流水线（lex → parse → sema → bytecode compile → execute）。Driver 持有的编译单元 arena 使所有阶段产物（AST、符号表、token 文本指向的源 buffer）在同一生命周期内有效。

## 3. 命令行接口

```
clux run <file.cx>    解释执行 .cx 文件
```

退出码：0 成功，1 编译错误（含文件打不开、词法/语法/语义错误），2 运行时错误。

## 4. 构建系统

CMake（C11；测试为 C++20 + GoogleTest）。库划分：

| 目标 | 源文件 | 说明 |
|------|--------|------|
| `clux_core` | `src/core/*.c` | allocator / stream / vec / rbtree / omap / strmap / string |
| `clux_parser` | `src/parser/*.c` | lexer（T4 之后加入 parser.c），依赖 `clux_core` |
| `clux_vm` | `src/vm/*.c` | 值计算引擎：type/vtable/value/scope/function（已实现），依赖 `clux_core` + `clux_parser` + ICU |
| `clux_driver` | `src/driver/*.c` | 流水线编排（加载 + 词法，后续加 parse/sema/bytecode），依赖 `clux_core` + `clux_parser` + `clux_vm` |
| `clux_sema` | `src/sema/*.c` | 语义分析（shadow value 驱动）；**目录为空时不创建目标**（GLOB + `if`） |
| `clux_diag` | `src/diag/*.c` | 诊断收集器；同上 |
| `clux_cmd` | `src/cmd/*.c` | 子命令分发，依赖 `clux_driver` |

`sema` / `diag` 两个目录目前还没有源文件（对应流水线阶段 T5/T6），一旦放入第一个 `*.c`，CMake 会自动创建对应目标并链入 `clux` 与 `clux_test`，无需再改构建脚本。测试目标通过 `file(GLOB CONFIGURE_DEPENDS)` 自动收集 `tests/*.cpp`。

**编译警告**：项目目标统一启用严格警告（MSVC 风格驱动用 `/W4`，GNU 风格驱动用 `-Wall -Wextra -Wpedantic`），第三方（ICU / GoogleTest）保持各自配置——通过 `clux_target_warnings(<target>)` 施加，不用全局 `add_compile_options`。

刻意关闭/绕过的项：

| 项 | 处理 | 理由 |
|------|------|------|
| MSVC CRT 安全弃用（`fopen` / `tmpnam` / `freopen`） | Windows 下定义 `_CRT_SECURE_NO_WARNINGS` | clux 使用可移植 C API，而非 MSVC 专有的 `_s` 变体 |

其余警告视为真实缺陷并直接修掉（如未使用变量/参数、C99 compound literal、结构体部分初始化），**当前构建零警告**。

### 4.1 构建注意事项

**不要执行 `ninja clean` 或 `cmake --build <dir> --clean-first`。**

`third_party/icu/icu_data_gen.c` 是在 **configure 阶段**生成到 build 目录中的文件，clean 会把它删掉，而 ninja 没有重建它的规则，随后构建必然失败：

```
FAILED: third_party/icu/CMakeFiles/icudata.dir/icu_data_gen.c.obj
clang: error: no such file or directory: '<build>/third_party/icu/icu_data_gen.c'
```

- 已经 clean 过：重新 configure 即可恢复（`cmake -S . -B build`，或在 build 目录内执行 `cmake .`），再正常构建。
- 需要一次干净构建：另建一个 build 目录（`cmake -S . -B build-clean && cmake --build build-clean`），而不是清理原目录。

## 5. 测试策略

- **单元测试**：Lexer、Parser、VM（type/vtable/value/scope/function）各模块独立测试，当前 597 测试通过
- **集成测试**：.cx 程序端到端执行，对比输出
- **测试用例**：hello.cx、arithmetic.cx、functions.cx、control_flow.cx、fibonacci.cx
