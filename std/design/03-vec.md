# std/design/03-vec.md — `std::vec`

> 状态：设计稿 v0.1（待评审）。依据：D-B1/C5（显式分配器、容器内嵌）、D-B3（视图 vs 拥有）、
> D-B4（move-only + `__dispose__`）、D-C3（`foreach` 走 `__len__`/`__index__`）、D-F1（确定性）。
> 规范对应：§2.4 切片/数组、§3.15.4 下标与切片、§3.15.5 生命周期、§4.9 foreach。

## 1. 范围

拥有型动态数组 `Vec[T]`（元素按值连续存储），以及"数组 ↔ 切片 ↔ Vec"的显式转换。
不负责：`String`（见 `02-str-string`）、排序等算法（后续 `std::sort`）。

## 2. 数据结构

```clux
namespace std::vec

struct Vec[T] {
    alloc: mem.Allocator,
    ptr: *T,      /* 容量 cap 的连续存储；cap == 0 时为 nil */
    len: usize,
    cap: usize,
}
```

- **不变量**：`len <= cap`；`cap > 0 ⇒ ptr != nil`；`ptr[0..len]` 是已初始化元素，
  `ptr[len..cap]` 是未初始化区域（不得读取）。
- 增长：`cap` 从 **8** 起，不足时**翻倍**；溢出检查（`cap * sizeof(T)` 溢出 → panic）。
- 元素移动：`push`/`insert`/`remove` 用**按值搬移**（`T` 的位拷贝 + 源置空语义由容器负责；
  对 move-only 元素，容器内部用 `__init_copy__` 语义搬运——实现层直接按字节搬移并视为已移出）。
- `Vec[T]` 是 **move-only**：`__init_copy__` 移动（源清空）、`__dispose__` 释放缓冲 + 元素析构
  （对每个元素调用 `dispose(elem)`，无 `__dispose__` 的类型静默）；`clone()` 深拷贝。

## 3. 公开 API

```clux
namespace std::vec

func Vec[T].new(alloc: mem.Allocator) -> Vec[T]
func Vec[T].with_capacity(alloc: mem.Allocator, cap: usize) -> Vec[T]
func Vec[T].from_slice(alloc: mem.Allocator, s: []T) -> Vec[T]      /* 逐元素拷贝 */

func (v: *Vec[T]) len(self: *Vec[T]) -> usize
func (v: *Vec[T]) is_empty(self: *Vec[T]) -> bool
func (v: *Vec[T]) capacity(self: *Vec[T]) -> usize
func (v: *Vec[T]) clear(self: *Vec[T]): void                        /* 析构元素，len = 0，容量保留 */
func (v: *Vec[T]) reserve(self: *Vec[T], extra: usize): void

/* 访问 */
func (v: *Vec[T]) at(self: *Vec[T], i: usize) -> *T                  /* 越界 panic */
func (v: *Vec[T]) get(self: *Vec[T], i: usize) -> ?*T                /* 越界 nil */
func (v: *Vec[T]) as_slice(self: *Vec[T]) -> []T
func (v: *Vec[T]) as_ptr(self: *Vec[T]) -> *T

/* 修改 */
func (v: *Vec[T]) push(self: *Vec[T], value: T): void
func (v: *Vec[T]) pop(self: *Vec[T]) -> ?T                          /* 尾元素移出 */
func (v: *Vec[T]) insert(self: *Vec[T], i: usize, value: T): void   /* i <= len；越界 panic */
func (v: *Vec[T]) remove(self: *Vec[T], i: usize) -> T              /* 越界 panic；后续元素前移 */
func (v: *Vec[T]) swap(self: *Vec[T], a: usize, b: usize): void
func (v: *Vec[T]) truncate(self: *Vec[T], n: usize): void           /* 析构 len-n 个尾部元素 */

/* 生命周期 */
func (v: *Vec[T]) __init_copy__(self: *Vec[T], other: *Vec[T]): void
func (v: *Vec[T]) __dispose__(self: *Vec[T]): void
func (v: *Vec[T]) clone(self: *Vec[T], alloc: mem.Allocator) -> Vec[T]

/* foreach 支持（语言内建路径：__len__ + __index__） */
func (v: *Vec[T]) __len__(self: *Vec[T]) -> usize
func (v: *Vec[T]) __index__(self: *Vec[T], i: usize) -> *T
```

- `at`/`__index__` 返回**借用指针**，在下一次 `push`/`insert`/`remove`/`reserve`/`clear`/`__dispose__`
  之后失效；`as_slice` 同理。
- `foreach (var x of v)` 走 `__len__` + `__index__`；`v[i]` 与 `v[i] = ...` 同样走 `__index__`。
- 越界策略与语言内建数组/切片一致（§3.15.4）：`at`/`__index__` panic（带索引值），`get` 返回 `nil`。

## 4. 错误契约

| 场景 | 行为 |
|---|---|
| 分配失败（`push`/`reserve`/`from_slice`/`clone`） | panic `"out of memory"` |
| `at`/`__index__`/`insert`/`remove`/`truncate` 越界 | panic，消息含索引与长度 |
| `pop` 空 | `nil` |
| 容量溢出（`cap * sizeof(T)` 溢出） | panic `"capacity overflow"` |
| `__dispose__` 重复调用 | 幂等（清空后 `ptr = nil, len = cap = 0`） |

## 5. 确定性

- 迭代顺序 = 下标顺序；`Vec` 的调试打印只输出长度与元素（不输出 `ptr`/`cap` 地址）。
- `from_slice`/`clone` 不改变元素顺序。
- 任何"容量"信息不得出现在诊断的稳定输出里（容量是实现细节，允许在不同分配器下不同——
  因此**不要**在 golden 里断言 `cap`）。

## 6. 测试清单

| 用例 | 断言 |
|---|---|
| `vec.pushPop` | 连续 push 超过 8 个（跨扩容）；pop 顺序正确；空 pop = nil |
| `vec.insertRemove` | 头/中/尾插入删除后元素顺序正确 |
| `vec.atPanics` | 越界 `at` panic；`get` 返回 nil |
| `vec.foreach` | `foreach` 遍历顺序与下标一致；`v[i] = x` 生效 |
| `vec.moveClone` | `__init_copy__` 后源空；`clone` 独立（改一个不影响另一个） |
| `vec.dispose` | 有 `__dispose__` 的元素被逐个析构；计数分配器无泄漏 |
| `vec.truncate` | 尾部元素被析构，`len` 正确 |
| `vec.capNotInGolden` | 格式化输出不含容量 |

## 7. 开放问题

1. `extend(slice)` / `append(Vec)`（移动语义）：S2 需要时再加。
2. `shrink_to_fit`：暂不提供（编译器不需要）。
3. 元素搬移的实现细节（memcpy vs 逐元素 `__init_copy__`）：设计上要求"搬移后源视为已移出"，
   实现选择留给 `03-vec` 的实现文档；对外语义不变。
4. 是否需要 `Vec[T].try_push`：OOM 策略已定（默认 panic），`try_*` 后续按需加。
