# clux 测试语法与执行方案

> 状态：需求收集中。以下条目按用户口述逐条记录，尚未实现。
> 已完成的 mock 架构调研见 [test-mocking-design.md](test-mocking-design.md)。

---

## 1. 测试块语法与执行

### 1.1 顶层 `test` 块

在源码顶层声明：

```clux
test "TestSuit.TestName" {
    /* 测试内容 */
}
```

- `test` 是顶层语句块，不是普通函数定义；
- 名字是字符串，格式约定为 `"测试套件.测试名"`；
- 每个文件可以在顶层插入任意多个 `test` 块；
- 测试块内部是普通语句块，可调用文件内函数和测试内置接口。

### 1.2 测试内置接口

需要提供：

- `mock` / `unmock` 相关接口（见
  [test-mocking-design.md](test-mocking-design.md)）；
- `assert` 系列接口：断言表达式结果必须为 true。

`assert` 失败时：

- 打印被断言的表达式；
- 终止**当前测试用例**；
- 不影响下一个测试用例继续执行。

### 1.3 普通 build/run 忽略测试

- `clux build` / `clux run`（以及普通执行路径）中，`test` 块直接忽略；
- 只有 `clux test` 子命令才编译/执行测试块；
- “忽略”至少要求 parser 能识别并跳过，不进入普通 sema/编译器逻辑。

### 1.4 执行范围

单文件：

```sh
clux test aaa.cx
```

- 只执行 `aaa.cx` 中的 `test` 块；
- 不执行其他文件里的 test。

项目模式（存在 `clux.mod`）：

```sh
clux test
```

- 遍历当前项目源码中的 `test` 块并执行；
- 项目源码的范围、模块边界、重复测试名处理待补充。

### 1.5 输出

每个测试执行需要在 **stdout** 打印：

- 单个用例的结果；
- 汇总统计。

输出样式参考 gtest，例如：

```text
[==========] Running 3 tests from 2 test suites.
[ RUN      ] TestSuit.TestName
[       OK ] TestSuit.TestName (0 ms)
[  FAILED  ] TestSuit.Other
...
[==========] 3 tests from 2 test suites ran.
[  PASSED  ] 2 tests.
[  FAILED  ] 1 test.
```

具体字段和退出码待补充。

---

## 2. assert / mock 接口仅限 test 上下文

- `assert` 系列接口只能在 test 上下文中使用；
- `mock` / `unmock` 相关接口也只能在 test 上下文中使用；
- 在非 test 上下文使用这些接口必须**报错**。

待确认的边界：

- “test 上下文”是否仅指 `test { ... }` 块内部；
- test 块调用的普通 helper 函数里能否使用 assert/mock；
- 若允许，是运行期“当前正在执行测试”的动态检查，还是编译器把
  helper 标记为 test-only；
- 非 test 构建下这些符号是否完全不注册，从而自然产生未定义错误。

---

## 3. test 上下文允许 helper 使用测试接口

- test 块执行时设置“当前处于 test 上下文”；
- test 块调用的普通 helper 函数可以使用 `assert` / `mock` / `unmock`；
- 上下文需要能穿过普通函数调用、闭包调用和嵌套 helper；
- 非 test 上下文调用这些接口仍然报错；
- 测试结束自动 `unmock_all`；
- test 不允许嵌套；
- 失败终止当前用例时不需要显式恢复上下文（运行器统一处理）。

由此得到的实现语义：

- `assert` / `mock` 不能只靠词法位置判断；
- VM 需要一个“当前正在执行 test”的上下文标记；
- test 块进入时设置，退出/失败时由运行器统一清理；
- helper 是普通函数，编译期正常生成，调用时检查上下文；
- 如果一个 helper 含 assert 却在非 test 代码中被调用，运行期报
  “not in test context”（或按保留字规则明确报错）。

---

## 4. 实现模型

### 4.1 test 块就是 void 函数

每个 test 块可以视为：

```clux
func __clux_test_<n>(): void {
    /* test 块内容 */
}
```

- 无参数、返回 `void`；
- 名字与 `"TestSuit.TestName"` 的映射由编译器/测试注册表维护；
- 测试函数内部可以调用普通 helper；
- 不允许 test 块嵌套。

### 4.2 生成测试 main

编译器/测试运行器生成一个测试主函数，依次调用各 test 函数：

```clux
func __clux_test_main(): void {
    __clux_test_0();   // "A.one"
    __clux_test_1();   // "A.two"
    ...
}
```

运行器负责：

- 每个 test 前设置 test 上下文；
- 调用 test 函数；
- 捕获 assert 的穿透错误；
- 记录成功/失败与耗时；
- 每个 test 结束后 `unmock_all`；
- 继续下一个 test。

### 4.3 assert 是特殊的穿透返回错误

`assert(expr)`：

- `expr` 必须为 true；
- 失败时打印表达式文本；
- 返回一个**特殊的穿透错误值**，越过当前 helper/test 函数栈，
  由测试运行器捕获；
- 运行器把它记成当前用例失败，终止当前用例；
- 不终止后续用例，也不退出整个进程（除非最终汇总决定退出码）。

“穿透”要求：

- helper 不必手工检查/传递错误；
- VM 函数调用栈展开时，沿途作用域的 own/defer 清理仍需执行；
- 运行器能区分普通运行错误和 assert 失败，以便打印用例结果；
- 上下文和 mock 清理由运行器统一完成，业务代码不处理。

### 4.4 保留字与诊断

- `test`、`assert`、`mock`、`unmock` 等测试相关名字按保留字处理，
  不允许用户定义同名普通函数/变量（最终列表待定）；
- 非 test 上下文使用测试接口要给出明确诊断；
- 普通 build/run 不生成 test 函数和测试 main。

---

## 5. 范围与后端路线

测试语法保持最小集，暂不做：

- 覆盖率；
- 用例过滤；
- 并发执行；
- 参数化/数据驱动；
- setup/teardown 等额外生命周期。

这些留给未来真正的 C 后端实现。

执行后端路线：

1. 当前 `字节码 + VM` 是**临时态**；
2. 测试语法/标准库就绪后，整体切换到 C 后端；
3. C 后端功能验证完成后，再考虑迁移 LLVM。

因此：

- 测试语法、mock/assert 的语义要按**后端无关**设计；
- 当前 VM 实现只作为验证载体，后续可以被丢弃；
- 不为了 VM 的局部实现便利引入难以迁移的语义；
- C 后端可以用“每个 test 一个静态 void 函数 + 生成的测试 main”
  自然实现；assert 穿透可以用后端自己的错误/异常机制处理。

---

## 待补充

- assert 的具体形式（`assert(expr)` / `assert_eq` / 消息参数）
- 测试块能否有返回值（当前按无返回值）
- 失败后的进程退出码
- 项目模式下源码遍历规则
- mock 接口与 test 块的作用域/自动清理规则
