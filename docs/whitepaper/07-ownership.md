# 7. 所有权、生命周期与清理

clux 没有 GC。值要么是平凡值，要么携带显式所有权槽位；所有权在类型上表达。

## 7.1 所有权修饰

```text
own T      拥有 T
ref T      借用 T
fatal T    必须被消费的 T
share T    引用计数共享
weak T     弱引用
```

指针和切片：

```clux
own *File
ref *const User
fatal []u8
share *Node
weak *Node
ref []const u8
```

`own` 值在离开作用域时释放；`ref` 不拥有，不负责释放。

## 7.2 初始化与移动

```clux
var p: own *User = new User{ .name = "a" };
var q: own *User = move(p);   // p 进入已移动状态
```

移动后原变量不能再读取，直到重新赋值。

```clux
p = new User{ .name = "b" };  // 重新初始化后可用
```

## 7.3 借用

```clux
func show(u: ref *const User): void { ... }

var p: own *User = new User{ .name = "a" };
show(p);
```

- `ref` 借用不转移所有权。
- 借用的生命周期不得超过被借用值。
- 拥有型值在仍有活跃借用时不能被移动或释放。

## 7.4 fatal

`fatal T` 表示必须被消费的值，通常来自：

```clux
new T{...}       // fatal *T → 由变量/字段/参数接管为 own
make(T, N, ...)  // fatal []T
move(x)
clone(x)
```

`fatal` 值必须被绑定到 `own` 槽位、传给接收 `own` 的参数、或作为返回值转交。

把将亡值传给 `ref` 或非 `own` 形参是编译错误：调用结束没有任何位置接住这块
资源（临时值本来由调用者负责销毁）→ 泄漏。想借用就先把它绑到 `own` 变量：

```clux
struct Box { p: own *i32; }

func peek(b: ref *Box): i32 { return b.p.*; }   // 借用
func take(b: own Box): i32 { return b.p.*; }    // 接管

var b: own Box = .Box{ .p = new i32{7} };
peek(b.&);                                      // 借用变量
take(.Box{ .p = new i32{8} });                  // fatal → own 形参：合法
peek(.Box{ .p = new i32{9} });                  // 编译错误：fatal → ref 形参
```

## 7.5 share / weak

```clux
var a: share *Node = undefined;
var b: share *Node = a;       // 引用计数增加
var w: weak *Node = a;        // 弱引用
```

- `share` 通过引用计数管理生命周期。
- `weak` 不增加强引用计数；访问弱引用前必须升级。
- `share` / `weak` 是真实类型，不是 `own` 的别名。

## 7.6 作用域清理

离开块时，作用域内的 `own` 变量按声明的逆序清理：

```clux
{
    var a: own *A = new A{};
    var b: own *B = new B{};
    // 退出时先释放 b，再释放 a
}
```

清理包括：

- 释放 `own` 指针；
- 递归释放拥有型聚合的 own 字段；
- 递减 `share` 引用计数；
- 执行 `defer` 块。

## 7.7 defer

```clux
var f: own *File = open(...);
defer {
    close(f);
}
```

`defer` 在离开作用域时执行，后进先出。`defer` 是清理逻辑，不是异常机制。

## 7.8 赋值与拷贝

- 平凡值可以直接赋值。
- 拥有型值不能隐式复制；需要 `move` 转移或 `clone` 深拷贝。
- `clone` 的结果是独立拥有型值。

```clux
var b = clone(a);
```

“拥有型”按**是否携带 own 槽位**判定，不只看顶层修饰：

- `own *T` / `own []T` 是顶层拥有型；
- `?own *T`、含 own 字段的 `struct`、`[N]own *T` 等**嵌套拥有型**同样是
  move-only：不能复制到新的存储位置（初始化 / 赋值 / 字段 / 元素 / 参数
  都适用）；要复制只能 `clone`。

从借源读取这类值得到的是**视图**，不是所有权：例如

```clux
struct Node { n: ?own *Node; }

func walk(q: ref *Node): void {
    var p = q.n;        // 编译错误：不能把借源视图复制成新的拥有位置
    var v: ref *Node = q.n.!;  // 正确：借用视图
}
```

视图只能绑定到 `ref`；需要独立所有权时 `clone(q.n)`。

## 7.9 字段与元素

结构体、数组、元组中的 own 槽位随聚合值一起清理。把值放入容器时，所有权
按槽位规则转移。

## 7.10 借用与闭包

闭包捕获拥有型值时要显式转移：

```clux
var f = func |(p: own *User = move(user))| run(): void { ... };
```

借用捕获需要作用域标注，保证闭包不会超过被借用值：

```clux
var f = func '<user> |u| run(): '<user> void { ... };
```

## 7.11 与 C ABI

所有权修饰可以出现在 C ABI 签名里，但它们**只是 clux 侧的契约**，不改变二进制
表示：

- `own *T` / `own opaque` / `own Agg`：所有权转移给 C 侧，C 侧负责释放（`own Agg`
  表示按值聚合里的 own 槽位由被调方释放）；**调用点必须用 `move(...)` 或
  `clone(...)`**，不能直接传普通左值。
- `ref *T`：借用，C 侧不得释放或保存；调用方保证活到调用结束。
- 返回位置用 `fatal T` 把所有权交回调用方（返回位置不允许写 `own`）。
- `share *T` / `weak *T` 不能跨 C ABI：它们指向 RC 块内的 payload，C 侧无法
  retain/release。需要传递时改用 `ref *T` / `fatal *T` 或 `opaque` 句柄。
- `str` 在 C ABI 里是 `const char *`（借用视图，C 侧不释放）；含 `str` 字段的
  聚合不能按值跨 C ABI。
