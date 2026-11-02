# std/design/01-mem.md — `std::mem`（分配器与 Arena）

> 状态：设计稿 v0.1（待评审）。依据：`00-decisions.md` 的 D-B1/B2/B5/B6、D-A5。
> 规范对应：§2.12 布局（对齐）、§3.15.5 生命周期（`__dispose__`）。

## 1. 范围

负责：`Allocator` 值类型、堆分配器、Arena、`sizeof/alignof` 相关约定、OOM 策略。
不负责：容器实现（见 `03-vec`/`02-str-string`）、文件/IO（见 `05-io-sys`）。

## 2. 设计原则

1. **显式注入**：任何分配都来自调用方传入的 `Allocator`；`std` 不提供隐式全局分配器。
2. **不用 vtable**：`Allocator` 就是**显式持有的函数指针 + 状态指针**（`ctx`），没有分发表、没有间接层。
3. **机制与策略分离**：分配器只做机制（成功返回地址、失败返回 `0`）；"失败怎么办"由调用方决定
   （容器默认 panic，`try_*` 变体返回 `?T`）。
4. **确定性**：分配地址不得进入任何可观察输出（诊断、生成的 C、golden）。

## 3. 数据结构

```clux
/* 分配器是**值**：可拷贝、可比较"是否同一分配器"（比较 ctx + alloc 指针）。 */
struct Allocator {
    /* 状态指针：无状态分配器（libc）填 nil；Arena 填自身地址。 */
    ctx: opaque,
    /* 分配 size 字节、按 align 对齐；失败返回 0（nil）。align 必须是 2 的幂。 */
    alloc: func(ctx: opaque, size: usize, align: usize) -> opaque,
    /* 释放 alloc 返回的指针；free(nil) 必须是 no-op。 */
    free: func(ctx: opaque, ptr: opaque) -> void,
}
```

**为什么必须有 `ctx`**：Arena / bump / 统计分配器都有状态，纯函数指针无法携带；
`ctx` 把它们统一进同一个 `Allocator` 值（libc 分配器把 `ctx` 设为 `nil`）。这不是 vtable：
只有两个函数指针，没有"按类型查表"的分派。

**为什么 `alloc` 带 `align`**：Arena 与 SIMD 场景需要按 `alignof(T)` 对齐；libc 实现可忽略
（malloc 已满足最大对齐）。**为什么 `free` 不带 size/align**：需要记账的分配器可在块头存元数据；
不带 size/align 让调用点更简单，也让 Arena 的 `free` 天然 no-op。

## 4. 公开 API

```clux
namespace std::mem

func heap() -> Allocator                    /* libc malloc/free，ctx = nil */
func null() -> Allocator                    /* 全零：任何分配都失败（测试/禁用分配） */

/* Arena：向 backing 按 chunk 申请，按块递增分配；释放时整块归还。 */
struct Arena {
    backing: Allocator,
    chunk: usize,
    cur: opaque,          /* 当前块起点 */
    cur_size: usize,      /* 当前块大小 */
    used: usize,          /* 当前块已用 */
    cur_free: func(ctx: opaque, ptr: opaque) -> void,  /* 见下：当前块的 free */
}

func Arena.new(backing: Allocator, chunk: usize) -> Arena
func Arena.allocator(self: *Arena) -> Allocator   /* ctx = self，alloc 指向 arena_alloc */
func Arena.reset(self: *Arena): void              /* 归还除第一块外的块，used = 0 */
func Arena.__dispose__(self: *Arena): void        /* 归还全部块 */
```

- `Arena.allocator()` 返回的 `Allocator` 里 `free` 是 no-op（整块释放），
  所以**不要**把 Arena 分配的对象交给普通 `free`。
- `chunk` 默认取 64 KiB；`null()` 分配器用于"禁止分配"的测试（容器必然 panic 或返回 `?T`）。

## 5. 错误契约（OOM）

| 场景 | 行为 |
|---|---|
| 分配器 `alloc` | 失败返回 `0`，**不 panic**（机制层不决定策略） |
| 容器默认构造（`Vec.new`） | 拿不到内存 → `std::rt.panic("out of memory")` |
| 容器 `try_*`（`Vec.try_push` 等） | 返回 `?T`，不 panic |
| `Arena.new` 的 backing 失败 | panic（编译器场景没有恢复策略）；`Arena.try_new` 后续再加 |

**不变量**：
- `alloc` 返回非 0 指针 ⇒ 之后的 `free(同一指针)` 必须成功（一次）。
- `free(nil)` = no-op。
- `align` 必须是 2 的幂；`align == 0` 视为 1。
- 分配器不得修改 `ctx` 之外的全局状态（`heap()` 的 ctx 是 nil，天然满足）。

## 6. 确定性要求

1. `Allocator` 的打印/诊断**不得**输出 `ctx` 或函数指针地址（只输出 `"std::mem.heap"` 这类名字）。
2. `Arena` 的迭代/遍历顺序（目前没有）若将来加入，必须按分配顺序。
3. 容器 `sizeof` 只依赖 `allocof`，不依赖分配地址。

## 7. 测试用例清单

| 用例 | 断言 |
|---|---|
| `mem.heapAllocFree` | `heap()` 分配/释放后可用；`free(nil)` 无副作用 |
| `mem.nullPanics` | 用 `null()` 构造容器 → panic（或 `try_*` 返回 nil） |
| `mem.alignRespected` | 按 8/16 对齐的分配地址满足 `addr % align == 0` |
| `mem.arenaBump` | Arena 连续分配不重叠；`reset` 后重新可用 |
| `mem.arenaDispose` | `dispose(a)` 归还全部块（用计数分配器验证 free 次数） |
| `mem.noAddrInOutput` | 诊断/格式化不输出指针值（golden 断言） |

## 8. 开放问题

1. `Allocator` 是否加 `realloc` 函数指针？（暂定不加：容器用 alloc+copy+free 实现）
2. `free` 是否带 size/align 以便统计分配器少一次查表？（暂定不带）
3. `Arena` 的 `cur_free` 字段是否必要？（暂定去掉：`Arena.allocator()` 的 free 直接 no-op）
4. 是否需要 `Tracking`/`Counting` 分配器（测试用）？（倾向：放在 `std::mem.test`，随测试构建）
