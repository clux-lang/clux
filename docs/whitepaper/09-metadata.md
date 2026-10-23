# 9. 装饰器与元信息

## 9.1 装饰器语法

```text
[[ <expr> ]]
```

装饰器可以出现在：

- 顶层 `var` / `func`；
- struct 方法；
- struct 字段；
- 类型定义（struct / enum / union / cunion / bitfield / type alias）。

普通装饰器是值级包装；`metadata` 和类型装饰器是编译期标记。

## 9.2 metadata 装饰器

```clux
[[metadata("name", "user")]]
struct User {
    [[metadata("unit", "px")]]
    x: i32;

    [[metadata("role", "handler")]]
    func run(self: ref *User): void { ... }
}
```

语法：

```text
[[metadata("<key>", <compile-time-value>)]]
```

规则：

- key 必须是字符串字面量。
- value 必须是编译期已知值。
- 同一个类型/字段/方法上 key 不得重复。
- 字段/方法元信息只属于 struct。
- 类型别名上的 metadata 作用于别名指向的类型；透明别名没有独立身份。

## 9.3 可存储的值

- 整数；
- `bool`；
- `str`；
- 无捕获函数值。

值在编译期求值，运行期只读。

## 9.4 运行期读取

```clux
var name: str = get_metadata[User]("name");
var unit: str = get_field_metadata[User]("x", "unit");
var role: str = get_method_metadata[User]("run", "role");
```

返回值类型是元信息条目自身的静态类型：

```clux
[[metadata("level", 3)]]
struct S { }

var level: i32 = get_metadata[S]("level");
```

规则：

- 类型实参必须是具体类型。
- key / 字段名 / 方法名必须是字符串字面量。
- 条目不存在是编译错误。
- 读取在编译期解析为常量，运行期没有查找表。

## 9.5 泛型实例

```clux
[[metadata("size", sizeof(T))]]
struct Box[T] { value: T; }

var a: usize = get_metadata[Box[i32]]("size");
var b: usize = get_metadata[Box[i64]]("size");
```

泛型类型元信息属于每个具体实例，`T` 绑定为该实例的类型实参。

## 9.6 类型装饰器

类型装饰器是一个泛型 comptime 函数：

```clux
comptime func controller[T](url: str): void {
    metadata[T]("controller.url", url);
}

[[controller("/users")]]
struct UserController { }
```

调用语义：

```text
controller[UserController]("/users")
```

规则：

- 装饰器函数必须是 `comptime func`。
- 恰好有一个目标类型参数，`T` 绑定为被装饰的具体类型。
- 返回类型是 `void`。
- 可以带普通参数。
- 在类型完成之后执行。
- 泛型类型按每个具体实例执行。
- 类型装饰器不产生运行期代码。

## 9.7 类型装饰器写入元信息

```clux
comptime func controller[T](url: str): void {
    metadata[T]("controller.url", url);
}
```

`metadata[T]` 是编译期函数调用：

```text
metadata[<type>]("<key>", <compile-time-value>)
```

它只允许在 comptime 上下文使用；运行期不能写元信息。

## 9.8 编译期读取

```clux
comptime func check[T](): void {
    var url: str = get_metadata[T]("controller.url");
    if (url == "") {
        metadata[T]("controller.empty", true);
    }
}
```

`get_*` 在 comptime 函数内可用，结果直接折叠为常量。

## 9.9 执行顺序

对同一类型：

1. 类型密封；
2. 字段/方法 metadata 登记；
3. 类型装饰器按声明顺序执行。

类型装饰器因此可以读取字段/方法 metadata。

## 9.10 缺失与重复

```clux
var x = get_metadata[S]("missing"); // 编译错误
```

```clux
[[metadata("k", 1)]]
[[metadata("k", 2)]]
struct S { }                         // 编译错误：重复 key
```

## 9.11 值类型与函数

```clux
func handler(x: i32): i32 { return x + 1; }

[[metadata("handler", handler)]]
struct S { }

var f = get_metadata[S]("handler");
var y = f(41);
```

函数值元信息在读取后可以直接调用。
