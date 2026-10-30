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
| 23 | 前缀 `!` `~` `-` `?` `*` `&` `own` `ref` `fatal` `share` `weak` `const` `volatile` | 右绑定 |
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
- 方法调用由接收者决定：值接收者或 `ref *T` 接收者。
- 静态方法使用 `Type::name(...)`。

## 5.9 成员访问与下标

```clux
obj.field
ptr.field          // 指针自动解引用
arr[i]
tuple[i]
```

字段访问返回字段值；指针上的字段访问自动解引用。下标用于数组、切片和元组。

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

## 5.11 堆分配与动态数组

```clux
// 堆分配通过 FFI 完成：
extern lib libc from "*";
extern cfunc libc::malloc(size: usize): opaque;
var raw: opaque = libc::malloc(16);
var p: *User = raw as *User;

// make 把“已有指针 + 长度”包成切片视图，不分配：
var xs: []i32 = make(buf, len);
```

- 指针生命周期照搬 C：分配与释放都由用户通过 FFI/库函数负责。
- `make(pointer, len)` 只构造切片视图，缓冲区由调用方管理与释放。

## 5.12 指针与解引用

```clux
p.&        // 取地址，得到 ref *
p.*        // 解引用读
p.* = v    // 解引用写
```

`p.field` 和 `p[i]` 会自动解引用指针。

## 5.13 optional 操作

```clux
opt.!
opt.?
```

- `opt.!` 断言有值；无值时运行期错误。
- `opt.?` 安全解包；无值时由上下文处理。

## 5.14 所有权原语

```clux
move(x)
clone(x)
```

- `move(x)` 把 `x` 的所有权转出；原位置进入不可用状态。
- `clone(x)` 深拷贝拥有型值。

## 5.15 编译期类型运算

```clux
sizeof(T)
alignof(T)
typeof(value)
```

参数必须能确定类型；结果是编译期常量或类型值。

## 5.16 内建序列操作

```clux
len(x)
data(x)
cfunc_ptr(f)
```

- `len(x)` 返回数组、切片、元组或字符串的长度。
- `data(x)` 返回序列首元素的 C 地址，用于 FFI。
- `cfunc_ptr(f)` 把 clux 函数转换为 C 函数指针。

## 5.17 调用实参展开

```clux
f(...args)
.T{ ...fields }
```

`...` 在调用实参位置展开数组/元组；在构造位置展开字段或元素。
