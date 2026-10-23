# clux 位域（bitfield）语义

> 状态：VM 阶段已实现（2026-10-23）。

## 语法

```clux
bitfield Flags: u32 {
    a: 3;
    b: 5;
}
```

- 底层类型只能是 `u8` / `u16` / `u32` / `u64`；
- 字段宽度是整数字面量，范围 `1..底层类型位宽`；
- 字段名唯一；
- 至少一个字段；
- 字段按声明顺序从 **LSB** 开始连续排布；
- 总位数不受底层类型宽度限制，允许超过一个存储字；
- 字段可以跨底层字的边界；
- 存储为 `ceil(total_bits / word_bits)` 个底层无符号字；
- `sizeof(Name) / alignof(Name)` 按隐藏存储数组计算。

## 字段读写

```clux
var f: Flags = .Flags{ .a = 1u32, .b = 3u32 };
var a = f.a;       // u32
f.a = 7u32;        // 写低 3 位
f.b += 2u32;       // 复合赋值
```

- 字段读取返回底层无符号类型（上例 `u32`）；
- 写入值截断到字段宽度（掩码），不 panic；
- 字段不能取地址/生成 `ref`；
- 指针 receiver 自动解引用：
  ```clux
  var p: ref *Flags = f.&;
  p.a = 4u32;
  ```

## 构造

```clux
var f: Flags = .Flags{ .a = 1u32 };
```

- 必须使用具名字段；
- 缺失字段补零；
- 允许 `i32` 等整数字面量参与构造/写入（按低 N 位截断）；
- 字段值超出宽度时掩码，不报错；
- 空构造 `.Flags{}` 全部为零。

## 布局规则

对 `word_bits = 8/16/32/64`：

```text
bit_offset[0] = 0
bit_offset[i] = bit_offset[i-1] + width[i-1]
word_count = ceil(total_bits / word_bits)
```

字段 i 位于连续位流的 `[offset, offset+width)`；访问按字读取/写回，
跨字时组合相邻两个字。

## 当前限制

- 不支持 `new Bitfield{...}`（堆上的位域暂未实现）；
- 不支持 bitfield 方法、嵌套 bitfield；
- 不支持在 bitfield 内声明静态成员；
- 字段读取类型是底层无符号类型，比较/运算需要匹配无符号类型
  （例如 `1u32`）；
- 反射暂时只看到隐藏的存储字段，没有 bitfield 专用 kind。

## 实现落点

- parser：`src/parser/parse_bitfield.c`，把 bitfield 转成带隐藏
  `__bits: [N]underlying` 字段的 struct AST + 元数据；
- sema：`struct_type_t::bitfield_ast`、字段读写标记、构造重排；
- compiler：`BITFIELD_GET` / `BITFIELD_SET` / `BITFIELD_MAKE`；
- VM：`src/vm/exec_bitfield.c` 连续位流读写；
- formatter：`bitfield` 原样输出。
