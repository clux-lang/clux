# 3. 声明

## 3.1 顶层结构

一个 `.cx` 文件可以包含任意顺序的顶层声明：

```clux
import foo from "foo.cx";
export func add(a: i32, b: i32): i32 { return a + b; }
struct User { name: str; }
test "Suite.Case" { assert(1 + 1 == 2); }
```

顶层声明之间不需要分隔符；声明末尾的 `;` 由各自语法决定。

## 3.2 装饰器

声明前可以有一个或多个装饰器：

```clux
[[trace]]
[[metadata("role", "handler")]]
func handle(): void { ... }
```

语法：

```text
[[ <expr> ]]
```

装饰器适用于顶层 `var`、`func`、类型定义、struct 字段和 struct 方法。
普通装饰器是值级包装；`metadata` 与类型装饰器是编译期标记。详见
[装饰器与元信息](09-metadata.md)。

## 3.3 函数

```clux
func name(a: i32, b: str): bool {
    return true;
}
```

语法：

```text
func <name> [<generic-params>] ( <params> ) : <return-type> { <block> }
```

- 参数写作 `name: type`。
- 返回类型必须显式写出；无返回值写 `void`。
- 函数体是块。
- 函数值/匿名函数：

```clux
var f = func(x: i32): i32 { return x + 1; };
```

- 带捕获的闭包：

```clux
var f = func |(offset: i32 = 10)| add(x: i32): i32 {
    return x + offset;
};
```

- 闭包作用域标注写在 `func` 后、捕获列表前：

```clux
var f = func '<owner> |r| get(): '<owner> ref *i32 { return r; };
```

- 泛型函数：

```clux
func id[T](x: T): T { return x; }
```

- 方法：struct 内的 `func`，第一个参数是接收者：

```clux
struct Counter {
    value: i32;

    func add(self: ref *Counter, n: i32): void {
        self.value += n;
    }
}
```

## 3.4 变量

局部变量：

```clux
var x: i32 = 1;
var y = 2;              // 类型推断
var z: ?i32 = nil;
var u: i32 = undefined; // 未初始化
```

全局变量：

```clux
var counter: i32 = 0;
```

语法：

```text
var <name> [':' <type>] '=' <expr> ';'
```

变量必须有初始化器；未初始化状态使用 `undefined` 显式表达。

编译期变量：

```clux
comptime var LIMIT: const u32 = 32;
```

## 3.5 结构体

```clux
struct User {
    name: str;
    age: i32;

    var count: i32 = 0;

    func rename(self: ref *User, name: str): void {
        self.name = name;
    }
}
```

语法元素：

- 字段：`name: type;`
- 静态/成员变量：`var ...;`
- 方法：`func ... { ... }`
- 嵌入：`...Base;`
- 实现接口：`struct S implements I[S] & J[S] { ... }`
- 泛型：`struct Box[T] { value: T; }`

字段可以带装饰器：

```clux
struct User {
    [[metadata("unit", "px")]] x: i32;
}
```

字段/方法元信息只属于 struct。

## 3.6 枚举

```clux
enum Color: u8 {
    Red = 1,
    Green = 2,
    Blue = 4,
}
```

语法：

```text
enum Name : Underlying { Variant = value, ... }
```

`Underlying` 必须是整数类型；variant 值是编译期整数常量。

## 3.7 tag union

```clux
union Shape {
    circle: Circle;
    rect: Rect;
}
```

`union` 是带 tag 的联合体：同一时刻只有一个 member 有效。

## 3.8 C union

```clux
cunion Value {
    i: i32;
    f: f32;
}
```

`cunion` 没有 tag；所有 member 共享偏移 0，用于 C ABI 布局。

## 3.9 位域

```clux
bitfield Flags: u32 {
    a: 3;
    b: 5;
}
```

详见[位域](10-bitfields.md)。

## 3.10 interface

```clux
interface IWriter[W] {
    func write(self: ref *W, data: ref []u8): void;
}

struct File implements IWriter[File] {
    func write(self: ref *File, data: ref []u8): void { ... }
}
```

接口是编译期约束，不产生运行期对象。

## 3.11 类型别名

```clux
type UserId = u64;
type Buffer[T] = []T;
type Callback = func(i32) -> void;
```

别名透明；泛型别名按实参实例化。

## 3.12 import / export / reexport

```clux
import foo from "foo.cx";
import bar from "bar.cx";
export func api(): void { ... }
export * from "other.cx";
```

- `import <alias> from "..."` 引入模块命名空间。
- `export` 把声明加入当前模块导出表。
- `export * from` 转发另一个模块的导出。

## 3.13 extern / FFI

动态库声明：

```clux
extern lib crt from "msvcrt";
```

外部 C 函数：

```clux
extern cfunc crt::puts(s: *const u8): i32;
```

等价写法：

```clux
extern func crt::puts(s: *const u8): i32;
```

外部数据符号：

```clux
extern var crt::errno: i32;
```

C ABI 函数定义：

```clux
cfunc callback(x: i32): i32 { return x + 1; }
```

`cfunc` 没有捕获环境，不能泛型，不能带捕获列表。

## 3.14 test 块

```clux
test "Suite.Name" {
    assert(1 + 1 == 2);
}
```

顶层 `test "..."` 是测试声明；见[测试](11-testing.md)。
