# std/design/02-str-string.md — 文本：`str`（视图）、`std::str`（自由函数）、`std::string`（拥有型）

> 状态：设计稿 v0.1（待评审）。依据：`00-decisions.md` D-B3（视图 vs 拥有）、D-B1/C5（显式分配器）、
> D-B4（move-only + `__dispose__`）、D-F1（确定性）、D-E2（长协议名）。
> 规范对应：§2.1 基础类型（`str`）、§2.4 切片、§3.15.5 生命周期、§5.15 内建序列操作。

## 1. 范围与术语

`str` 是**语言内建的字节串视图**（`{ ptr: *u8, len: usize }`，不拥有）。本设计不重新定义它，
只规定 std 在这两层上的 API：

| 层 | 类型 | 拥有 | 谁分配 |
|---|---|---|---|
| 视图 | `str`（内建）、`[]u8` | 否 | 不分配 |
| 自由函数 | `std::str`（模块内 `func`，第一个参数是 `str`） | 否 | 只有显式返回 `String`/`[]T` 的才分配，且分配器**必须传入** |
| 拥有型 | `std::string.String` | 是 | 自带 `Allocator` 字段 |

**不引入** `Str` 别名：直接用 `str`，避免两套名字。

## 2. `String` 的数据结构

```clux
namespace std::string

struct String {
    alloc: mem.Allocator,
    ptr: *u8,      /* 容量 cap 的字节缓冲；cap == 0 时为 nil */
    len: usize,    /* 有效字节数（不含结尾 0） */
    cap: usize,    /* 已分配字节数 */
}
```

- **不变量**：`len <= cap`；`cap > 0 ⇒ ptr != nil`；`ptr[0..len]` 是有效内容；
  **不保证** NUL 结尾（C 互操作走 `std::sys.as_cstr`，显式复制）。
- 编码：按 UTF-8 **字节**处理，**不做**有效性校验（与语言 `str` 一致）；大小写/排序只做 ASCII 规则。
- `String` 是 **move-only**：`__init_copy__`（移动，源清空）、`__dispose__`（释放缓冲）；
  `clone()` 显式深拷贝（要求传分配器）。

## 3. `String` 公开 API

```clux
namespace std::string

func String.new(alloc: mem.Allocator) -> String
func String.with_capacity(alloc: mem.Allocator, cap: usize) -> String
func String.from_str(alloc: mem.Allocator, s: str) -> String
func String.from_bytes(alloc: mem.Allocator, b: []u8) -> String

func (s: *String) len(self: *String) -> usize
func (s: *String) is_empty(self: *String) -> bool
func (s: *String) capacity(self: *String) -> usize
func (s: *String) clear(self: *String): void
func (s: *String) reserve(self: *String, extra: usize): void

/* 追加 */
func (s: *String) push_byte(self: *String, b: u8): void
func (s: *String) push_str(self: *String, t: str): void
func (s: *String) push_char(self: *String, c: char): void      /* UTF-8 编码后追加 */

/* 视图（借用；下一次会扩容的修改后失效） */
func (s: *String) as_str(self: *String) -> str
func (s: *String) as_bytes(self: *String) -> []u8              /* 可写视图，长度 len */

/* 修改内容（不扩容，长度不变） */
func (s: *String) byte_at(self: *String, i: usize) -> *u8      /* 越界 panic */
func (s: *String) get_byte(self: *String, i: usize) -> ?*u8    /* 越界 nil */

/* 生命周期 */
func (s: *String) __init_copy__(self: *String, other: *String): void
func (s: *String) __dispose__(self: *String): void
func (s: *String) clone(self: *String, alloc: mem.Allocator) -> String
```

**借用失效规则**（写进文档 + 测试）：`as_str`/`as_bytes`/`byte_at` 返回的视图/指针，
在下一次 `push_*`/`reserve`/`clear`/`__dispose__` 之后失效。

## 3.5 实现漂移记录（`str[lo:hi]` 的结果类型）

规范与实际实现：`str[lo:hi]` 的结果是 **`[]const u8`**（见 `05-expressions.md`
§5.9 与 `DriverM4.Str*` 用例），不是 `str`。因此 `std::str` 的视图函数当前返回
`[]const u8`（`slice_bytes` / `trim_bytes`），只用索引的地方返回 `usize`
（`trim_start_index` / `trim_end_index`）。

**待补（语言层）**：`[]const u8 -> str` 的显式转换（或 `str_from(ptr, len)` 内建）
一旦落地，`slice_bytes` / `trim_bytes` 就改回返回 `str`，API 与 §4 设计一致。
在此之前不要在 std 里手工拼 `str`（没有构造路径，且 `.str{}` 只接受已有 `str`）。

## 4. `std::str` 自由函数（不分配优先）

```clux
namespace std::str

func len(s: str) -> usize
func is_empty(s: str) -> bool
func byte_at(s: str, i: usize) -> ?u8
func slice(s: str, lo: usize, hi: usize) -> str                 /* 越界 panic */
func starts_with(s: str, prefix: str) -> bool
func ends_with(s: str, suffix: str) -> bool
func find_byte(s: str, b: u8) -> ?usize
func find(s: str, needle: str) -> ?usize                         /* 朴素搜索，O(n*m) */
func trim(s: str) -> str                                         /* 去 ASCII 空白 */
func trim_start(s: str) -> str
func trim_end(s: str) -> str
func equals(a: str, b: str) -> bool                              /* 字节相等 */
func compare(a: str, b: str) -> Ordering                         /* 字节字典序（确定性） */
func equals_ascii_ignore_case(a: str, b: str) -> bool

/* 解析（失败/溢出返回 nil，不 panic） */
func parse_i64(s: str) -> ?i64
func parse_u64(s: str) -> ?u64
func parse_f64(s: str) -> ?f64                                  /* 后期 */

/* 需要分配：分配器显式传入 */
func to_lower_ascii(alloc: mem.Allocator, s: str) -> String
func to_upper_ascii(alloc: mem.Allocator, s: str) -> String
func split_byte(alloc: mem.Allocator, s: str, sep: u8) -> []str  /* 快照数组 */
```

- `compare` 用无符号字节字典序；**不使用 locale**，`equals_ascii_ignore_case` 只折叠 `A-Z`。
- `parse_*` 的接受语法（写死并测试）：`parse_i64` 接受可选 `-`/`+`、十进制数字、无前导空白；
  溢出返回 `nil`；空串/非法字符返回 `nil`。
- `find` 只返回首个匹配的字节偏移（字符边界不做校验）。

## 5. 错误契约

| 场景 | 行为 |
|---|---|
| 分配失败（`push_*`/`from_str`/`clone`） | panic `"out of memory"`（`try_*` 变体后续加） |
| 越界读取（`slice`/`byte_at` 的 `i >= len`） | panic（`get_byte` 返回 `nil`） |
| `parse_*` 非法输入/溢出 | 返回 `nil`，不 panic |
| `__dispose__` 重复调用 | 幂等（释放后置 `ptr = nil, len = cap = 0`） |

## 6. 确定性与测试

- 任何输出（格式化、诊断）只能依赖字节内容，不依赖地址；`String` 的调试打印只输出内容与长度。
- 测试清单：

| 用例 | 断言 |
|---|---|
| `str.basic` | `len`/`is_empty`/`slice`/`startsWith`/`endsWith`/`find` |
| `str.trim` | 前后空白；全空白；空串 |
| `str.compare` | 字节序（区分大小写）；与 `equals` 一致 |
| `str.parse` | 正负、溢出、空串、非法字符、`+` 前缀 |
| `string.push` | 多次 `push_byte`/`push_str`/`push_char` 后内容正确（含扩容路径） |
| `string.grow` | 容量翻倍后旧视图失效但内容保留 |
| `string.move` | `__init_copy__` 后源为空；`clone` 独立 |
| `string.dispose` | 计数分配器上无泄漏；重复 dispose 幂等 |

## 7. 开放问题

1. `split_byte` 返回 `[]str` 快照（需分配）还是提供 `Split` 迭代器（需要 `std::iter`）；暂定快照。
2. 是否需要 `char` 级遍历（UTF-8 解码）在 S0–S4：暂不需要（编译器按字节处理）。
3. `parse_f64` 的舍入/格式化规则与 `04-fmt` 的浮点输出必须共用同一实现（避免两套规则）。
4. `String` 是否提供 `truncate(len)`/`remove_range(lo, hi)`：S2 再评估（编译器几乎不用）。
