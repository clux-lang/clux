# 4. 语句

## 4.1 变量声明

```clux
var x: i32 = 1;
var y = 2;
var z: ?i32 = nil;
```

规则见[声明](03-declarations.md)。局部变量属于当前块作用域。

## 4.2 赋值

```clux
x = 3;
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
```

目标必须是可写左值。复合赋值展开为“读取、运算、写回”。

**魔法赋值**：目标类型是结构体且定义了 `__assign__` 时，简单赋值
`a = b;` 展开为 `a.__assign__(b.&)`（返回 `void`，两个操作数都按指针传）；
复合赋值 `a += b;` 先展开为 `a = a + b`（`__add__` 派发）再按上一条派发
`__assign__`。成员 / 下标 / 解引用目标同样适用。

**`a = b.?`（try 糖）**展开成 `if (b.ok()) { a = b.value(); } else { ... }`，
then 分支的赋值同样派发 `__assign__`。

**初始化不派发**：`var a = b;`、参数绑定、字段构造、返回值都是
`value_assign`（复制/隐式转换），不会调用 `__assign__`。

```clux
struct V {
    x: i32;
    func __assign__(self: *V, o: *const V): void { self.x = o.x; }
}
var a: V = .V{ .x = 1 };
var b: V = .V{ .x = 2 };   // 初始化：不派发
a = b;                      // 赋值：a.__assign__(b.&)
```

## 4.3 块

```clux
{
    var x = 1;
    x += 1;
}
```

块引入独立作用域；块内声明的变量在退出块时结束生命周期。

## 4.4 if

```clux
if (cond) {
    ...
} else if (other) {
    ...
} else {
    ...
}
```

条件必须用圆括号包围。

## 4.5 switch

```clux
switch (value) {
    (1, 2) -> {
        ...
    }
    (3) -> {
        ...
    }
    default -> {
        ...
    }
}
```

- 条件必须用圆括号。
- 分支写作 `(pattern, ...) -> { ... }`。
- `default` 最多一个。
- 分支不 fallthrough。

## 4.6 while

```clux
while (cond) {
    ...
}
```

## 4.7 do-while

```clux
do {
    ...
} while (cond);
```

循环体至少执行一次。

## 4.8 for

```clux
for (var i = 0; i < 10; i += 1) {
    ...
}
```

三段式 `for` 的初始化、条件、更新都以 `;` 分隔；初始化可以省略。

## 4.9 foreach

```clux
foreach (var item of items) {
    ...
}
```

- 迭代对象必须是数组、切片、元组或可索引序列。
- `var` 可省略；省略时只读。
- 迭代变量可以带显式类型标注。

```clux
foreach (var item: Item of items) { ... }
```

## 4.10 break / continue

```clux
break;
continue;
```

只作用于当前最近一层循环。

## 4.11 return

```clux
return;
return value;
```

返回表达式必须能转换为函数的声明返回类型。

## 4.12 defer

```clux
defer {
    cleanup();
}
```

`defer` 块在离开当前作用域时执行，按后进先出顺序。

带捕获的 defer：

```clux
defer |(h = handle)| {
    close(h);
}
```

## 4.12.1 using

```clux
using a = expr;
using b: T = expr;
```

`using` 声明一个变量，并在**离开当前作用域**时调用它的 `__dispose__` 方法
（宽松约定，**不是析构**：编译器只在作用域退出点插入这一句调用，不参与
所有权/生命周期推导）：

```text
using a = e;   ⇒   var a = e;
                   defer |pa = a.&| { pa.__dispose__(); }
```

- 退出顺序与 `defer` 共用同一条 LIFO 序列（后声明先执行）；
- 块尾正常退出、`return`、`break`、`continue` 都会触发；
- `__dispose__` 必须是该类型自身的成员方法：`func __dispose__(self: *T): void`；
  类型没有这个方法时 `using` 报错。

```clux
struct File {
    fd: i32;
    func __dispose__(self: *File): void { close_file(self.fd); }
}

func work(): i32 {
    using f = open_file("a.txt");   // 作用域退出时 f.__dispose__()
    return f.fd;
}
```

## 4.13 表达式语句

```clux
foo();
x + 1;
```

表达式作为语句时以 `;` 结束。没有副作用的值表达式仍必须满足类型检查。

## 4.14 空语句

```clux
;
```

空语句不产生操作，只占一个语句位置。

## 4.15 comptime 语句

```clux
comptime if (LIMIT > 0) {
    ...
}

var seq: [3]i32 = .[3]i32{1, 2, 3};
comptime foreach (var x of seq) {
    ...
}
```

- `comptime if` 在编译期选择分支；未选分支不参与运行期代码。
- `comptime foreach` 在编译期展开；序列长度和元素必须是编译期已知值。
  序列必须是**已有的编译期值**（局部/全局变量、参数等）：clux 没有数组字面量，
  所以不能直接写 `comptime foreach (var x of [1, 2, 3])`，先绑定到变量再迭代。

## 4.16 编译期断言

```clux
static_assert(sizeof(i64) == 8 as u64, "expected 64-bit i64");
```

- `static_assert` 是标准语句，可出现在任何语句位置。
- 条件在编译期求值（CTFE），必须是 `bool`：
  - `true` → 无操作，不生成运行期代码；
  - `false` → 编译失败，诊断带上 `msg` 的字符串字面量文本。
- 条件不是编译期常量、或不是 `bool` → 编译失败。
- 位于 `comptime if` 未选中分支里的 `static_assert` 不参与语义检查（随剪枝
  消失），可用于泛型/平台条件断言，例如禁止 `T == i32`：

```clux
func f[T](x: T): void {
    comptime if (T == i32) {
        static_assert(false, "f: T must not be i32");
    }
}
```
