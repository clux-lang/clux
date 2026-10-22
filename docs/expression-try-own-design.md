# `.?' 作为表达式 + own payload 的可行性研究

> 状态：copyable Result 的 **AST_TRY 表达式 + early RET 已实现**
> （2026-10-23）；`take_opt` own 原语已实现；own Result 的泛型
> `value(): T` 仍受返回所有权限制。设计见
> [error-handling-design.md](error-handling-design.md)。

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

## 2. 表达式化：已实现（copyable）

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

## 4. 编译器改动点（已落地）

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

## 5. 备选方案：`?own T` 的运行时 take（已实现原语）

另一个更局部的方案是给 optional own 增加一个**原地 take** 操作：

```text
take_opt(x: lvalue ?T) -> T
  T 普通类型       -> 返回 T 的拷贝，x 置 nil
  T own / 含 own 槽位 -> 返回 fatal T（移出 payload），x 置 nil
  x == nil        -> panic
```

实现为 `BCODE_OPT_TAKE`：

- 弹出 optional 的**借用视图**（变量/字段读取天然是借用）；
- some：把 payload 字节移到新值，源 tag 置 `false`，源 value 清零；
- none：运行期错误 `panic: take on none optional value`；
- own payload 用 `type_ownership_intern(FATAL, core)` 生成 `fatal T`；
- 普通 payload 直接返回 `T`。

它比 `into_value` 更小：

- 移动的是 optional 槽位里的整个 payload，不是聚合字段的局部 move；
- 清 tag 后，Result 析构时看到 `nil`，不会二次释放；
- 失败分支不调用 take，error 侧照常析构；
- 对 `?own *X`、`?own Struct`（payload 整体带 own 槽位）都成立。

Result 方法可以写成（self 必须是可变 ref，不能是 `ref *const`）：

```clux
func value(self: ref *Result[T, E]): T {
    return take_opt(self.val);
}
```

### 但它不能单独解决泛型 Result 的返回值所有权

如果 `T = own *X`，`value()` 的签名就是 `value(self): T`，实例化后是
“返回 own *X”——而 clux 的返回位只允许 ref/fatal/none，**own 返回类型
本身非法**。

因此 take 方案仍然需要下面之一：

1. 专门的 owned Result（例如 `ResultOwned[X, E]`，payload 是 `?own *X`，
   `take_value(self): fatal *X`），不让同一个 `Result[T,E]` 同时承担
   copyable 和 owned 两种返回类型；
2. 或 `.?' 在编译器里直接根据具体 `T` 生成 take，而不是通过泛型
   `value(): T` 方法；
3. 或引入“返回值所有权多态”，这会扩大语言模型，不建议仅为 Result 引入。

### 与表达式化的关系

take 只解决“own payload 怎么取出来”，**不解决 `.?' 只能在赋值语句里**。
表达式 `.?' 仍然需要 AST_TRY + early RET 方案。

建议落地顺序：

1. 实现 optional take 原语（`OPT_TAKE` / `take_opt(x)`）并测试 `?own *X`；
2. 用 `ResultOwned` 原型验证 own payload；
3. 再做 copyable 的表达式 `.?'；
4. 最后决定是否让 owned Result 也走统一 `.?' 语法。

## 6. 结论

- **copyable Result 的 `.?' 表达式化：已实现**（AST_TRY + early RET）；
- **`take_opt` own 原语：已实现**（`?T -> T` / `?own T -> fatal T`）；
- **own 的泛型 Result `value(): T`：仍不可行**，需要 `ResultOwned` 或
  编译器按具体 T 生成 take；
- “表达式里 return”已用 early RET 解决：VM RET 丢弃当前帧临时值，
  compiler 维护编译期 stack_depth + defers。
- 下一阶段：
  1. 用 `ResultOwned` 原型验证 own payload；
  2. 表达式 `.?' 在 own payload 上切到 `take_opt`；
  3. C 后端用语句表达式生成对应 early return。
