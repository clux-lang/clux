# bootstrap/ — clux 自举编译器

用 clux 写 clux 编译器。设计先行、代码按图索骥；与现有 C 实现**独立**，不做逐行翻译。

## 与仓库其余部分的关系

| 目录 | 角色 |
|---|---|
| `docs/whitepaper/` | **唯一规范**。自举编译器按它实现，不按 C 代码实现。 |
| `conformance/` | 可执行的语义对照，两个实现共用。自举撞到的偏差先在这里加用例。 |
| `src/` | 现有 C 实现。**只作为行为 oracle**（跑 conformance），不作为设计参考。 |
| `std/` | clux 标准库（唯一一份），编译器唯一的平台边界。 |
| `bootstrap/` | 本目录：自举编译器 + 它的设计文档。 |

## 设计文档（先写，后实现）

- [`design/00-architecture.md`](design/00-architecture.md)：总纲（分层、契约、数据模型、诊断、泛型、魔法派发、生命周期、cgen、验证、阶段）
- [`design/README.md`](design/README.md)：子设计文档规范与索引（`01-lexer` … `07-fixpoint`）

## 阶段

| 阶段 | 设计 | 实现 | 验收 |
|---|---|---|---|
| D0 | 00 总纲 + 子文档索引 | — | 设计评审 |
| S0 | 01-lexer + 02-std-core | `std` 骨架、`lexer.cx` | token dump 与 oracle 一致 |
| S1 | 03-ast | `parser.cx` + AST printer | AST dump 一致 |
| S2 | 04-resolve-semantics | `resolve.cx` + `sema.cx` 子集 | 子集 conformance 通过 |
| S3 | 05-tir-lower | `tir.cx` + `lower.cx` | MIR golden |
| S4 | 06-cgen | `cgen.cx` | 行为对照一致 |
| S5 | 07-selfhost | 全量 + 自举脚本 | `b2 = b1(b1)` |
| S6 | 08-fixpoint | 定点验证 | `b3` 行为一致 |

## 三条硬规矩

1. **先设计后实现**：每个阶段先有 `design/NN-*.md`（含输入/输出/错误契约与测试用例清单），再有代码。
2. **bug 回根目录**：自举撞到的编译器缺陷，在根 `conformance/` 加用例、修 C 实现，自举代码照规范书写，不绕路。
3. **确定性**：任何输出（诊断、生成 C、golden）不得依赖 hash 顺序或声明顺序。

## 当前状态

- [x] D0 总纲（`design/00-architecture.md`）
- [ ] S0 设计（lexer 契约、`std` 最小 API 面）
- [ ] S0 实现（`std` 骨架 + `lexer.cx` + token dump 对照）
