# 2. 类型

clux 的类型是静态的。类型本身也是表达式：类型表达式可以出现在类型位置，
也可以作为值参与 `typeof`、`sizeof`、泛型和编译期计算。

## 2.1 基础类型

```text
void    bool    str    usize    opaque
i8 i16 i32 i64
u8 u16 u32 u64
f32 f64
```

- `void` 只作为无返回值。
- `bool` 是二值类型。
- `usize` 是独立的平台宽度无符号类型，不是 `u64` 或 `u32` 的别名。
- `str` 是 UTF-8 字符串值。
- `opaque` 表示无类型载荷，只用于 FFI 或显式类型擦除边界。

## 2.2 类型表达式

```clux
i32
User
Box[i32]
?i32
*User
ref *User
[]u8
[4]i32
<i32, str>
func(i32, str) -> bool
cfunc(i32) -> i32
const i32
volatile u32
```

类型表达式可以嵌套：

```clux
?ref *Box[i32]
[4]?u8
func(ref *User) -> ?str
```

## 2.3 类型修饰与所有权

所有权是类型的一部分，不是单独的变量属性。

```text
own T      拥有 T
ref T      借用 T
fatal T    必须被消费的 T
share T    引用计数共享
weak T     弱引用
```

指针和切片使用相同修饰：

```clux
own *File
ref *const User
fatal []u8
share *Node
weak *Node
[]u8
ref []const u8
```

所有权规则的完整语义见[所有权](07-ownership.md)。

## 2.4 指针、切片、数组

```clux
*T         裸指针
own *T     拥有指针
ref *T     借用指针
[]T        切片
[N]T       长度 N 的数组
```

- `N` 是编译期整数表达式。
- 数组是值类型，长度属于类型。
- 切片是“指针 + 长度”的视图，不拥有元素。
- 指针和切片指向的元素类型可以是 `const`。

## 2.5 可选类型

```clux
?T
```

`?T` 要么是 `nil`，要么包含一个 `T`。

```clux
var x: ?i32 = nil;
var y: ?i32 = 3;
```

解包语法：

```clux
x.!     // 断言有值，无值则运行期错误
x.?     // 可选安全解包，上下文必须能处理无值
```

## 2.6 元组

```clux
<i32, str>
<i32, i32, bool>
```

元组是值类型，元素按位置访问：

```clux
var t: <i32, str> = .<i32, str>{ 1, "a" };
var a = t[0];
```

## 2.7 函数类型

```clux
func(i32, str) -> bool
cfunc(i32) -> i32
```

- `func` 是 clux 函数值，可以带捕获环境。
- `cfunc` 是 C ABI 函数指针，没有捕获环境。
- 两者不隐式互通；需要 C 调用时使用 `cfunc(...)` 类型和 `cfunc_ptr(...)`。

## 2.8 类型别名

```clux
type UserId = u64;
type Handler = func(ref *Request) -> i32;
type Buffer[T] = []T;
```

别名是透明的；`UserId` 与 `u64` 是同一个类型。

## 2.9 泛型类型

```clux
Box[i32]
Map[str, User]
Result[i32, Error]
```

类型实参用方括号，不是圆括号。

## 2.10 类型转换

显式转换使用 `as`：

```clux
var n = 3 as i64;
var p = opaque_ptr as *u8;
```

clux 不进行隐式跨类型转换；整数和浮点字面量在上下文明确时可以按字面量类型
推断，但变量之间、聚合类型之间不会隐式转换。

## 2.11 类型运算

```clux
sizeof(T)
alignof(T)
typeof(expr)
```

- `sizeof` / `alignof` 产生 `usize` 编译期值。
- `typeof` 产生 `expr` 的类型值。

## 2.12 布局

- 基础类型的宽度由目标平台决定；`usize` 随平台。
- 数组是连续元素。
- struct 按字段声明顺序布局并遵守对齐。
- union / cunion / bitfield 的布局分别见对应章节。
- 跨 C ABI 的类型必须满足 C ABI 兼容规则。
