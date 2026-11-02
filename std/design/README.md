# std/design/ — 标准库设计文档

## 写作规范

每篇文档必须包含：**范围**（哪些模块/API）、**公开 API 面**（签名级）、**所有权与生命周期**
（谁分配、谁释放、是否可拷贝）、**错误契约**（`?T` 还是 panic）、**确定性要求**（迭代顺序等）、
**依赖**（只允许依赖 `std` 内模块）、**测试用例清单**、**开放问题**。

未写完设计的模块不进入实现；实现与文档不一致时以文档为准并立刻回填。

## 索引

| 编号 | 文档 | 覆盖模块 | 状态 |
|---|---|---|---|
| 00 | [decisions](00-decisions.md) | 决策台账（Idea 收集 + 已定/待定） | v0.1 持续更新 |
| 01 | [mem](01-mem.md) | `std::mem`（`Allocator` + `Arena`） | v0.1 |
| 02 | [str-string](02-str-string.md) | `std::str`、`std::string` | v0.1 |
| 03 | [vec](03-vec.md) | `std::vec` | v0.1 |
| 04 | [fmt](04-fmt.md) | `std::fmt`（`Writer` + `Display`） | v0.1 |
| 05 | [io-sys](05-io-sys.md) | `std::io`、`std::sys` | v0.1 |
| 06 | [map](06-map.md) | `std::map`（有序，可换 HashMap） | v0.1 |
| 07 | [args-diag-test](07-args-diag-test.md) | `std::args`、`std::diag`、`std::test` | v0.1 |

## 依赖方向（只允许向下）

```
args / diag / test
        │
      fmt ──→ string ──→ str
        │        │
        │        ▼
        └────→ vec ──→ mem
        │
        io ──→ sys（唯一 extern 边界）
        │
      map ──→ cmp(协议) + mem
```

- `str` 是语言内建视图，不是模块；`std::str` 只是自由函数集合。
- `sys` 只能被 `io`（以及 `mem.heap` 的实现）调用；其它模块不得 `extern`。
