# Parser 解析范式设计

## 1. parser_t 上下文

```c
typedef struct {
    allocator_t *alloc;       // token/临时分配
    arena_t     *arena;       // AST 节点分配
    vec_t       *tokens;      // token pool（由 driver 构建）
    uint32_t     pos;         // 当前游标（token pool 下标）
    bool         has_error;   // 已发生语法/词法错误（语法错误即终止，仅词法检查用）
} parser_t;
```

生命周期：`parser_create(alloc, arena, tokens)` → `parser_parse(p)` → `parser_destroy(p)`。
arena 和 tokens 的生命周期由调用方管理（driver），parser 只持有引用。

## 2. 统一 parse_xxxx 范式

### 2.1 两种失败模式

parse 函数有两种截然不同的失败语义：

| | **不匹配 (mismatch)** | **错误 (error)** |
|---|---|---|
| 含义 | 当前 token 序列不属于这个构造 | 已确认是此构造，但内部语法有误 |
| 游标 | **恢复到入口位置** `p->pos = tb` | **不恢复**，停在出错处 |
| 返回值 | **NULL** | **`AST_ERROR` 节点** |
| 上层行为 | 尝试其他分支 (failback) | 向上传播，`parse_program` 终止解析 |
| 典型场景 | `parse_primary` 看到非 primary token | `parse_if` 看到 `if` 但缺少 `{` |

返回值区分：调用方检查 `node == NULL` 表示不匹配，`node->kind == AST_ERROR` 表示错误。两者都不会被误判为成功。

### 2.2 签名约定

```c
ast_node_t *parse_xxxx(parser_t *p);
```

- **成功**：返回 AST 节点，游标指向构造之后
- **不匹配**：返回 NULL，**游标恢复到入口位置**
- **错误**：返回 AST_ERROR 节点，游标停在出错处
- **tok_begin / tok_end 约定**：`tok_begin = 进入时的 p->pos`，`tok_end = 返回时的 p->pos`

### 2.3 函数骨架

每个 parse 函数遵循统一三段式，入口保存 `tb` 用于不匹配时恢复：

```c
ast_node_t *parse_if(parser_t *p) {
    uint32_t tb = p->pos;              /* ① 记录起始位置 */

    /* 不匹配判断：首 token 不是 "if" → 恢复游标，返回 NULL */
    if (!check_keyword(p, "if")) { p->pos = tb; return NULL; }
    advance(p);                        /* 确认是 if，消费首 token，从此 commit */

    /* ② 消费 token + 递归解析子结构 */
    /* 此后失败都是 error → 返回 AST_ERROR 节点 */
    ast_node_t *cond = parse_expr(p);
    if (!cond) return make_error_node(p, tb, "expected condition after 'if'");

    ast_node_t *then = parse_block(p);
    if (!then) return make_error_node(p, tb, "expected '{' after condition");

    ast_node_t *else_ = NULL;
    if (match_keyword(p, "else")) {
        else_ = parse_block(p);
    }

    /* ③ 构造节点 */
    ast_node_t *node = ast_if_new(p->arena, tb, p->pos);
    ast_if_t *n = (ast_if_t *)node;
    n->cond      = cond;
    n->then_body = then;
    n->else_body = else_;
    return node;
}
```

关键点：**首 token 是 mismatch/error 的分水岭**。首 token 不匹配 → 恢复游标 + 返回 NULL；首 token 匹配并消费后 → commit，后续所有失败都是 error。

### 2.5 AST_ERROR 节点构造

当 parse 函数 commit 后遇到错误，构造 `AST_ERROR` 节点返回：

```c
// 构造错误节点：tok 范围 [tb, p->pos)，附带 message
ast_node_t *make_error_node(parser_t *p, uint32_t tb, const char *msg) {
    ast_node_t *node = ast_error_new(p->arena, tb, p->pos);
    ast_error_t *e = (ast_error_t *)node;
    e->message = strslice_from_cstr(msg);
    return node;
}
```

上层调用方通过返回值类型判断：
- `node == NULL` → 不匹配，尝试 failback
- `node != NULL && node->kind == AST_ERROR` → 错误，向上传播终止解析
- `node != NULL && node->kind != AST_ERROR` → 成功

```c
ast_node_t *parse_stmt(parser_t *p) {
    if (check_keyword(p, "var"))    return parse_var_def(p);
    if (check_keyword(p, "if"))     return parse_if(p);
    if (check_keyword(p, "while"))  return parse_while(p);
    if (check_keyword(p, "for"))    return parse_for(p);
    if (check_keyword(p, "return")) return parse_return(p);
    /* ... */
    /* 最后尝试赋值或表达式语句（无首 token 区分） */
    return parse_assign_or_expr_stmt(p);
}
```

各 parse_xxxx 内部做首 token 检查，不匹配时恢复游标返回 NULL，
dispatcher 的下一个 `if` 看到相同的 `p->pos` 继续尝试。
一旦某个 parse 函数消费了首 token（commit），后续失败返回 AST_ERROR 节点。

dispatcher 中不需要检查 NULL，因为最后一个 parse 函数
（parse_assign_or_expr_stmt）总会 commit 或返回 AST_ERROR。

### 2.6 空白与注释跳过约定

**父节点负责跳过空白和注释，子节点看到的第一个 token 必须是有效 token。**

```
parse_program 在每次调用 parse_func_def 前调用 skip_trivia
parse_block  在每条 stmt 前调用 skip_trivia
parse_func_def 在参数列表中跳过逗号后的 trivia
parse_for 在分号后跳过 trivia
...
```

`skip_trivia(p)`：推进游标跳过所有 `TOKEN_TYPE_WHITESPACE`、`TOKEN_TYPE_COMMENT`、`TOKEN_TYPE_MULTILINE_COMMENT`，停在第一个有效 token 上。

**例外**：`parse_program` 没有父节点，需在入口自行调用 `skip_trivia`。

**所有 parse 函数的入口前置条件**：`p->pos` 指向一个有效 token（非空白/非注释）。
**所有 parse 函数的出口后置条件**：`p->pos` 指向一个有效 token（非空白/非注释）。

这意味着：
- `advance` 不自动跳 trivia（某些场景需要精确停在原始 token）
- `skip_trivia` 由需要的地方显式调用（主要是 `parse_block`、`parse_program` 的循环体）

### 2.3 解析函数清单

| 函数 | 对应 AST 节点 | 说明 |
|------|--------------|------|
| `parse_program` | AST_PROGRAM | 顶层入口，循环 parse_func_def |
| `parse_func_def` | AST_FUNC_DEF | func name(params):type { body }（语句级，调用 parse_func_like） |
| `parse_func_like` | AST_FUNC_DEF / M2+ AST_FUNC_LIT | func 统一入口，分歧点：name 有无 |
| `parse_block` | AST_BLOCK | { stmt; stmt; ... } |
| `parse_stmt` | 各种语句 | 分派：var/if/while/for/return/break/continue；赋值/表达式走统一式 |
| `parse_var_def` | AST_VAR_DEF | var name[:type] = init; |
| `parse_assign_or_expr_stmt` | AST_ASSIGN / AST_EXPR_STMT | 赋值已在 Pratt parser 中作为表达式处理（最低优先级），此函数仅消费 ; 并包装 |
| `parse_if` | AST_IF | if (cond) { then } [else { else }] |
| `parse_while` | AST_WHILE | while (cond) { body } |
| `parse_for` | AST_FOR | for (init; cond; update) { body } |
| `parse_return` | AST_RETURN | return [expr]; |
| `parse_expr` | 表达式节点 | Pratt parser 入口 |
| `parse_expr_prec(p, min_prec)` | 表达式节点 | Pratt 核心，绑定力驱动 |
| `parse_unary` | AST_UNARY | ! / ~ / - 前缀 |
| `parse_primary` | 各种原子 | 分派：lit/ident/call/分组 |

## 3. 辅助函数

### 3.1 游标操作

```c
// 当前 token（不推进游标）
const token_t *cur_token(const parser_t *p);

// 前一个 token
const token_t *prev_token(const parser_t *p);

// 推进游标一步，返回被跳过的 token（不跳 trivia）
const token_t *advance(parser_t *p);

// 跳过空白和注释，停在下一个有效 token 上
void skip_trivia(parser_t *p);

// 推进并跳 trivia（advance + skip_trivia 的便捷组合）
const token_t *advance_skip(parser_t *p);

// 是否到达 EOF
bool at_end(const parser_t *p);
```

### 3.2 查看（不消费）

```c
// 当前 token 是指定关键字？（只看，不消费）
bool check_keyword(const parser_t *p, const char *kw);

// 当前 token 是指定符号？（只看，不消费）
bool check_symbol(const parser_t *p, const char *sym);

// 当前 token 是指定 kind？（只看，不消费）
bool check_kind(const parser_t *p, token_kind_t kind);
```

### 3.3 匹配与消费

```c
// 当前 token 是指定关键字？匹配则消费并返回 true，否则不动返回 false
bool match_keyword(parser_t *p, const char *kw);

// 当前 token 是指定符号？匹配则消费并返回 true，否则不动返回 false
bool match_symbol(parser_t *p, const char *sym);
```

### 3.4 期望与报错

```c
// 当前 token 必须是指定关键字，否则报错。成功则消费。
void expect_keyword(parser_t *p, const char *kw);

// 当前 token 必须是指定符号，否则报错。成功则消费。
void expect_symbol(parser_t *p, const char *sym);
```

`expect_*` 失败时：报告语法错误 → 构造 AST_ERROR 节点 → **立即向上传播，解析终止**。

### 3.5 错误处理策略

**语法阶段 fail-fast**：遇到第一个语法错误即构造 AST_ERROR 节点并向上传播，
`parse_program` 检测到 AST_ERROR → 终止解析 → 输出诊断 → 返回错误。

不做 panic mode 恢复（不调 synchronize、不跳 token）：
- 与词法错误的 fail-fast 策略一致，用户心智模型统一
- 避免 synchronize 的复杂度（同步集合随上下文变化、级联假错误）
- M1 极简语言，一次报一个清晰错误更有价值

**语义阶段可恢复**：语法阶段保证 AST 结构完整（每个节点都是合法构造），
语义分析遇到错误时可以以语句为单位跳过子树继续分析，不影响其余代码。
这是因为 AST 的父子/兄弟关系提供了天然的边界，无需 synchronize 猜测跳多少 token。

```
语法阶段：一个错误 → 终止（AST 结构未完成，无法可靠恢复）
语义阶段：一个错误 → 跳过该语句子树 → 继续分析（AST 结构完整，跳过是安全的）
```

### 3.6 词法错误处理

```c
// 在 parse 入口（parse_program）检查 token pool 是否含 TOKEN_TYPE_ERROR
// 如有：输出诊断 → 直接返回 NULL（不做任何解析）
```

词法错误不可恢复，parser 不尝试继续。

## 4. Pratt Parser 约定

### 4.1 绑定力表

```c
typedef struct {
    const char *symbol;   // 运算符文本
    int         left_prec; // 左绑定力（中缀运算符）
    int         right_prec;// 右绑定力（前缀运算符 / 中缀右结合）
} prec_entry_t;
```

| 优先级 | 运算符 | 左绑定力 | 右绑定力 | 结合性 |
|--------|--------|---------|---------|--------|
| 1 | \|\| | 1 | 2 | 左 |
| 2 | && | 3 | 4 | 左 |
| 3 | \| | 5 | 6 | 左 |
| 4 | ^ | 7 | 8 | 左 |
| 5 | & | 9 | 10 | 左 |
| 6 | == != | 11 | 12 | 左 |
| 7 | < > <= >= | 13 | 14 | 左 |
| 8 | << >> | 15 | 16 | 左 |
| 9 | + - | 17 | 18 | 左 |
| 10 | * / % | 19 | 20 | 左 |
| 11 | as | 21 | 22 | 左 |

前缀运算符 `! ~ -`：right_prec = 23
函数调用 `()`：left_prec = 25（最高）

### 4.2 Pratt 核心

```c
ast_node_t *parse_expr_prec(parser_t *p, int min_prec) {
    // 前缀：一元或原子
    ast_node_t *left = parse_unary(p);
    if (!left || left->kind == AST_ERROR) return left;

    // 中缀/后缀循环
    for (;;) {
        skip_trivia(p);

        // 后缀绑定力最高(25)，贪婪消费
        if (check_symbol(p, "(") || check_symbol(p, ".") || check_symbol(p, "[")) {
            if (POSTFIX_LEFT_PREC < min_prec) break;
            left = parse_postfix(p, left);
            if (left->kind == AST_ERROR) return left;
            continue;
        }

        // 中缀运算符查表
        const token_t *op = cur_token(p);
        int lp, rp;
        if (!infix_binding(op, &lp, &rp)) break;  // 不是中缀运算符
        if (lp < min_prec) break;                   // 绑定力不够

        advance(p);  // 消费运算符
        skip_trivia(p);

        // as 特殊处理：右侧是类型名，不是表达式
        if (lp == 21) {
            if (!check_kind(p, TOKEN_TYPE_KEYWORD)) {
                parse_error(p, "expected type name after 'as'");
                return ast_error_new(p->arena, ...);
            }
            strslice_t target_type = token_strslice(cur_token(p));
            advance(p);
            ast_node_t *node = ast_cast_new(p->arena, ...);
            ((ast_cast_t *)node)->expr        = left;
            ((ast_cast_t *)node)->target_type = target_type;
            left = node;
            continue;
        }

        // 递归解析右侧
        ast_node_t *rhs = parse_expr_prec(p, rp);

        ast_node_t *bin = ast_binary_new(p->arena, ...);
        ((ast_binary_t *)bin)->op  = op;   // 直接存储 token 指针
        ((ast_binary_t *)bin)->lhs = left;
        ((ast_binary_t *)bin)->rhs = rhs;
        left = bin;
    }

    return left;
}
```

`as` 运算符特殊处理：右侧不是表达式而是类型文本，在 Pratt 循环中单独分支。

`ast_binary_t.op` 直接存储 `const token_t *`（零拷贝引用 token pool），
后续阶段通过 `token_strslice(op)` 或 `token_get_kind(op)` 获取运算符文本和类型。

## 5. AST_ERROR 节点构造

当 parse 函数 commit 后遇到错误，构造 `AST_ERROR` 节点返回并终止解析：

```c
ast_node_t *make_error_node(parser_t *p, uint32_t tb, const char *msg) {
    ast_node_t *node = ast_error_new(p->arena, tb, p->pos);
    ast_error_t *e = (ast_error_t *)node;
    e->message = strslice_from_cstr(msg);  // 注意：msg 须是静态/arena 生命周期
    return node;
}
```

## 6. 文件布局

```
include/parser/
  parser.h           — parser_t + 公开 API（create/destroy/parse/error）
  parse_stmt.h       — parse_stmt / parse_block / parse_var_def / parse_if / ...
  parse_expr.h       — parse_expr / parse_expr_prec / parse_unary / parse_primary
  parse_utils.h      — cur_token / advance / match_*/expect_*/check_*/skip_trivia

src/parser/
  parser.c           — parser_create / parser_destroy / parser_parse / parse_program
  parse_stmt.c       — 所有语句解析实现
  parse_expr.c       — Pratt parser 实现
  parse_utils.c      — 辅助函数实现
```

## 7. 设计约束

1. parse 函数**不直接调用 malloc/free** — 所有内存走 alloc 或 arena
2. parse 函数**不关闭/释放 tokens/arena** — 生命周期由 driver 管理
3. **tok_begin 在函数入口保存，tok_end 在构造节点时取 p->pos** — 统一模式
4. **expect 失败不推进游标** — 构造 AST_ERROR 向上传播终止解析
5. **语法阶段 fail-fast** — 一个错误即终止，不做 panic mode 恢复
6. **语义阶段可恢复** — AST 结构完整，可以语句为单位跳过子树继续分析
7. parse_xxxx 的子节点赋值通过强制转换 `(ast_xxx_t *)node` 完成，不复用 ast_new 中央工厂
8. **不匹配时恢复游标** — `p->pos = tb`，返回 NULL，让父节点 failback
9. **错误时不恢复游标** — 返回 AST_ERROR 节点，向上传播终止解析
10. **dispatch 用 check_* 不用 match_*** — 不在分派阶段消费 token，由具体 parse 函数自行消费首 token
11. **首 token 是 mismatch/error 的分水岭** — 匹配并消费首 token 后 commit，后续失败都是 error
12. **父节点负责跳过 trivia** — 子节点的入口和出口都应指向有效 token（非空白/非注释），parse_program 入口自行 skip_trivia

## 8. 歧义消解：统一生成式

当多个构造共享首 token、无法在入口区分时，**不用独立的 parse 函数分别尝试**，
而是用一个统一式 parse 函数解析到分歧点，再根据上下文分支。

### 8.1 核心原则

```
能首 token 区分 → 独立 parse 函数 + dispatcher check
不能首 token 区分 → 统一式 parse，解析到分歧点再分支
```

### 8.2 经典案例：func 的三种角色

```
func add(a:i32, b:i32):i32 { return a+b; }    // 函数定义（顶层语句）
var add = func(a:i32, b:i32):i32 { ... };      // 函数字面量（表达式）
type fn_t = func(i32, i32)->i32;               // 函数类型（类型构造，M2+）
```

三个 `func` 含义完全不同，但首 token 都是 `func`。不能用 `parse_func_def` 独占 `func`。

**统一式解法**：

```c
/* parse_func_like: 遇到 func 时统一入口
 *
 * 上下文决定语义：
 *   - parse_program 调用 → 函数定义 → AST_FUNC_DEF
 *   - parse_expr    调用 → 函数字面量 → AST_FUNC_LIT（M2+）
 *   - parse_type    调用 → 函数类型 → AST_FUNC_TYPE（M2+）
 *
 * M1 只需处理函数定义和函数字面量。
 */
ast_node_t *parse_func_like(parser_t *p, ast_kind_t expected_kind) {
    uint32_t tb = p->pos;
    if (!check_keyword(p, "func")) { p->pos = tb; return NULL; }
    advance(p);  /* commit: 消费 "func" */

    /* 函数定义：func name(params):type { body }
     * 函数字面量：func(params):type { body }   (无 name)
     * 函数类型：  func(params)->type            (无 body)
     *
     * 共同前缀：func 后跟 ( 或 标识符
     * 分歧点：name 是否存在、) 后跟 : 还是 ->
     */

    strslice_t name = STRSLICE_EMPTY;
    if (check_kind(p, TOKEN_TYPE_IDENTIFIER) && !check_symbol(p, "(")) {
        /* 有 name 且不紧跟 ( → 函数定义 */
        name = token_strslice(cur_token(p));
        advance(p);
    }
    /* 否则：函数字面量或函数类型，无 name */

    expect_symbol(p, "(");
    /* 解析参数列表... */
    expect_symbol(p, ")");

    if (expected_kind == AST_FUNC_DEF) {
        /* 顶层函数定义：:type { body }（返回类型必选，不允许隐式 void） */
        strslice_t ret_type = STRSLICE_EMPTY;
        if (match_symbol(p, ":")) {
            ret_type = parse_type_text(p);
        }
        ast_node_t *body = parse_block(p);
        ast_node_t *node = ast_func_def_new(p->arena, tb, p->pos);
        ast_func_def_t *n = (ast_func_def_t *)node;
        n->name        = name;
        n->return_type = ret_type;
        n->body        = body;
        return node;
    }

    /* M2+: 函数字面量 / 函数类型 */
    return make_error_node(p, tb, "func literal not supported in M1");
}
```

### 8.3 案例二：赋值 vs 表达式语句

`add = expr;` vs `add(args);` — 首 token 都是标识符，无法区分。

**统一式解法**：先解析左值表达式，再看后接 token 分派：

```c
ast_node_t *parse_assign_or_expr_stmt(parser_t *p) {
    uint32_t tb = p->pos;

    /* 统一：先解析一个表达式 */
    ast_node_t *expr = parse_expr(p);
    if (!expr) return make_error_node(p, tb, "expected expression");

    /* 分歧点：表达式后跟赋值运算符？ */
    if (check_symbol(p, "=")  || check_symbol(p, "+=") ||
        check_symbol(p, "-=") || check_symbol(p, "*=") ||
        check_symbol(p, "/=") || check_symbol(p, "%=")) {
        /* 赋值语句 */
        if (expr->kind != AST_IDENT) {
            return make_error_node(p, tb, "invalid assignment target");
        }
        strslice_t name = ((ast_ident_t *)expr)->name;
        const char *op_sym = token_text_str(cur_token(p));
        int op = op_symbol_as_int(cur_token(p));
        advance(p);

        ast_node_t *value = parse_expr(p);
        if (!value) return make_error_node(p, tb, "expected expression after assignment");

        expect_symbol(p, ";");
        ast_node_t *node = ast_assign_new(p->arena, tb, p->pos);
        ast_assign_t *n = (ast_assign_t *)node;
        n->name  = name;
        n->op    = op;
        n->value = value;
        return node;
    }

    /* 表达式语句 */
    expect_symbol(p, ";");
    ast_node_t *node = ast_expr_stmt_new(p->arena, tb, p->pos);
    ((ast_expr_stmt_t *)node)->expr = expr;
    return node;
}
```

### 8.4 案例三：discard 语句

`_ = expr;` — `_` 是标识符但语义是丢弃。语法阶段不特殊处理，统一为 `AST_ASSIGN(name="_")`；语义分析时检查左值是 `_` 则丢弃值返回 void。

### 8.5 总结

| 歧义场景 | 共同首 token | 统一式函数 | 分歧点 |
|----------|-------------|-----------|--------|
| 函数定义 vs 函数字面量 vs 函数类型 | `func` | `parse_func_like` | name 有无、`)` 后 `:` vs `->` |
| 赋值 vs 表达式语句 | 标识符 | `parse_assign_or_expr_stmt` | 表达式后 `=` / `+=` 等（赋值已在 Pratt parser 中处理） |

统一式的优点：
- **无需回溯**：解析到分歧点时上下文已足够判断，不浪费已解析的结果
- **自然扩展**：M2 加入函数字面量只需在 `parse_func_like` 增加分支
- **与 mismatch/error 范式兼容**：统一式函数首 token 匹配即 commit
