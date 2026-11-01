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
var f = func '<owner> |r| get(): '<owner> *i32 { return r; };
```

- 泛型函数：

```clux
func id[T](x: T): T { return x; }
```

- 方法：struct 内的 `func`，第一个参数是接收者：

```clux
struct Counter {
    value: i32;

    func add(self: *Counter, n: i32): void {
        self.value += n;
    }
}
```

实例方法用 `c.add(1)` 调用，静态方法用 `Counter::add(c, 1)` 调用。实参校验与
普通函数调用相同：数量必须匹配，每个实参必须能隐式转换到形参类型
（`func` 形参接受 `cfunc` 实参时自动包装）。

接收者同样按 `self` 形参参与隐式转换：值接收者先取地址（保留 `const` /
`volatile`）再比较。因此 `*T` → `*const T`、`T` → `*const T`、`*T` → `*T`
合法；`*const T` / `const T` 调用 `self: *T` 的方法会被拒绝（不得丢限定），
指针接收者调用值 `self` 形参也会被拒绝（需显式解引用）。

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

### 3.4.1 编译期求值范围

`comptime var` 的初始化器、`comptime func` 的调用点，以及常量槽位（数组边界、
`type` 别名右手、枚举值、`static_assert` 条件、`make` 长度等）由编译期求值器
执行。可用的数据与操作：

- 标量、`str`、数组、元组、`struct`、tag `union`、`cunion`、`?T`、切片；
- 字段读写（含嵌套字段与指针自动解引用 `p.field`）、tag `union` 的 `is`
  判定与成员读取、`?T` 的 `.!` 解包与 `nil` 比较；
- `x.&` / `p.*`、数组 / 切片 / `str` 的切片表达式 `a[lo:hi]`、
  `make(ptr, len)`（切片视图）、`data(x)`（首元素指针）与 `take_opt(x)`
  （移出 optional 载荷并把源置 `nil`）；
- 实例方法调用（含泛型方法）与静态方法调用：值接收者得到副本，`*T` 接收者直接
  修改接收者的存储；
- 错误处理语法糖 `x.?` / `x.!`（result struct 协议 `ok()` / `value()` /
  `error()` / `panic()`）：`x.?` 失败时执行 `return of_error(...)`，`x.!` 失败
  是编译期 panic（诊断，不是运行期 abort）；
- 复合值可继续参与运算（`==` / `!=` 按字段比较），结果可作编译期常量使用。

不在编译期求值范围内：通用堆分配、任意 FFI 调用、依赖运行期状态的表达式；
`cfunc_ptr(f)`（C 函数指针没有编译期表示）与 `panic(msg)`（编译期直接报错
而不是运行期终止）会被明确诊断。
`data(x)` 返回的指针指向**编译期存储**，只能在同一段编译期求值内使用；
指针不得逃逸出该作用域（与运行期的悬垂指针同义，编译器不做检查）。

## 3.5 结构体

```clux
struct User {
    name: str;
    age: i32;

    var count: i32 = 0;

    func rename(self: *User, name: str): void {
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

### 3.5.1 方法调用与魔法方法

**方法就是类型命名空间里的普通函数**。实例调用是语法糖：

```text
a.b(args)  ⇒  typeof(a)::b(a.&, args)     // a 是结构体值
a.b(args)  ⇒  typeof(a)::b(a,    args)     // a 已经是指针
```

因此 `self` 只是首参的约定名字（不是关键字），没有"实例方法/静态方法"的
声明区分；`a.b()` 传的是 `a.&`，形参写成值类型时会在调用点因 `*T → T`
不兼容而被普通实参规则拒绝。`T::b(a, ...)` 静态调用不插入接收者。

结构体成员方法名可以写成 `__name__`（**魔法方法**），仅限下表内的名字；
未知的 `__*` 方法名报错。魔法方法把该类型上的原生语法位置**改写为上面这套
普通方法调用**：

| 语法 | 方法 | 签名要求 |
|---|---|---|
| `a + b` `- * / %`，`& \| ^ << >>` | `__add__ __sub__ __mul__ __div__ __mod__`，`__and__ __or__ __xor__ __shl__ __shr__` | `(self: *const T, rhs: *const R) -> V` |
| `-a` / `!a` / `~a` | `__neg__` / `__not__` / `__bnot__` | `(self: *const T) -> V`（`__not__` 必须 `bool`） |
| `a == b` / `a != b` | `__eq__` | `(self: *const T, rhs: *const R) -> bool`；`!=` 自动取反 |
| `a < b` `<=` `>` `>=` | `__lt__` `__le__` `__gt__` `__ge__` | `(self: *const T, rhs: *const R) -> bool`，各自独立不互相推导 |
| `a[i]` | `__index__` | `(self: *const T, i: I) -> R`；`R` 必须可解引用（`*U` / `*const U` / 带 `__deref__`） |
| `len(a)` | `__len__` | `(self: *const T) -> usize` |
| `a.x` / `a.m()` / `a.*` 的回退 | `__deref__` | `(self: *const T) -> R`；`R` 必须可解引用 |
| `using a = e;` 的作用域退出 | `__dispose__` | `(self: *T) -> void`（宽松约定，不是析构；见 §4.12.1） |
| `a = b;`（赋值） | `__assign__` | `(self: *T, rhs: *const R) -> void`；初始化（`var`/参数/构造/返回值）不派发 |
| `.T{args}`（构造） | `__init__` | `(self: *T, ...任意参数...) -> void`；有它时禁用具名构造，`.T{args}` = 零初始化 + init；单参数时隐式构造用于 var 初始化/实参/返回值/字段初始化 |

规则：

- 二元魔法方法的**右操作数按指针传**（`rhs: *const R`，零拷贝）：`a op b`
  ⇒ `typeof(a)::__op__(a.&, b.&)`；`b` 不是左值时编译器先物化临时副本。
- 派发只发生在**前导操作数**上，且仅当它是结构体（指针 / `?T` / 数组不
  自动解引用）；内建类型有既有语义时永远优先。
- 魔法方法**可以是泛型**：派发点生成的是普通方法调用，按实参推断并实例化
  （`a + 5` ⇒ `a.__add__(5.&)` 推断出 `U = i32`）。clux 没有重载，
  泛型 + `comptime if` 就是重载的表达方式；聚合内部的判等同样有实参调用点，
  泛型 `__eq__` 在那里也会正常实例化。
- `a[i]` 等价于 `(typeof(a)::__index__(a.&, i)).*`：`__index__` 返回指向
  元素的指针，读/写/复合赋值都作用于该指针目标；返回 `*const U` 即只读。
  越界检查由 `__index__` 自己决定（可直接 `panic`）。
- `__deref__` 是 `.` 成员访问（读/写/调用）与 `.*` 的回退：直接成员永远
  优先；链上每个类型都必须能给出指针或另一个 `__deref__`；环或超深报错
  （深度默认 8：`--magic-deref-depth=N`（0..31）优先，环境变量
  `CLUX_MAGIC_DEREF_DEPTH` 次之，`0` 表示禁用整条展开）。
- `__eq__` 是该**类型的相等性定义**：聚合内部的判等（`?T` 载荷、`[N]T` 元素、
  其他结构体的同类型字段、union 载荷）同样调用它，而不是整块 `memcmp`。
  实现上 `a == b` 展开成内部函数 `__clux_eq_<T>(a.&, b.&)`，函数体逐字段 /
  逐元素 / 按 tag 展开，叶子比较仍走同一条糖。没有 `__eq__` 的类型保持
  逐字段比较（结构体不整块 `memcmp`：C 的 padding 内容不确定）。

```clux
struct Vec2 {
    x: i32;
    y: i32;

    func __add__(self: *const Vec2, o: *const Vec2): Vec2 {
        return .Vec2{ .x = self.x + o.x, .y = self.y + o.y };
    }
    func __eq__(self: *const Vec2, o: *const Vec2): bool {
        return self.x == o.x && self.y == o.y;
    }
}
```

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
    func write(self: *W, data: []u8): void;
}

struct File implements IWriter[File] {
    func write(self: *File, data: []u8): void { ... }
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

只有 `cfunc` 能作为外部 C 函数声明：clux `func` 使用 env-first 的 clux
调用约定，不能作为 C ABI，因此 `extern func ...;` 会被明确拒绝（它不再
是 `extern cfunc` 的等价写法）。

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
