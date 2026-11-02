# std/design/04-fmt.md — `std::fmt`：Writer 能力值 + `Display` 协议

> 状态：设计稿 v0.1（待评审）。依据：D-E2（长协议名）、D-B7/D-B8（能力值 `{ctx, fn}`，无 vtable）、
> D-F1（确定性）。`Writer` 形状已确认（需求）。
> 规范对应：§2.1 基础类型、§3.15.5 生命周期、§5.15 内建序列操作。

## 1. 范围

- 运行期**输出能力**：`Writer`（`{ctx, write}` 能力值，写向任意 sink）。
- 编译期**格式化协议**：`Display`（`format(self, w: *Writer)`）。
- 内建类型的确定性格式化：整数（2/8/10/16 进制）、布尔、字符串/字节（含转义）、浮点规则（定规则，S2 实现）。

不负责：具体 sink（`String`/文件/stdout → 见 `02-str-string`、`05-io-sys`）、表格对齐等排版糖。

## 2. `Writer`：能力值，不是接口对象

```clux
namespace std::fmt

struct Writer {
    ctx: opaque,
    write: func(ctx: opaque, data: []u8) -> void,
    /* 写入失败由 sink 记录；Display 实现不返回错误，避免每个实现都处理 ? */
    failed: bool,
}

func Writer.new(ctx: opaque, write: func(opaque, []u8) -> void) -> Writer
func (w: *Writer) write_bytes(self: *Writer, data: []u8): void
func (w: *Writer) write_str(self: *Writer, s: str): void          /* 原样写字节，不转义 */
func (w: *Writer) write_byte(self: *Writer, b: u8): void
func (w: *Writer) has_failed(self: *Writer) -> bool               /* 调用方最后检查一次 */
```

- **没有 vtable**：与 `Allocator` 同构——一个状态指针 + 一个函数指针。
- **错误模型**：`write` 不返回错误；sink 内部记录（如 `File` 写失败置 `failed`），调用方在末尾
  `has_failed()` 统一处理。理由：`Display` 是"生成文本"，不该把 I/O 错误穿过每一层格式化。
- 一个 `Writer` 可传 `*Writer` 给 `format`，也可被复制（两个 Writer 写同一 sink）。

## 3. `Display` 协议

```clux
namespace std::fmt

interface Display[Self] {
    func format(self: *Self, w: *fmt.Writer) -> void;
}

/* 泛型入口：用户类型（结构约束，不写 implements） */
func write_display[T extends Display[T]](w: *fmt.Writer, v: *T): void
```

- 方法名用**长名** `format`（D-E2）；与魔法方法 `__fmt__` 无关（后者被拒绝，D-E0b）。
- 类型只要提供 `func format(self: *T, w: *fmt.Writer) -> void` 即满足（E1.2 结构匹配）。
- `write_display` 只是把调用转发成 `v.format(w)`，不引入额外间接层。

## 4. 内建类型的格式化（确定性规则）

```clux
namespace std::fmt

func write_bool(w: *fmt.Writer, v: bool): void            /* "true" / "false" */
func write_i64(w: *fmt.Writer, v: i64): void              /* 十进制，含负号 */
func write_u64(w: *fmt.Writer, v: u64): void              /* 十进制 */
func write_u64_base(w: *fmt.Writer, v: u64, base: u8): void  /* base ∈ {2,8,10,16}，小写字母，无前缀 */
func write_i64_base(w: *fmt.Writer, v: i64, base: u8): void
func write_usize(w: *fmt.Writer, v: usize): void
func write_f64(w: *fmt.Writer, v: f64): void                 /* 规则见下，S2 实现 */

/* 诊断/转储常用：带引号与转义的字符串 */
func write_quoted_str(w: *fmt.Writer, s: str): void
func write_quoted_bytes(w: *fmt.Writer, b: []u8): void

/* 固定宽度（编译器转储用）：pad 只允许空格或 '0' */
func write_str_padded(w: *fmt.Writer, s: str, width: usize, left: bool): void
func write_u64_padded(w: *fmt.Writer, v: u64, width: usize): void
```

**转义规则（写死）**：`\n` `\t` `\r` `\\` `\"` `\0` 用短转义；其它 `< 0x20` 与 `0x7F` 输出
`\xNN`（两位小写十六进制）；`>= 0x80` 的字节**原样写出**（内容是 UTF-8），不转义。

**浮点规则（先定规则，S2 实现）**：最短可回读表示（round-trip），
`nan` / `inf` / `-inf` 固定文本，`-0.0` 输出 `-0`；不使用 locale，不使用 `%g`；
指数形式阈值与 `parse_f64` **共用同一实现**（见 `02-str-string.md` §7）。

## 5. 错误契约

| 场景 | 行为 |
|---|---|
| sink 写失败 | `Writer.failed = true`，后续写入成为 no-op；调用方 `has_failed()` |
| `base` 不在 {2,8,10,16} | panic（编程错误） |
| `width` 过大（> 1 MiB） | panic（防止误用导致巨量输出） |
| 浮点未实现阶段（S0/S1） | `write_f64` 编译期报"暂不支持"或 panic（实现阶段二选一，文档标 TODO） |

## 6. 确定性

1. 整型/布尔/转义规则固定，无 locale 参与。
2. 不输出指针、地址、`cap`、hash 值（D-F1 规则 4）。
3. `Writer` 本身不缓冲（除 sink 自身），因此输出顺序 = 调用顺序。
4. golden 测试必须覆盖：负数、`i64` 最小值、`u64` 最大值、各进制、全部转义字符。

## 7. 测试清单

| 用例 | 断言 |
|---|---|
| `fmt.int` | 0/负数/`i64` 最小/`u64` 最大/各进制 |
| `fmt.padded` | 左右对齐、宽度小于内容时不截断 |
| `fmt.escape` | 所有短转义 + 控制字节 `\xNN` + 非 ASCII 原样 |
| `fmt.display` | 用户类型只提供 `format` 即可被 `write_display` 调用（无需 implements） |
| `fmt.writerFail` | sink 失败后 `has_failed()` 为真且不再写 |
| `fmt.deterministic` | 同一输入两次运行逐字节一致 |

## 8. 开放问题

1. 是否需要 `Writer.write_fmt` 的 `{}` 风格模板语法？倾向**不做**——编译器用链式 `write_*` 更可控。
2. `write_f64` 的指数阈值细节（`1e21` 起用指数？）留到实现时定，但必须与 `parse_f64` 同源。
3. `String` sink 的构造位置：`std::string.writer(s: *String) -> fmt.Writer`（倾向放在 `string` 模块）。
