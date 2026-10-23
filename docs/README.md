# clux 文档

本目录是 clux 的**规范文档树**。它只描述 clux 语言当前应具有的语法、类型规则、
运行语义和工具行为，不记录实现演进过程、临时取舍或历史缺陷。

历史设计、实现笔记、里程碑与 AI 演进记录统一保存在仓库根目录的
`docs-backup/`，**不属于规范**。

## 阅读顺序

1. [语言白皮书总览](whitepaper/README.md)
2. [词法与源文件](whitepaper/01-lexical.md)
3. [类型](whitepaper/02-types.md)
4. [声明](whitepaper/03-declarations.md)
5. [语句](whitepaper/04-statements.md)
6. [表达式](whitepaper/05-expressions.md)
7. [泛型与接口](whitepaper/06-generics.md)
8. [所有权、生命周期与清理](whitepaper/07-ownership.md)
9. [模块、包与 FFI](whitepaper/08-modules.md)
10. [装饰器与元信息](whitepaper/09-metadata.md)
11. [位域](whitepaper/10-bitfields.md)
12. [测试](whitepaper/11-testing.md)
13. [工具链与格式](whitepaper/12-tooling.md)

## 工程路线（非规范）

- [C 后端路线](roadmap/c-backend.md)

## 规范用词

- **必须**：语言或工具强制要求；违反即诊断或拒绝。
- **应当**：规范行为；实现可以给出等价结果，但不得改变可观察语义。
- **可以**：允许的写法或实现自由度。
- **未定义**：语言不承诺行为，程序不应依赖。

示例默认是规范示例。示例中的 `T`、`U` 表示类型参数，`N` 表示编译期整数。

## 文档边界

本树不描述：

- 字节码指令、VM 内部结构；
- 编译阶段编号与临时实现方案；
- C 后端尚未完成的内部计划；
- 历史设计争论、失败方案与回归记录；
- 特定提交或 AI 会话的经验总结。

这些内容属于实现或历史记录，不能作为语言规范引用。

## 版本

当前规范对应 clux 源码树中的解析器、语义分析与标准测试套件。
后端实现（VM 或 C）不改变本规范定义的源语言。
