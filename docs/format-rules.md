# clux 格式化规则（clux format）

clux 附带一个源码格式化器（命令 `clux format <file>`，实现见 `src/parser/fmt.c`）。
它**不重排代码结构、不改写语义**，只规整空白、缩进与注释位置，并保证：

- **声明完整**：`union` / `cunion` / `struct` / `enum` / `type` / `import` / `export`
  等顶层构造一律原样保留（不丢声明、不丢 `export` 前缀）；
- **语义等价**：格式化后程序运行结果不变；
- **幂等**：对格式化结果再跑一次 `clux format`，输出逐字节不变。

> 实现说明：格式化器是**基于 AST 渲染 + token pool 提供注释/空行信息**的混合实现
> （`render_stmt` / `render_expr` 遍历 AST，`mount_comments` 从 token 池挂载注释），
> 因此"注释天然保留"来自 token 池，而结构来自 AST。
>
> 上述三条性质在 2026-10 曾全部被破坏（union/import/export 被静默删除、单遍不幂等），
> 现已修复并有回归测试覆盖 —— 见 §9。

---

## 1. 总体策略

1. **过滤 trivia**：忽略 `WHITESPACE`，仅保留实义 token（标识符 / 关键字 / 字面量 /
   符号 / 注释）。每个实义 token 记录其前导空白中是否出现过换行、空行。
2. **按 token 序列 + 简单上下文规则逐 token 输出**：空白完全由规则重新生成，
   源码中的原始缩进、连续空格、制表符等一律丢弃。
3. **缩进统一 4 空格**，不使用制表符；整体输出**以单个换行结尾**。

---

## 2. 块与缩进

| 场景 | 规则 |
|------|------|
| `{` | 跟随前行输出（前面补一个空格）；随后若是非空块则换行、下一行缩进 +1。 |
| 非空块 `{` | `}` 之前换行、`}` 单独占一行，并缩进 −1。 |
| 空块 `{` | 紧凑写成 `{}`（中间无空格），`}` 紧跟 `}` 输出。 |
| `} else` | `}` 与 `else` 同行，中间一个空格（`} else`）。 |
| `}` 后 | 若后随 `;`、`,`、`)`、`.`、`else` 则保持同行；否则换行。 |

---

## 3. 语句分隔

- **`;`** 后换行（独占一行结束）。例外：
  - 在 `()` 内部（如 `for (a; b; c)`）不换行，交给后续 token 的常规空格判定；
  - 行内注释（`;` 后紧跟同行的 `//` 注释）不换行，注释归属本条语句。
- **`,`** 后恒为一个空格，不换行。

---

## 4. 运算符与空格

- **二元运算符两侧留空格**（如 `a + b`、`x = (y)`、`p -> q`）。运算符集合
  见 `is_operator`：赋值类（`=`/`+=`/`*=`/…）、算术（`+ - * / %`）、比较
  （`== != < > <= >=`）、逻辑/位（`&& || & | ^ << >>`）、`!` `~` `->` `=>`。
- **运算符后的 `(` 须空格**：`a + (b)`、`x = (y)`，不得紧贴成 `+(` / `=(`。
- **函数调用 / 泛型实例化的 `(` 紧贴**：`main(`、`foo(`、`Vec(i32)` 不插空格。
- **控制流关键字的 `(` 须空格**：`if (cond)`、`while (cond)`、`for (...)`、
  `foreach (x of ...)`、`switch (...)`。注意 clux 的 `foreach`/`switch` 等软关键字
  在词法期以标识符身份产出，故按**文本**判定而非 `kind == KEYWORD`。

### 一元前缀运算符

`!` / `~` **永远是一元**，紧贴其后操作数（`!x`、`~a`）。

`-` / `+` 按上下文区分一元 / 二元：当其前置 token 处于"期待一元操作数"位置时
（`=` `:` `,` `;` `(` `[` 之后、另一运算符之后、`return` 之后、行首 / 表达式起始）
视为**一元**，紧贴操作数（`-x`、`= -x`、`(-x)`、`return -x`）；否则为二元，两侧留空格。

```clux
var neg: i32 = -sum;      /* 一元：紧贴 */
var bnot: i32 = ~a;       /* 一元：紧贴 */
var notb: bool = !yes;    /* 一元：紧贴 */
y = -(a + b);             /* 一元：紧贴；括号内为二元 */
var r: f64 = (a as f64) + b;   /* 二元：两侧空格 */
```

---

## 5. 数字类型后缀紧贴

词法器把数值与其类型后缀切成两个 token（如 `7i8` → `7` + `i8`，后缀由 parser 消费）。
若在此处插入空格会直接改变语义（`7 i8` 不再是字面量）。格式化器以**字节偏移**判定相邻性：
只有源码中本就紧贴的数字与后缀才保持紧贴（`7i8`、`2.5f32`）。

---

## 6. 紧贴符（无空格）

- **前无空格**：`)` `]` `,` `;` `.` `:`。
- **后无空格**：`(` `[` `.`（点成员访问）。
- 数字类型后缀（见上）。

---

## 7. 注释

- 注释**原样保留**，不做改写。
- **行内注释**（源码中与前一 token 同行）：前面一个空格，归属前一语句。
- **独立行注释**：按当前缩进输出；若源码中其前有空行，则输出一个空行以保留分组意图。
- 块注释（`/* */`）后若源码中有换行则换行；行内块注释保持同行。

---

## 8. 幂等性

格式化器幂等：对格式化后的结果再跑一次 `clux format`，输出逐字节不变。
相关测试：`tests/fmt_test.cpp` 的 `Fmt.Idempotent`、`FmtRegression.*`
（`IdempotentAfterStandaloneComment` / `IsOperatorParensStable` /
`IdempotentAcrossDeclKinds`），以及 examples 全量三遍格式化结果一致。

---

## 9. 历史缺陷与回归覆盖（2026-10）

格式化器曾存在三类**静默破坏源码**的缺陷，均已修复。记录在此便于追溯，并说明
为什么 §1–§8 的每条规则都值得一条回归测试。

### 9.1 顶层 union 声明被删除

`render_stmt` 缺 `AST_UNION_DEF` 分支，`union Name { ... }` 落到 `default:`
分支后只渲染出一个 `;`（类型声明整段消失，程序随即无法编译）。

修复：补 `AST_UNION_DEF` 渲染分支（与 `cunion` / `struct` 同款平铺渲染）。
回归：`FmtRegression.UnionDeclPreserved`、`CunionDeclPreserved`。

### 9.2 import 语句与 export 前缀被删除

同一处 `switch` 缺 `AST_IMPORT` 分支（`import x from "..."` 变成 `;`），
且**没有任何代码读取 `is_exported`**（parser 把 `export` 记为声明节点上的标志位，
不是独立节点）——`export func/var/union/struct` 的 `export` 前缀被整体丢弃，
模块导出表静默消失。

修复：补 `AST_IMPORT` 渲染分支（AST 中 path 已去引号，渲染时补回）；
`render_program` 在渲染每个顶层声明前输出 `is_exported` 对应的 `export ` 前缀。
回归：`FmtRegression.ImportStmtPreserved`、`ExportPrefixPreserved`。

### 9.3 单遍不幂等（两处独立根因）

**(a) 顶层分隔换行未检查行首状态**：`render_program` 每项前无条件补 `'\n'`，
而上一顶层项若以独立行注释结尾（注释自带换行），该分隔符就凭空多出一个空行；
第二遍的"保留空行"规则又会合法地补一个空行 → 输出收敛到两个空行，单遍不幂等。
影响 34 个 examples 中的 10 个（当时规模）。修复：改为 `if (!first && !f->at_line_start)`。

**(b) `is` 运算符未登记绑定力，且查表失败时未初始化**：括号决策表
（`op_binding_len`）漏了 `is`，而 `node_prec` 忽略返回值 —— `*lp/*rp` 保持
未初始化，`need_paren` 读出垃圾值（UB），导致同一文件两遍格式化括号结果不同
（`((e) is (radius))` vs `((e) is radius)`）。修复：表补 `{ "is", 21, 22 }`
（与 parser 的 `infix_binding` 一致），并让未登记运算符回落到原子优先级。

回归：`FmtRegression.IdempotentAfterStandaloneComment` /
`IsOperatorParensStable` / `IdempotentAcrossDeclKinds`。

### 9.4 当前已核对成立

- **声明完整**：`union` / `cunion` / `struct` / `import` / `export` 全量 examples 扫描后均保留。
- **语义等价**：examples 全量 `.cx`（当前 37 个：35 个入口 + 2 个模块库）格式化后运行输出与退出码不变
  （比较时排除运行期泄漏转储——其含地址，逐次不同）。
- **幂等**：examples 全量 `.cx` 连续三遍格式化结果逐字节一致（37/37）。
- **`.cxs` 汇编往返**：`examples/asm/*.cxs` 经 `bc asm → bc disasm → bc asm`
  两次 `.cxb` 逐字节一致（5/5）。
