# 10. 位域

## 10.1 语法

```clux
bitfield Flags: u32 {
    a: 3;
    b: 5;
}
```

```text
bitfield <Name> : <u8|u16|u32|u64> {
    <field>: <width>;
    ...
}
```

## 10.2 规则

- 底层类型只能是无符号整数：`u8`、`u16`、`u32`、`u64`。
- 字段宽度是整数字面量，范围 `1..底层类型位宽`。
- 字段名唯一。
- 至少一个字段。
- 字段按声明顺序从 LSB 连续排布。
- 总位数不限制；可以超过底层类型宽度。
- 字段可以跨越底层字的边界。
- 存储为 `ceil(total_bits / word_bits)` 个底层字。

## 10.3 布局

```clux
bitfield Big: u8 {
    a: 4;
    b: 8;
    c: 5;
}
```

```text
bit 0..4    a
bit 4..12   b
bit 12..17  c
sizeof(Big) == 3
```

## 10.4 构造

```clux
var f: Flags = .Flags{ .a = 1u32, .b = 3u32 };
```

- 必须使用具名字段。
- 缺失字段补零。
- 字段值截断到字段宽度。
- 空构造 `.Flags{}` 全部为零。

## 10.5 字段访问

```clux
var a: u32 = f.a;
f.a = 7u32;
f.b += 2u32;
```

- 读取结果类型是底层无符号类型。
- 写入按字段宽度掩码。
- 不能对字段取地址。
- 指针接收者自动解引用：

```clux
var p: ref *Flags = f.&;
p.a = 4u32;
```

## 10.6 sizeof / alignof

`sizeof` / `alignof` 按隐藏存储数组计算：

```clux
var n: usize = sizeof(Flags);
```

## 10.7 限制

- 位域没有字段以外的方法；
- 位域字段不能被 `ref` 指向；
- 位域不参与 C 位域布局的未定义行为；
- 位域的跨字访问由语言定义，不依赖具体 C 编译器的位域布局。
