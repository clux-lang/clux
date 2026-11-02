# std/design/06-map.md — `std::map`（有序映射 → 未来可换 HashMap）

> 状态：设计稿 v0.1（待评审）。依据：`00-decisions.md` D-C2（**已定：当前有序**，且换 HashMap 的成本要低）、
> D-C5（容器内嵌 `Allocator`）、D-E2（长协议名）、D-F1（确定性）。
> 规范对应：§3.7 tag union、§6.5 结构约束、§4.9 foreach（Map 不直接可索引，用 `entries()`）。

## 1. 决策：当前用有序映射，但**契约按"实现无关"写**

- 现在：`Map[K, V]` = **有序数组 + 二分查找**（插入 O(n)、查找 O(log n)），`K extends std::cmp::Ordered[K]`。
- 将来：可以换成 HashMap（`K extends Hash[K]` + `equals`），**不改变调用点**。
- 让切换便宜的三条硬约束（本设计的核心）：
  1. **`Map` 不透明**：不暴露节点/树的任何概念；所有遍历只能走 `entries()`。
  2. **`entries()` 的契约是"按 key 全序"**：HashMap 实现要么维护有序索引、要么在 `entries()` 时排序。
     契约不变 ⇒ golden 测试与调用点不变。
  3. **不提供只对树有意义的 API**（`first_key`/`last_key`/`range(lo, hi)`）。
     这类需求将来作为**独立扩展 API**（`ordered_range(...)`）提供，不混进 `Map` 主接口。

## 2. 数据结构

```clux
namespace std::map

/* 一条键值对（`entries()` 的返回元素；字段只读视图）。 */
struct Entry[K, V] {
    key: K,
    value: V,
}

/* 有序映射：条目数组按 key 升序排列，二分查找定位。 */
struct Map[K, V] {
    alloc: mem.Allocator,
    items: *Entry[K, V],   /* 容量 cap 的连续存储 */
    len: usize,
    cap: usize,
}
```

- 容量增长：`cap` 从 8 起翻倍；`alloc` 失败 → panic（`try_*` 变体返回 `?T`）。
- **不变量**：`items[0..len]` 按 `key.compare` 严格升序（`compare` 相等视为同键，不允许重复键）。
- `Map` 是 move-only（`__init_copy__` 移动 + `__dispose__` 释放）；`clone()` 显式深拷贝。

## 3. 公开 API

```clux
namespace std::map

func Map[K extends Ordered[K], V].new(alloc: mem.Allocator) -> Map[K, V]
func Map[K, V].with_capacity(alloc: mem.Allocator, cap: usize) -> Map[K, V]

/* 查询：key 按协议签名以指针传入（与 compare 一致），返回 value 的借用。 */
func (m: *Map[K, V]) get(self: *Map[K, V], key: *K) -> ?*V
func (m: *Map[K, V]) contains(self: *Map[K, V], key: *K) -> bool
func (m: *Map[K, V]) get_or_insert(self: *Map[K, V], key: K, init: V) -> *V   /* 可选，S2 后再加 */

/* 修改 */
func (m: *Map[K, V]) put(self: *Map[K, V], key: K, value: V) -> bool   /* true = 新增，false = 覆盖 */
func (m: *Map[K, V]) remove(self: *Map[K, V], key: *K) -> ?V          /* 返回值（移出） */
func (m: *Map[K, V]) clear(self: *Map[K, V]): void
func (m: *Map[K, V]) reserve(self: *Map[K, V], extra: usize): void

/* 规模 */
func (m: *Map[K, V]) len(self: *Map[K, V]) -> usize
func (m: *Map[K, V]) is_empty(self: *Map[K, V]) -> bool

/* 遍历：分配一个快照数组，按 key 全序（确定性契约）。 */
func (m: *Map[K, V]) entries(self: *Map[K, V], alloc: mem.Allocator) -> []Entry[K, V]

/* 生命周期 */
func (m: *Map[K, V]) __init_copy__(self: *Map[K, V], other: *Map[K, V]): void
func (m: *Map[K, V]) __dispose__(self: *Map[K, V]): void
func (m: *Map[K, V]) clone(self: *Map[K, V], alloc: mem.Allocator) -> Map[K, V]
```

**借用规则**：`get` 返回的 `?*V` 在下一次修改（`put`/`remove`/`reserve`/`clear`/`dispose`）后失效；
`entries()` 返回的是**快照**（拷贝），不受后续修改影响。两条都必须写进文档与测试。

## 4. 错误契约

| 场景 | 行为 |
|---|---|
| `new`/`reserve`/`put` 分配失败 | panic（`std::rt.panic("out of memory")`） |
| `get` 键不存在 | `nil`（`?*V`） |
| `remove` 键不存在 | `nil`（`?V`） |
| 重复键 | `put` 覆盖并返回 false；**不报错** |
| `entries()` 分配失败 | panic（后续可加 `try_entries`） |

## 5. 换成 HashMap 的成本分析（关键一节）

| 维度 | 现在（有序数组） | 换 HashMap 时 | 调用点是否要改 |
|---|---|---|---|
| `K` 约束 | `Ordered`（`compare`/`equals`） | 追加 `Hash`（`hash`） | 否（类型只需**多实现一个方法**） |
| 查找复杂度 | O(log n) | 期望 O(1) | 否 |
| 插入复杂度 | O(n)（搬移） | 期望 O(1) | 否 |
| 迭代顺序 | key 全序 | 必须维持 key 全序（排序或维护有序索引） | 否（契约不变） |
| `Map` 内存布局 | 有序数组 | 桶数组 + 元数据 | 否（`Map` 不透明） |
| `entries()` 返回 | 直接快照 | 排序后的快照 | 否 |
| 树专属 API | 不提供 | 不提供 | — |

**结论**：切换只需要（a）给 key 类型补 `hash` 方法，（b）把 `Map` 内部换成桶结构并保持
`entries()` 的 key 序契约。**API、调用点、golden 测试都不用动。**

将来加速切换的一个可选项：`comptime if (K extends Hash[K])` 在 `Map` 内部按实例选择实现
（两套代码，一个类型名），这样"有序/哈希"可以共存而不是二选一。这条**不现在做**，但 API
契约已经按它的前提写好了。

## 6. 确定性与测试

- `entries()` 必须按 `compare` 全序；`compare` 返回 `Equal` 的键不可能同时存在（同键覆盖）。
- 诊断/日志里打印 Map 只能走 `entries()`（不得打印内部数组地址或容量）。
- 测试清单：

| 用例 | 断言 |
|---|---|
| `map.putGet` | 插入/覆盖/查找；`put` 返回值正确 |
| `map.order` | 乱序插入后 `entries()` 按 key 升序 |
| `map.remove` | 删除中间/首尾元素后仍有序；返回值正确 |
| `map.grow` | 超过初始容量后仍正确（realloc 路径） |
| `map.borrowInvalidation` | `get` 后 `put` 使借用失效（文档 + 测试双重保证） |
| `map.moveDispose` | `__init_copy__` 移动后源为空；`__dispose__` 无泄漏（计数分配器） |
| `map.deterministic` | 同一插入序列连跑两次，`entries()` 逐字节一致 |

## 7. 开放问题

1. `get_or_insert` 是否需要（编译器符号表常用）；倾向 S2 再加。
2. `remove` 是否要保留"被删键"给调用方（`?Entry[K,V]`）；倾向 `?V`，键本来就在调用方手里。
3. 是否提供 `keys()`（`[]*K`）；倾向不加，让调用方用 `entries()`。
4. 元素搬移成本：`Entry[K,V]` 是值数组（无间接），插入搬移 O(n) 字节级；若将来 Map 变大，
   换 B 树时 `Entry` 结构不变（仍是值），调用点不受影响。
