# 1. 词法与源文件

## 1.1 编码

- 源文件必须是 UTF-8。
- 文件开头可以有一个 UTF-8 BOM；BOM 不属于任何 token，编译器必须忽略。
- 换行可以是 LF 或 CRLF；语义相同。
- 标识符和字符串按 UTF-8 解码；列号按字形簇计算。

## 1.2 空白与注释

空白：空格、制表符、回车、换行。

行注释：

```clux
// 到行尾
```

块注释：

```clux
/* 可以跨行 */
```

注释不产生 token。注释不得嵌套块注释；`/*` 中的 `/*` 是普通文本，第一个
`*/` 结束注释。

## 1.3 标识符

标识符首字符必须是 Unicode `ID_Start` 或 `_`；后续字符必须是 `ID_Continue`
或 `_`。

以下划线开头并后接另一个下划线的名字（`__name`）属于编译器保留名字空间，
用户源码不得声明。

## 1.4 关键字

```text
alignof  as       bitfield  bool      break     cfunc     clone
comptime const    continue  cunion    default   defer     do
else     enum     export    extends   extern    f32       f64
false    fatal    for       foreach   from      func      i8
i16      i32      i64       if        implements import   interface
is       len      lib       make      move      new       nil
of       opaque   own       ref       return    share     sizeof
str      struct   switch    true      type      typeof    u8
u16      u32      u64       union     undefined usize     var
void     volatile weak      while
```

`test` 不是全局关键字。只有顶层出现 `test "名称" { ... }` 时才按测试块解析；
因此普通函数可以命名为 `test`。

## 1.5 标点与运算符

```text
( ) { } [ ] ; , . : :: -> => ... .? .! .& .*
+ - * / % & | ^ ~ ! = == != < <= > >= << >>
&& || ? += -= *= /= %=
```

`<<` / `>>` 在词法层不合并：解析器在相邻字符位置按上下文合成为移位运算符；
在其他位置分别解释为尖括号。`...` 是单独的三字符符号。

## 1.6 整数字面量

```clux
0
123
0xFF
0o755
0b1010
```

- 十进制、十六进制、八进制、二进制。
- 进制前缀后必须有至少一个数字。
- 字面量可以带类型后缀，后缀必须紧贴数字：

```clux
1i8   2i16   3i32   4i64
5u8   6u16   7u32   8u64   9usize
```

无后缀的整数字面量类型由上下文推断；默认整数类型是 `i32`。

## 1.7 浮点字面量

```clux
0.0
2.5
1e9
1.5e-3
2.5f32
3.5f64
```

十进制整数部分后跟 `.` 和至少一位数字，或带 `e` / `E` 指数。
`f32` / `f64` 是类型后缀；无后缀浮点字面量默认 `f64`。

## 1.8 布尔与字符串字面量

```clux
true
false
"hello"
"line\n"
"\x41"
```

字符串由双引号包围，不能跨行。转义：

```text
\n  \t  \r  \\  \'  \"  \0  \xHH
```

`\xHH` 允许一到两位十六进制数字。

## 1.9 字符字面量

```clux
'a'
'\n'
'\x41'
```

字符字面量恰好包含一个范围在 `u8` 内的字符，类型是 `u8`。
字符字面量不用于 Unicode 标量值；需要字符串时使用 `str`。

## 1.10 空值字面量

```clux
nil
undefined
```

- `nil` 是 optional 的“无值”字面量。
- `undefined` 表示未初始化；它只能出现在允许未初始化语义的位置。

## 1.11 源文件结构

一个 `.cx` 文件由顶层声明组成，声明之间不需要分隔符，语句和声明各自以
`;` 或块结束。
