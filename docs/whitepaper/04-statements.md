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
- 迭代变量可以带类型和所有权标注。

```clux
foreach (var item: ref *Item of items) { ... }
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

comptime foreach (var x of [1, 2, 3]) {
    ...
}
```

- `comptime if` 在编译期选择分支；未选分支不参与运行期代码。
- `comptime foreach` 在编译期展开；序列长度和元素必须是编译期已知值。
