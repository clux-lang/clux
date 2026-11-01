# clux 一致性用例集（conformance）

本目录是**用 clux 自己写的**语言一致性用例集：每个 `.cx` 文件对应白皮书的一章，
文件里的每个 `test "chapter.feature"` 块自声明一条规范要求，并用 `assert` 检查。

它有两个用途：

1. **当前实现的自检**：C 实现（本项目）通过 `clux test <file>` 逐条执行；
2. **未来自举/其它实现的标准**：用例只用语言自身的 `test` / `assert`（以及第 11 章的
   `mock`），不引用任何实现内部细节、C 运行时或本项目的测试框架。任何符合白皮书的
   实现，只要能运行 `test` 块，就能直接跑这套用例。

## 文件与章节对应

| 文件 | 白皮书章节 |
| --- | --- |
| `01-lexical.cx` | 1 词法与字面量 |
| `02-types.cx` | 2 类型 |
| `03-declarations.cx` | 3 声明 |
| `04-statements.cx` | 4 语句 |
| `05-expressions.cx` | 5 表达式 |
| `06-generics.cx` | 6 泛型与接口 |
| `08-modules/main.cx` | 8 模块（依赖 `08-modules/lib/shapes.cx`） |
| `09-metadata.cx` | 9 装饰器与元信息 |
| `10-bitfields.cx` | 10 位域 |
| `11-testing.cx` | 11 测试（`test` / `assert` / `mock`） |

## 运行

```sh
clux test conformance/01-lexical.cx
clux test conformance/08-modules/main.cx
# 或一次跑完（任一文件失败即非零退出）：
for f in conformance/*.cx conformance/*/main.cx; do clux test "$f" || exit 1; done
```

约定：`conformance/*/lib/` 下是**依赖模块**，不是入口文件，不要单独运行。

## 编写约定

- 一个 `test` 块的名称是 `章节.特性`，例如 `types.optional`、`statements.defer.lifo`；
- 只写**正向**用例（能编译、能跑通的形态）。需要在编译期拒绝的写法属于
  `docs/` 的规范描述与实现自己的负例测试，不放进这里——本目录必须能被任何
  实现整文件跑通；
- 用例之间不共享状态：需要共享的全局量在每个块内自行重置（例如
  `04-statements.cx` 的 `reset_trace()`）；
- 新增语言特性时，先在这里加一条自声明用例，再更新白皮书。
