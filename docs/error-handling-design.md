# M8 错误处理：`.?` 与 struct `.!` 语法糖

> 状态：**已实现（2026-10-22）**。clux 不引入 throw/异常；错误是普通的
> result struct 值，`.?` / `.!` 只是两个展开为 `if` 的语法糖。

---

## 1. result struct 协议

一个可被 `.?' / `.!' 使用的左值必须是 struct，并提供：

```clux
struct R {
    okf: bool;
    v: i32;
    e: i32;

    func ok(self: ref *const R): bool  { return self.okf; }
    func value(self: ref *const R): i32 { return self.v; }
    func error(self: ref *const R): i32 { return self.e; }
    func panic(self: ref *const R): str { return "result error"; }

    func of_error(e: i32): R { ... }   // 当前函数的返回类型上的静态构造
}
```

- `ok()` 返回 `bool`
- `value()` 返回成功值（可 copy）
- `error()` 返回错误值，**必须可 copy**（own/fatal/含 own 槽位报错）
- `panic()` 返回 `str`（panic 分支给内建 `panic` 用）
- 当前函数返回类型必须是 struct，且提供 `of_error(error)` 静态方法

---

## 2. `b = a.?`

展开为：

```clux
if (a.ok()) {
    b = a.value();
} else {
    return <当前函数返回类型>::of_error(a.error());
}
```

`a` 必须是简单的 place 表达式（变量/字段），不能是有副作用的调用。

## 3. `b = a.!`（struct 左值）

展开为：

```clux
if (a.ok()) {
    b = a.value();
} else {
    panic(a.panic());
}
```

optional 的 `x.!` **语义不变**（assert unwrap）；只有当左值静态类型是
struct 时才走 result 语法糖。

## 4. `var b = a.?` / `var b = a.!`

展开需要先声明再赋值：

```clux
var b: T = undefined;
if (a.ok()) {
    b = a.value();
} else {
    return Ret::of_error(a.error());   // 或 panic(a.panic())
}
```

实现上：

- `shadow_var_def` 把原 var 改成 `var b: T = undefined`；
- 生成的分支挂在该语句的 **postlude**（compiler 在 `DEFINE b` 之后编译）；
- 分支完成后 `b` 标记为已初始化（两个可达路径都赋值，或 else 终止）。

赋值形式 `b = a.?` 的分支挂在 **prelude**（赋值语句本身变为 no-op），
目标变量必须是已声明、可 copy 的简单变量。

左值可以是任意表达式；展开时先绑定临时变量，保证只求值一次：

```clux
var __try_N: R = half(x);
if (__try_N.ok()) { ... } else { ... }
```

因此 `var v = half(x).?;` 可以直接写，`half(x)` 只调用一次。

## 5. panic

`panic` 是内建函数：

```clux
panic(msg: str);   // 打印 "panic: <msg>" 到 stderr，然后以退出码 1 结束
```

- VM 内建 `builtin_panic.c`，不是 clux 用户函数；
- 编译器按普通 `CALL` 调用内建 fid；
- sema 把 `panic(...)` 标记为 **never return**（`SEMA_BUILTIN_PANIC`）；
- 终止用 C `_Exit(1)`：不跑 atexit、不弹 Windows abort 对话框，退出码
  与 Linux `_exit(1)` 一致。

## 6. `[[noreturn]]` / `[[never]]` 标记装饰器

“不返回”是通用的函数属性，不是 panic 的专属行为。内建标记装饰器：

```clux
[[noreturn]] func fail(): void { panic("custom fail"); }
[[noreturn]] func fail_generic[T](): void { panic("generic fail"); }
```

- desugar 识别并摘除该标记，写 `ast_func_def_t::is_noreturn`；
- sema 把它复制到函数符号；调用该函数（含显式泛型 `fail[i32]()`）
  的表达式语句返回 `definitely_returns = true`，不参与 TDZ 汇合；
- 标记必须是**唯一**装饰器（与普通包装装饰器混用会报错，因为包装后的
  函数值不再具有“不返回”属性）；
- 泛型函数/泛型 struct 方法也支持该标记。

`panic` 内建函数同样通过 `is_noreturn` 标记参与 TDZ，不再有独立的
flow 特殊分支。

## 7. TDZ / never-return 修正
原来的 if 汇合规则是“两个分支都初始化才初始化”。panic 分支不会继续
执行，因此不能参与汇合：

```clux
var b: i32 = undefined;
if (r.ok()) { b = r.value(); } else { panic("bad"); }
return b;   // 合法：else 不落到 join
```

`walk_if` 的 flow meet 现在按可达分支计算：

- then 终止、else 可达 → 只取 else 的 flow；
- else 终止、then 可达 → 只取 then 的 flow；
- 两支都终止 → 整个 if definitely_returns；
- 否则仍按 AND 汇合。

`panic(...)` 作为表达式语句时，`walk_stmt` 直接返回
`definitely_returns = true`。

## 8. 错误返回路径的资源释放

`.?` 的错误分支是真正的 return，必须执行当前作用域已拥有资源的
释放。生成的 `AST_RETURN` 会挂上 `collect_free_stmts` 的结果；
`compile_stmt` 的 return 路径会先 materialize 返回值，再执行 FREE。

## 9. 约束与未纳入

- `error()` 的返回值禁止 own/fatal/含 own 槽位（语法糖要求可 copy）。
- `.?` 只用于 result struct；optional 的 try 语义仍未定义。
- 赋值形式只支持简单变量目标；`a.field = x.?` 后续再扩展。
- value 为 own/fatal 的 result 不做 `.?' 绑定（需要 move 语义另行设计）。
- 没有 throw/异常栈；错误就是值。

## 10. 泛型 Result[T, E] 原型

`examples/result/result.cx` 验证了标准库 Result 的可行形态：

```clux
struct Result[T, E] {
    good: bool;
    val: ?T;
    err: ?E;

    func ok(self: ref *const Result[T, E]): bool { return self.good; }
    func value(self: ref *const Result[T, E]): T { return self.val.!; }
    func error(self: ref *const Result[T, E]): E { return self.err.!; }
    func panic(self: ref *const Result[T, E]): str { return "Result error"; }

    func of_value(v: T): Result[T, E] {
        return .Result[T, E]{ .good = true, .val = .?T{ v }, .err = nil };
    }
    func of_error(e: E): Result[T, E] {
        return .Result[T, E]{ .good = false, .val = nil, .err = .?E{ e } };
    }
}
```

使用：

```clux
func half(x: i32): Result[i32, str] {
    if (x < 0) { return Result[i32, str]::of_error("negative"); }
    return Result[i32, str]::of_value(x / 2);
}

func use(x: i32): Result[i32, str] {
    var v = half(x).?;   // 临时变量 __try_N 接住 half(x)，只求值一次
    return Result[i32, str]::of_value(v + 1);
}
```

结论：泛型 Result 可以工作，`of_value`/`of_error` 作为泛型 struct 的
静态方法可以按 `Result[T,E]` 实例化；`.?' 能直接消费它。

已知限制：

- 两个 payload 用 `?T` / `?E` 表示，构造一侧时另一侧写 `nil`；
- `panic()` 只能返回固定字符串（泛型 E 无法通用转 `str`，除非标准库
  给 E 增加 interface 约束/格式化协议）；
- 静态构造需要显式 `Result[T,E]::of_value(...)`，暂不做返回类型反向推导。

## 11. 实现落点

- `src/sema/stmt_try.c`：协议分析、临时变量绑定、生成 `if`、`of_error`/panic 分支
- `src/sema/stmt_var.c`：var 形式，预声明 `undefined` + postlude
- `src/sema/stmt_assign.c`：赋值形式，prelude + `sugar_noop`
- `src/sema/stmt_flow.c`：可达分支 flow meet + panic noreturn
- `src/desugar/desugar.c`：摘除 `[[noreturn]]` / `[[never]]` 标记
- `src/sema/stmt.c`：按 `sema_symbol_t::is_noreturn` 标记终止分支
- `src/vm/builtin_panic.c`：stderr + `_Exit(1)`（无 abort 弹窗）
- `src/compiler/compile_stmt.c`：prelude/postlude 与 sugar no-op
- `tests/error_handling_test.cpp`
