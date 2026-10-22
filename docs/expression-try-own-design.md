# `.?' 作为表达式 + own payload 的可行性研究

> 状态：设计研究（2026-10-23）。当前 `.?' / struct `.!' 是**语句级**
> 语法糖（var / 简单赋值），见
> [error-handling-design.md](error-handling-design.md)。

---

## 1. 当前实现为什么限制在赋值语句

现在 `.?' 展开成：

```clux
var __try_N: R = <左值表达式>;
if (__try_N.ok()) {
    target = __try_N.value();
} else {
    return Ret::of_error(__try_N.error());
}
```

错误分支是**语句** `return`，所以只能挂在 var / 赋值语句上：

- 需要当前函数返回类型来构造 `Ret::of_error`；
- 需要一个赋值目标 `target`。

这正好绕开了“表达式里出现 return”的问题，但也导致：

- `foo(r.?)`、`return r.?`、`if (r.?)` 不支持；
- own payload 无法自然处理。

---

## 2. 表达式化：技术上可行

把 `.?' 变成 `AST_TRY` 表达式节点，sema 生成：

```text
var __try_N: R = <operand>;   // 每次求值一次
__try_N.ok()
  ? __try_N.value()           // 成功：表达式结果
  : <early return Ret::of_error(__try_N.error())>
```

compiler 对 `AST_TRY` 发条件跳转 + **表达式内 early RET**：

```text
  <eval temp init>; DEFINE __try_N
  CALL __try_N.ok
  JZ fail
  CALL __try_N.value        ; 成功值留在栈顶
  JMP end
fail:
  CALL __try_N.error
  CALL Ret::of_error
  <defers / FREE>
  RET
end:
```

### 为什么 return 在表达式里可以成立

- VM 的 `RET` 会结束当前调用帧，帧内的操作数栈临时值**整体丢弃**；
  外层表达式尚未完成的其他值不需要手工 POP。
- compiler 只需要在**编译期**保存/恢复 `stack_depth`：
  - 进入 AST_TRY 时记录 base；
  - 成功路径结束为 base+1；
  - 失败路径编译完 RET 后，把编译期栈深度恢复到 base+1，
    让父表达式继续按“产生一个值”处理。
- 错误返回值的析构/defer 与普通 return 相同：sema 给 `AST_TRY`
  挂上当前作用域的 FREE 链，compiler 在 RET 前编译。

### 临时变量的作用域

`__try_N` 不能用当前语句的 prelude（循环条件里的 `.?' 会只求值一次）。
表达式化后需要**表达式局部临时**：

```text
PUSH_SCOPE
  <operand>; DEFINE __try_N
  ...try 分支...
POP_SCOPE
```

成功值留在操作数栈上，`POP_SCOPE` 只销毁临时作用域，不会弹结果值；
失败路径 RET 丢弃整个帧，临时作用域随之消失。

---

## 3. own payload 才是真正的难点

表达式化本身可以解决 copyable Result：

```clux
var v = half(x).?;      // T 可 copy
return half(x).?;       // 表达式本身产生 T
```

但 own `T` 不能靠现有 `value(self: ref *const R): T` 协议：

```clux
struct Result[T, E] {
    val: ?T;            // T = own *X
    func value(self: ref *const R): T {
        return self.val.!;   // ✗ 不能 move 聚合字段
    }
}
```

这不是 `.?' 语法的问题，而是所有权模型一直坚持的规则：

- **局部变量/聚合字段不能单独 move 出去**；
- 只有整个值可以 move；
- 因此 own payload 不能通过“读取字段并返回 T”转移。

### 需要的能力：消费整个 Result

建议为 own Result 增加消费式协议：

```clux
func into_value(self: own Result[T, E]): fatal T;
func into_error(self: own Result[T, E]): fatal E;
```

`.?' 对 own payload 的展开变成：

```clux
if (__try.ok()) {
    <expr result> = into_value(move(__try));  // 消费整个 Result
} else {
    var e = into_error(move(__try));
    return Ret::of_error(e);
}
```

- `move(__try)` 移动的是**整个聚合临时**，符合现有所有权规则；
- `into_value` 在函数内部丢弃 error 侧，返回 `fatal T`；
- `.?' 的结果类型是 `fatal T`，外层按 move 消费；
- 失败分支同理，`into_error` 消费整个 Result 并交出 E。

这要求：

1. Result 的 own 版本提供 `into_value` / `into_error`（或一个统一的
   `take()` 返回 tagged 结果）；
2. `.?' 根据 `T` 是否含 own 槽位选择 `value()` 或 `into_value(move(temp))`；
3. `into_value` 只能是方法（需要访问整个 self），不能是普通字段读取。

### lvalue 的消费语义

```clux
var r: Result[own *X, E] = ...;
var v = r.?;
```

如果 r 是 own Result，`.?' 必须显式 move：

```clux
var v = move(r).?;
```

否则 `r` 本身还有所有权，不能同时被消费。对 copyable Result
`r.?` 仍是借用/拷贝，不需要 move。

---

## 4. 编译器改动点

- 新 AST kind `AST_TRY`：
  - `temp`（表达式局部变量名/R）；
  - `ok_call` / `value_call`（或 `into_value_call`）；
  - `error_call` / `of_error_call`；
  - `early_defers`（FREE 链）；
  - `panic` 变体（`.!`）。
- sema：
  - 表达式上下文生成 temp + AST_TRY；
  - 检查当前函数返回类型/of_error；
  - own 时选消费协议并插入 `move(temp)`；
  - 生成 FREE/defers。
- compiler/compile_expr：
  - 保存 base stack depth；
  - 发 JZ/JMP/RET；
  - 表达式局部 `PUSH_SCOPE`/`POP_SCOPE`；
  - 处理 early RET 前 defers；
  - 成功路径栈 +1。
- 现有语句级展开可以先保留，等 AST_TRY 稳定后替换为统一表达式。

---

## 5. 结论

- **copyable Result 的 `.?' 表达式化：可行**，主要是 AST/compiler
  控制流和栈深度管理，不需要改 VM opcode。
- **own payload：可行，但前提是消费整个 Result**：
  - 不能把 `value()` 设计成从聚合字段 move；
  - 需要 `into_value(self: own R): fatal T` / `into_error(...)`；
  - `.?' 对 own 左值要求 `move(r)`，并消费整个 Result。
- “表达式里 return”不是不可逾越的问题：VM RET 会丢弃当前帧的临时值，
  compiler 只需维护编译期 stack_depth，并把 FREE/defers 当作普通
  return 处理。
- 下一阶段建议顺序：
  1. 实现 copyable 的 `AST_TRY` 表达式（不改变现有 Result 协议）；
  2. 给 Result 增加 own 消费协议；
  3. 再打开 own `.?'，并明确 `move(r)` 的调用点语义。
