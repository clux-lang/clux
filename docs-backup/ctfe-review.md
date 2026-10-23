# CTFE 审查（2026-10）与泛型单态化就绪度

> 审查动机：用户指出"CTFE 的代码也需要审查一下，未来泛型单态化的重头戏"。
> 审查范围：`src/ctfe/**`。**2026-10-22 复核**：`ctfe.c` 148 行、`ctfe_call.c` 222 行、
> `ctfe_expr.c` 968 行 / 29 个 `case AST_*`、`ctfe_stmt.c` 330 行 / 14 个 case。
> 结论先给：**CTFE 与 2026-10 的 VM 所有权模型天然一致，无需改动；作为单态化
> 地基的核心能力（类型值作为一等值传递/返回）已经具备。** 需要补的是"结构可
> 扩展性"与"实例化基础设施"，见 §3。
>
> 引用约定：正文只写函数名 / 分支名，不写行号——行号会随重构漂移，函数名可直接 grep 复核。

---

## 1. 与 VM 所有权模型的一致性（逐项核对）

| 检查项 | 结论 | 证据 |
|---|---|---|
| 是否存在"孤立于作用域的 value" | **否** | `src/ctfe/**` 中 `value_alloc_shell` / `value_dispose` / `scope_untrack` / 手工 `allocator_free` **零出现**；全部经 `value_make*` / `scope_define` / `value_clone` |
| 块级作用域 | **有** | `ctfe_eval_stmt` 的 `AST_BLOCK` 分支：`vm_push_scope` → 逐语句 → `ctfe_hoist_return` → `vm_pop_scope` |
| for 头变量作用域 | **有** | `ctfe_eval_stmt` 的 `AST_FOR` 分支：一个 scope 覆盖 init 与 body（与语言语义一致） |
| 错误路径的作用域平衡 | **有** | 每条 error 分支先 `vm_pop_scope` 再 return（`ctfe_eval_stmt` 多处）；`ctfe_call_ast_fn` 的 cleanup 标签同理 |
| 返回值跨帧移交 | **copy，且顺序正确** | `ctfe_call_ast_fn` 的返回段：**先**把 `ctx->ret_value` clone 进 caller scope，**再** pop callee scope；并显式管理 `vm->current_scope`（注释点明"误 pop caller 会销毁调用方作用域"） |
| 嵌套块中的 return 值 | **逐层上移** | `ctfe_hoist_return`（块 pop 前调用）——与 VM 侧 `RET = copy` 同一语义 |
| error 值的跨帧存活 | **hoist 到父作用域** | `ctfe_hoist_error`（`ctfe.c`）：`value_clone` 到 `scope->parent` 后返回；7 处调用点一致 |

**与 `func_vcall` 步骤 7 的对称性**：VM 侧返回值在调用方作用域 `value_clone`；
CTFE 侧同样"pop 前 clone 到 caller"——两套解释器在"返回值 = 拷贝"上语义一致 ✓。

## 2. 单态化地基：类型值是一等值（已具备）

| 能力 | 状态 | 证据 |
|---|---|---|
| 函数参数为类型值 | **支持** | `ctfe_eval_func_def`：形参类型表达式经 `ctfe_eval` 求值为 `TYPE_KIND_TYPE` 后 `value_as(pt, const type_t *)` |
| 函数返回类型值 | **支持** | 同上：`return_expr` 走同一求值/校验路径（非 `TYPE_KIND_TYPE` 报错） |
| `const` / `volatile` / `typeof` / `sizeof` 产出类型值 | **支持** | `ctfe_eval_inner` 的 `AST_CONST` / `AST_VOLATILE` / `AST_TYPEOF` / `AST_SIZEOF` 分支（`type_const_intern` / `type_volatile_intern` / `type_as_value`） |
| 类型值参与常量编码/折叠 | **支持** | `sema_ct_type_ref` / `sema_ct_lit`（`sema/comptime.c`） |
| 调用点求值范围 | `AST_FUNC_DEF` 体 + cfunc 内建 | `ctfe_call_value`（含"程序函数引用"回落分支，第 21 项修复） |
| DoS 保护 | `budget` 每表达式/语句递减 + 循环单独检查；`max_depth` 128 | `ctfe_eval`（`ctfe.c`）与 `ctfe_eval_stmt` 顶部的 budget/depth 检查 |

**推论**：`Vec(T)` 这类"类型参数 + 类型返回"的计算在 CTFE 层**已经可表达**；
单态化缺的是**实例化基础设施**（见 §3-B/C/D），不是求值能力。

## 3. 结论与待办（按优先级）

### A. 结构可扩展性（建议在加泛型**之前**做）
`ctfe_expr.c` 是单个 968 行 / 29-case 分发器，`ctfe_stmt.c` 330 行 / 14-case。
单态化必然新增 case（类型实参绑定、实例化调用、mangling 相关）。
**建议**：照 sema 的 `sema_expr`（第 17 项：拆分成分派 + 每 AST kind 一个处理器）的做法，
按 AST kind 抽出处理器 + `ctfe_guard` 之类基础设施（`is_poison` 的 CTFE 对应物）。
现在做是纯搬移（低风险），等泛型落地后再做代价高得多。
（2026-10 复核：A 项仍未做，`ctfe_expr.c` 已从 709 行增长到 968 行。）

### B. 实例化缓存/去重表（缺失）
今天没有 `(泛型定义, 类型实参) → 实例` 的表。单态化需要：
mangling 方案（`sema_type_t::name`/`type_id_alloc` 是天然锚点）、实例缓存、
以及"同一实参组合只实例化一次"的去重。
**建议位置**：与 `sema->funcs` / `sema->types` 并列（sema 拥有实例的生命周期），
CTFE 只负责**求值类型实参**，不持有实例表。

### C. 实例化层面的递归保护（缺失）
`budget` / `max_depth` 保护的是**求值**；`Vec(Vec(Vec(...)))` 这类递归实例化需要
**实例化层**的深度/预算保护。**建议**：复用同一套模式（ctx 上的 budget/depth
计数 + 明确诊断），并在 mangling 后先查缓存再递归，避免无限展开。

### D. 实例的 AST 表示（已定稿）
设计已定稿为**替换类型后的 `ast_func_def_t` 副本**（见 `docs/m7-design.md` §9/§14）：
类型槽位替换为具体 `AST_TYPE_REF`，函数体优先深拷贝（避免实例间共享 sema 状态），
别名不产生副本、CTFE 求值到具体类型即可。

### E. 诊断可定位性（UX）
CTFE 错误目前是无位置信息的字符串（经 error value 传播）。单态化报错必须能指出
**泛型定义位置 + 实参类型**。建议在实例化时把 (定义点, 实参) 记入诊断上下文，
而不是把字符串拼进消息。

---

## 4. 本次审查没有发现需要立即修改的缺陷

- 无内存泄漏路径（CTFE 不使用脱离作用域的值；错误值/返回值均按顺序 hoist/clone）。
- 无作用域失衡（每条 error 分支都 pop；调用帧显式管理 `current_scope`）。
- 与 2026-10 VM 所有权重构（第 23/24 项）**无需联动改动**——CTFE 本来就走
  `value_make*` / `scope_define` / `value_clone`，从未使用 `is_fresh`/`untrack`。

上表 §1/§2 的每一项都可用**函数名 grep** 复核；§3 的 A–E 是单态化动工前的准备清单，
其中 **A 与 B 建议优先**（A 是纯结构搬移，B 是单态化的第一个必需构件）。

> 2026-10-22 复核：所有权升级（P0–P3f）后 CTFE 仍无需联动改动；A/B/C/E 仍未做，
> D 已由 `m7-design.md` 定稿。
