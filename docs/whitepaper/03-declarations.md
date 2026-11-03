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

- 带捕获的闭包（`|...|` 是捕获列表）：

```clux
var f = func |(offset: i32 = 10)| add(x: i32): i32 {
    return x + offset;
};
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
指针不得逃逸出其所属存储块的生命周期（与运行期的悬垂指针同义）。逃逸后的
解引用是编译期错误（`ctfe: use-after-free`），不会读到已释放的存储：

```clux
comptime func bad(): i32 {
  var p: *i32 = undefined;
  { var x: i32 = 5; p = x.&; }   // x 的存储随块退出释放
  return p.*;                     // 编译期错误：use-after-free
}
```

编译期求值的存储**全部由编译器管理**（没有真实栈）：因此实现上对 CTFE
值采用**隔离释放**（本次编译内不把存储还给分配器，编译结束统一释放），
保证同一编译内**地址不复用**。地址因此就是对象身份，逃逸诊断是确定性的：
既不会因为地址被新对象复用而漏报，也不会因为旧归属残留而误报。

该检查覆盖编译期求值器里所有**能承载地址的写法**：`x.&`、`data(x)`、
`make(ptr, len)`、切片表达式 `a[lo:hi]`（含在逃逸切片上再切片）以及 `*T`
接收者。clux 不提供指针算术与指针下标（`p + 1`、`p[i]` 都是编译错误），指针
只能整体传递，因此不存在"算出来的地址绕过检查"这条路径；按元素访问必须先得到
切片，而切片基址总是在解引用前校验。

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

结构体成员方法名可以写成 `__name__`（**魔法方法**）：它把该类型上的原生语法
位置改写为上面这套普通方法调用。魔法方法的完整清单、签名要求、派发规则与限制
见 [§3.15 魔法方法](#315-魔法方法)。

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

接口是编译期约束，不产生运行期对象。约束的满足条件是**结构**的：类型只要提供接口要求的全部成员（名字与签名一致）即满足，**不要求**显式写 `implements I[...]`；`implements` 是可选的显式断言（写了同样会被逐项检查，缺失或签名不符即报错）。

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

## 3.15 魔法方法

结构体成员方法名可以写成 `__name__`（**魔法方法**），仅限下表内的名字；未知的
`__*` 方法名报错。魔法方法把该类型上的原生语法位置**改写为普通方法调用**
（改写形式见 §3.5.1），因此没有额外的调用协议：它就是一个普通方法。

### 3.15.1 派发规则

- 派发只发生在**前导操作数**上，且仅当它是结构体。指针、`?T`、数组、切片、
  `str` 等接收者**不**自动解引用去派发（要派发就先显式解引用）。
- 内建类型有既有语义时永远优先：`i32 + i32`、`[N]T` 下标、`len(str)` 等不会
  找魔法方法。
- 二元魔法方法的**右操作数按指针传**（`rhs: *const R`，零拷贝）：`a op b`
  ⇒ `typeof(a)::__op__(a.&, b.&)`；`b` 不是左值时先物化临时副本。
- 魔法方法**可以是泛型**：派发点生成的是普通方法调用，按实参推断并实例化
  （`a + 5` ⇒ `a.__add__(5.&)` 推断出 `U = i32`）。clux 没有重载，
  泛型 + `comptime if` 就是重载的表达方式；聚合内部的判等同样有实参调用点，
  泛型 `__eq__` 在那里也会正常实例化。
- 静态调用 `T::__op__(x, ...)` 当然也可以直接写，它就是普通方法调用。

### 3.15.2 方法表

| 语法 | 方法 | 签名要求 |
|---|---|---|
| `a + b` `- * / %`，`& \| ^ << >>` | `__add__ __sub__ __mul__ __div__ __mod__`，`__and__ __or__ __xor__ __shl__ __shr__` | `(self: *const T, rhs: *const R) -> V` |
| `-a` / `!a` / `~a` | `__neg__` / `__not__` / `__bnot__` | `(self: *const T) -> V`（`__not__` 必须 `bool`） |
| `a == b` / `a != b` | `__eq__` | `(self: *const T, rhs: *const R) -> bool`；`!=` 自动取反 |
| `a < b` `<=` `>` `>=` | `__lt__` `__le__` `__gt__` `__ge__` | `(self: *const T, rhs: *const R) -> bool`，各自独立不互相推导 |
| `a[i]` | `__index__` | `(self: *const T, i: I) -> R`；`R` 必须可解引用（`*U` / `*const U` / 带 `__deref__`） |
| `a[lo:hi]` | `__slice__` | `(self: *const T, lo: usize, hi: usize) -> S`；两个边界总是有实参 |
| `len(a)` | `__len__` | `(self: *const T) -> usize` |
| `a.x` / `a.m()` / `a.*` 的回退 | `__deref__` | `(self: *const T) -> R`；`R` 必须可解引用 |
| `using a = e;` 的作用域退出 | `__dispose__` | `(self: *T) -> void`（宽松约定，不是析构；见 §4.12.1） |
| `a = b;`（赋值） | `__assign__` | `(self: *T, rhs: *const R) -> void`；**初始化**（`var` / 实参 / 返回值 / 字段）不派发，见 `__init__` |
| `.T{args}`（构造） | `__init__` | `(self: *T, ...任意参数...) -> void`；有它时禁用具名构造，`.T{args}` = 零初始化 + init。**只有恰好一个非 `self` 形参时**，所有初始化位置（`var a: T = x`、实参、返回值、字段初始化）都改写成 `.T{x}`；源表达式是**已有值的位置**（变量 / 成员 / 下标 / 解引用 / 解包）时也走构造，因此从 `T` 值初始化会因形参不匹配而报错——这就是 move-only 类型禁止 copy 的手段 |
| `x(args)`（调用） | `__call__` | `(self: *T, ...任意形参...) -> R`；形参与返回类型由实现者决定。被调用者类型是带 `__call__` 的 struct 时派发；函数值 / 闭包的调用路径不受影响 |
| 同类型初始化（`var b: T = a;` / 实参 / 返回值 / 字段） | `__init_copy__` | `(self: *T, src: *T) -> void`；固定签名，形参是**指针**，所以"参数传递 = 构造"不会自递归。初始化位置把源作为**地址**传入（与 `__assign__` 的 rhs 对齐），构造里可以置空源对象 ⇒ move。没有它时同类型初始化按"不可 copy"拒绝 |

### 3.15.3 运算符

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

`__eq__` 是该**类型的相等性定义**：聚合内部的判等（`?T` 载荷、`[N]T` 元素、
其他结构体的同类型字段、union 载荷）同样调用它，而不是整块 `memcmp`。
实现上 `a == b` 展开成内部函数 `__clux_eq_<T>(a.&, b.&)`，函数体逐字段 /
逐元素 / 按 tag 展开，叶子比较仍走同一条糖。没有 `__eq__` 的类型保持逐字段
比较（结构体不整块 `memcmp`：C 的 padding 内容不确定）。

### 3.15.4 下标与切片

- `a[i]` 等价于 `(typeof(a)::__index__(a.&, i)).*`：`__index__` 返回指向元素的
  指针，读 / 写 / 复合赋值都作用于该指针目标；返回 `*const U` 即只读。越界检查
  由 `__index__` 自己决定（可直接 `panic`）。
- `a[lo:hi]` 等价于 `typeof(a)::__slice__(a.&, lo, hi)`，结果**不再**额外解引用
  （`__slice__` 直接返回值，通常是 `[]T`）。
- `__slice__` 的两个边界实参**一定有值**：省略下界时为 `0`，省略上界时为
  `len(a)`，所以省略上界要求该类型实现 `__len__`，否则报错。
- 两个边界形参的类型必须是 `usize`（与原生切片一致）。源码里的整数字面量边界
  会自动补上 `usize` 后缀，`x[1:3]` 无需写成 `x[1usize:3usize]`；非字面量边界
  若是别的整数类型需显式 `as usize`。

```clux
struct Window {
    data: [4]i32;

    func __len__(self: *Window): usize { return 4usize; }

    func __slice__(self: *Window, lo: usize, hi: usize): []i32 {
        return self.data[lo:hi];
    }
}

var w: Window = .Window{ .data = .[4]i32{ 1, 2, 3, 4 } };
var all: []i32 = w[:];     // ⇒ w.__slice__(0, len(w))
var mid: []i32 = w[1:3];   // ⇒ w.__slice__(1, 3)
```

### 3.15.5 访问回退与生命周期

- `__deref__` 是 `.` 成员访问（读 / 写 / 调用）与 `.*` 的回退：直接成员永远
  优先；链上每个类型都必须能给出指针或另一个 `__deref__`；环或超深报错
  （深度默认 8：`--magic-deref-depth=N`（0..31）优先，环境变量
  `CLUX_MAGIC_DEREF_DEPTH` 次之，`0` 表示禁用整条展开）。
- `__dispose__` 在 `using a = e;` 所在作用域退出时调用（正常退出、`return`、
  `break` / `continue` 都一样），顺序与 `defer` 一致：后登记的先执行。
- `dispose(x);` 是**手动**析构语句：`x` 的类型（或 `*x` 指向的类型）实现了
  `__dispose__` 就调用它，否则**静默无操作**（实参仍会求值）。这按**实例化
  后的类型**决定，所以泛型容器可以直接写 `dispose(self.item);` 而不需要
  为"有析构/没析构"的元素类型各写一份代码；用户自己定义的 `dispose` 函数
  优先于该内建。
- `__assign__` 只在**赋值语句**上派发：`target = value` ⇒
  `target.__assign__(value.&)`；初始化（`var`、参数、构造字段、返回值）不派发。
- 同类型初始化（`var b: T = a;`、实参、返回值、字段）走专门的
  `__init_copy__`，固定签名 `(self: *T, src: *T) -> void`，源按**地址**传入：
  `var b: T = a;` ⇒ 零初始化 + `b.__init_copy__(&a)`。形参是指针，所以"参数
  传递 = 构造"不会无限递归；构造里可以置空源对象 ⇒ 这是 move-only 类型
  （`Fatal` / `Own`）实现**移动初始化**的方式。没有 `__init_copy__` 时，同类型
  初始化按"不可 copy"拒绝。
- **`__init__` / `__init_copy__` 里的字段 TDZ**：`self` 指向"整体分配、
  字段只置零"的存储，所以字段在赋值前**不是有效的 T**。两条强制约束：
  (1) 任一 `return`（含函数体末尾的隐式返回）之前，`self` 的所有字段都必须
  已被赋值——整体赋值 `self.* = v` 或逐字段赋值都算；
  (2) 字段首次赋值之前不得读取它（读、方法调用、`+=` 读改写）。
  分支汇合取保守交集（只在某个分支里赋的值不算"肯定"），循环体不保证执行。
  因此 `__init__` 里靠调用 `self.setup()` 间接赋字段**不被接受**（编译器
  看不到赋值）；需要时用 `self.* = other.*` 整体赋值。
- `__init__` **不允许**把类型自身当形参（`func __init__(self: *T, o: T)`）：构造
  `T` 需要先有 `T`，而参数传递本身又是构造 → 编译期无限递归。编译器直接拒绝
  这种声明，并提示改用 `__init_copy__`。
- `__init__` 让 `.T{args}` 成为"零初始化 + `__init__(args)`"；此时禁用具名
  （`.T{ .field = ... }`）构造。`__init__` 只有一个非 `self` 参数时，该类型还
  支持从该参数**隐式构造**：`var t: T = 5;`、实参、返回值、字段初始化都会插入
  构造调用。
- 初始化**总是**走 `__init__`（而不是值拷贝）：源表达式是已有值的位置（变量 /
  成员 / 下标 / 解引用 / 解包）时同样插入构造，所以 `var b: T = a;`（`a: T`）会
  因为 `a` 不能转换成 `__init__` 的形参而报错。构造表达式（`.T{...}`）与函数
  调用返回的新值不受影响，直接放入目标。**move-only 类型**就建立在这条规则上：

  ```clux
  struct Fatal[T] {
      ptr: ?*T;
      func __init__(self: *Fatal[T], p: *T): void { self.ptr = p; }
      func __init_copy__(self: *Fatal[T], other: *Fatal[T]): void {
          self.ptr = other.take();  // 初始化 = move
      }
      func __assign__(self: *Fatal[T], other: *Fatal[T]): void {
          self.ptr = other.ptr;
          other.ptr = nil;          // 赋值 = move
      }
  }
  var a: Fatal[T] = b;   // 合法：__init_copy__ 把 b 置 nil（move）
  a = b;                 // 合法：__assign__ 把 b 置 nil（move）
  // 没有 __init_copy__ 时：var a: Fatal[T] = b; 报"不可 copy"
  ```

### 3.15.6 限制与诊断

- 方法名以 `__` 开头但不在上表：编译错误（保留名字空间）。
- 形参个数 / 形参类型 / 返回类型不满足表内要求：编译错误（例如 `__not__`
  不返回 `bool`、`__index__` 返回不可解引用的类型、`__slice__` 的边界不是
  `usize`）。
- 内建类型与内建语义优先；想用魔法方法就必须先包成结构体。
- 指针 / `?T` / 数组 / 切片接收者不自动解引用派发：`p + q`（都是 `*T`）不会
  找 `__add__`。
- 泛型 `__init__`（`func __init__[U](self: *T, src: U)`）**支持**：`.T{args}`
  按实参推断 `U` 并实例化方法（同时绑定 struct 实例的类型参数），构造节点直接
  调用实例。方法体里用 `comptime if (U extends X)` 分派不同来源形态。注意
  **同类型**初始化不走 `__init__`，而是走 `__init_copy__`（见上）。