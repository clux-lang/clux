# 5. 表达式

## 5.1 字面量

```clux
42
3.14
true
"text"
'a'
nil
undefined
```

## 5.2 标识符与命名空间

```clux
value
Type::member
module::name
Enum::Variant
```

`::` 用于类型/模块命名空间访问。

## 5.3 运算符优先级

从低到高：

| 优先级 | 运算符 | 结合性 |
|---|---|---|
| 0 | `=` `+=` `-=` `*=` `/=` `%=`，三元 `?:` | 右结合 |
| 1 | `\|\|` | 左结合 |
| 3 | `&&` | 左结合 |
| 5 | `\|` | 左结合 |
| 7 | `^` | 左结合 |
| 9 | `&` | 左结合 |
| 11 | `==` `!=` `extends` | 左结合 |
| 13 | `<` `<=` `>` `>=` | 左结合 |
| 15 | `<<` `>>` | 左结合 |
| 17 | `+` `-` | 左结合 |
| 19 | `*` `/` `%` | 左结合 |
| 21 | `as` `is` | 左结合 |
| 23 | 前缀 `!` `~` `-` `?` `*` `&` `const` `volatile` | 右绑定 |
| 25 | 后缀调用、成员、下标、`?` `!` `.*` `.&` | 左结合 |

## 5.4 一元表达式

```clux
!flag
~bits
-value
```

`-` 与 `+` 在操作数位置是一元；在两个操作数之间是二元。

## 5.5 二元表达式

```clux
a + b
a - b
a * b
a / b
a % b
a & b
a | b
a ^ b
a << b
a >> b
a && b
a || b
a == b
a != b
a < b
a <= b
a > b
a >= b
```

`==` / `!=` 对复合值的判等规则：

- `str` 按内容比较，不比较底层指针；
- `struct` 按**字段递归**比较（`str` 字段按内容、数组逐元素、`?T` 字段先比
  tag 再比载荷）；结构兼容的不同具名 `struct` 可以比较，按左操作数类型解释；
- tag `union` 先比 tag，相同再按该成员的载荷递归比较；
- `cunion` 与元组按字节比较；
- 切片按 `(data, len)` 比较；函数值按 `(fn, env)` 比较。

其余运算符只接受标量操作数（跨类型不做隐式转换，见 §2.10）。

### 5.5.1 用户类型的运算符（魔法方法）

当**前导操作数**是结构体且该类型定义了对应魔法方法时，运算符改写为普通
方法调用（见 §3.5.1 的魔法表）：

```text
a + b    ⇒  #typeof(a)::__add__(a.&, b.&)
a == b   ⇒  #typeof(a)::__eq__(a.&, b.&)
a != b   ⇒  !(#typeof(a)::__eq__(a.&, b.&))
-a       ⇒  #typeof(a)::__neg__(a.&)
a[i]     ⇒  (#typeof(a)::__index__(a.&, i)).*
#len(a)  ⇒  #typeof(a)::__len__(a.&)
```

- 内建类型（标量 / 数组 / 切片 / `str` / 元组 / enum / 指针）永远走内建
  语义，不查魔法表；`*T`、`?T`、`[N]T` 不会自动解引用到 `T` 去派发。
- 只派发前导操作数：`b` 自己的魔法方法不影响 `a op b` 是否派发。
- `<` `<=` `>` `>=` 各自独立（`__lt__` 等），不互相推导。
- 定义了 `__eq__` 的类型，其相等性在所有聚合判等位置生效（§3.5.1）。

## 5.6 三元表达式

```clux
cond ? then_value : else_value
```

三元是右结合，优先级与赋值同级。

## 5.7 类型转换与类型判断

```clux
value as i64
value is Member
value extends Type
```

- `as` 执行显式转换。
- `is` 判断 tag union 当前 member，结果为 `bool`。
- `extends` 在编译期判断类型关系。

## 5.8 调用

```clux
f(a, b)
f[T](a, b)
obj.method(a)
Type::static_method(a)
```

- 泛型实参用方括号。
- 方法调用的接收者是第一个实参（通常是 `self: *T`）。
- 静态方法使用 `Type::name(...)`。

## 5.9 成员访问与下标

```clux
obj.field
ptr.field          // 指针自动解引用
arr[i]
tuple[i]
arr[lo:hi]         // 切片：任意一端可省略（省略下界=0，省略上界=len）
```

字段访问返回字段值；指针上的字段访问自动解引用。下标用于数组、切片和元组；
切片用于数组、切片与 `str`（`s[:]` 是 `[N]T → []T` 的唯一显式转换路径）。
结构体上定义 `__index__` / `__slice__` 时，`a[i]` / `a[lo:hi]` 改为派发到
这两个魔法方法（见 §3.5.1）；`__slice__` 的两个边界实参总是有值。

## 5.10 构造

具名构造：

```clux
var user = .User{ .name = "a", .age = 1 };
```

匿名构造：

```clux
var user: User = .{ .name = "a", .age = 1 };
```

数组/元组按位置：

```clux
var a = .[4]i32{ 1, 2, 3, 4 };
var t = .<i32, str>{ 1, "a" };
```

重复填充：

```clux
var zeros = .[100]i32{ <0, 100> };
```

构造字段展开：

```clux
var u = .User{ ...base, .age = 2 };
```

## 5.11 堆分配与切片视图

```clux
// 堆分配通过 FFI 完成：
extern lib libc from "*";
extern cfunc libc::malloc(size: usize): opaque;
var raw: opaque = libc::malloc(16);
var p: *User = raw as *User;

// #slice 把“已有指针 + 长度”包成切片视图，不分配：
var xs: []i32 = #slice(buf, n);
```

- 指针生命周期照搬 C：分配与释放都由用户通过 FFI/库函数负责。
- `#slice(pointer, len)` 只构造切片视图，缓冲区由调用方管理与释放。

## 5.12 指针与解引用

`x.&` 取地址要求 `x` 是左值（变量 / 字段 / 下标 / 解引用）。对右值
（字面量、调用结果、构造表达式）用户代码写 (5).& 是错误：
`error: cannot take the address of a temporary value`。

**右值物化只发生在魔法方法派发处**：`a + 5` ⇒ `a.__add__(5.&)`，这里的
`5.&` 由编译器生成，物化成 C99 复合字面量 `&(T){ v }`（不需要额外临时
变量，对象存活到所在块结束）。

```clux
p.&        // 取地址，得到 *T
p.*        // 解引用读
p.* = v    // 解引用写
```

`p.field` 和 `p[i]` 会自动解引用指针。

## 5.13 optional 操作

```clux
opt.!
```

- `opt.!` 断言有值；无值时运行期错误（先判空再解包：
  `if (x != nil) { var v = x.!; }`）。
- optional **只支持 `.!`**；`.?`（try）不在 optional 上提供——它只用于
  result-like 结构体（`ok()` / `value()` / `error()` 方法）。需要分支时
  自己写 `if (x == nil) { ... }`。

## 5.14 编译期类型运算

```clux
#sizeof(T)
#alignof(T)
#typeof(value)
```

参数必须能确定类型；结果是编译期常量或类型值。

## 5.15 内建序列操作

```clux
#len(x)
#slice(pointer, len)
#data(x)
#cfunc_ptr(f)
#take_opt(x)
#panic(msg)
```

- `#len(x)` 返回数组、切片、元组或字符串的长度；结构体可派发 `__len__`。
- `#slice(pointer, len)` 把已有指针与长度包装成切片视图，不分配。
- `#data(x)` 返回序列首元素的 C 地址，用于 FFI。
- `#cfunc_ptr(f)` 把 clux 函数转换为 C 函数指针。
- `#take_opt(x)` 移出 optional 载荷并把源置 `nil`。
- `#panic(msg)` 是编译器内建的直接报错入口。

## 5.16 调用实参展开

```clux
f(...args)
.T{ ...fields }
```

`...` 在调用实参位置展开数组/元组；在构造位置展开字段或元素。
