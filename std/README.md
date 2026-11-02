# std/ — clux 标准库

clux 的唯一标准库。**先设计、后实现**；编译器的 `std` 依赖与用户程序完全相同，
没有"编译器特权模块"。

## 定位

- 与 `src/core/`（C 运行时支撑）是**两层不同东西**：`src/core` 是当前 C 实现的内部支撑；
  `std/` 是 clux 语言级的库，两个编译器（C 实现、自举实现）都会消费它。
- 平台边界只在这里：`std::sys` 是唯一允许写 `extern cfunc` 的模块，其余模块只依赖 `std`。
- 后期整体独立成仓库（目录布局按"可 subtree 拆分"设计：无仓库内相对路径依赖）。

## 设计原则

1. **最小可用面**：只为"写编译器"需要的 API 负责，不追求大而全。
2. **值语义优先**：`Str`/`Slice` 是视图（不拥有），拥有型容器显式命名（`String`/`Vec`/`Map`）。
3. **错误用 `?T` 表达**，不 panic；只有"程序错误"（越界、null 解包）才 panic。
4. **确定性**：`Map` 迭代顺序必须由调用方显式指定（有序实现或 `sorted()`），禁止把 hash 顺序泄漏到输出。
5. **无隐藏分配**：所有分配走显式 `Allocator` 参数（或默认分配器 `std::mem::default()`）。

## 模块划分（S0 只需要前 5 个）

| 模块 | 内容 | 备注 |
|---|---|---|
| `std::mem` | 分配器接口、`Allocator`、`alloc/free/resize` | 编译器唯一的堆入口 |
| `std::str` | `Str` 视图、切片、查找、前缀/后缀、分割、trim、大小写 | 不分配 |
| `std::string` | `String`（拥有型字节串）：push/append/format | 依赖 `mem` |
| `std::vec` | `Vec[T]`：push/pop/insert/remove/slice/迭代 | 依赖 `mem` |
| `std::fmt` | 格式化为 `String`：`fmt.write(...)`、整型/浮点/字符串/自定义 | 诊断与 C 生成都用它 |
| `std::io` | 文件读/写、stdout/stderr、路径拼接 | S1 起需要 |
| `std::map` | 有序映射 `Map[K, V]`（编译器查表）：get/put/entries 有序迭代 | S2 起需要 |
| `std::args` | 命令行解析 | 驱动用 |
| `std::sys` | FFI：文件、进程、时间、内存 | 唯一 `extern cfunc` 处 |
| `std::test` | `test` 块运行时（断言、失败汇总） | 与根 conformance 机制对齐 |
| `std::diag` | 诊断渲染（文本/JSON） | 编译器与工具共用 |

## 与编译器的契约

- 编译器**只**通过上表模块访问平台能力；缺什么就补 `std`，不在编译器里写 FFI。
- 每个模块的公开 API 在 `std/design/NN-*.md` 里定稿，实现必须与文档一致（文档是唯一契约）。
- `std` 自身的测试用 `test` 块，随 `std` 一起编译运行；不依赖 C 实现的特有扩展。

## 状态

- [ ] 设计：`design/00-overview.md`（模块依赖图 + 最小 API 面）
- [ ] 实现：`mem` / `str` / `string` / `vec` / `fmt`（S0 范围）
