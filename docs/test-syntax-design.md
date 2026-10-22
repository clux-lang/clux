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

## 6. 实施评估

整体结论：**语法和最小执行框架可实现；难点集中在“assert 穿透错误
如何只终止当前用例并继续”、测试块到函数的转换、以及 mock 的方法表。**

### 6.1 低难度

- 顶层 `test "Suite.Name" { ... }` 的解析：
  - lexer/parser 增加 `AST_TEST`；
  - 普通 build/run 在 sema/compiler 前丢弃；
  - 测试模式收集到测试表。
- 每个 test 转换成一个无参 `void` 函数：
  - test 块本身就是语句块，AST_BLOCK 可直接作为函数体；
  - 生成唯一函数名（模块 + 序号）。
- 测试结果输出、耗时、汇总统计：
  - driver 层 printf 即可；
  - gtest 风格是纯展示。
- 测试上下文标志：
  - `vm_t` 增加 `in_test`（或测试上下文对象）；
  - runner 每个用例前设置/清理。

### 6.2 中等难度

- **assert 表达式文本**
  - AST 只有 token 起止下标；需要从 token pool 还原源码文本；
  - parser 在解析 `assert(expr)` 时直接生成
    `__assert(bool, "expr source")` 最稳；
  - 不需要运行时反射。
- **assert / mock 只允许在测试上下文**
  - 内置符号在普通构建可以不注册，但 helper 复用会受影响；
  - 采用运行期 `in_test` 检查，报错清晰；
  - 保留字列表要定：`test`、`assert`、`mock`、`unmock`、
    `mock_method`、`unmock_method`。
- **项目模式遍历**
  - 单文件简单；
  - `clux.mod` 下需要复用项目的源码发现逻辑，收集所有模块的 test；
  - 测试名跨文件重复需要规则（建议报重复错误）。
- **mock 自由函数**
  - 当前自由函数调用发 `PUSH "name"`，全局/模块作用域槽位可替换；
  - 需要 VM 内建 `mock/unmock` + mock 栈；
  - 签名校验、原值保存、自动 unmock_all。
- **测试函数注册**
  - 不是普通用户函数；需要 `is_test` 标记或独立测试表；
  - 测试函数不进入正常入口调用。

### 6.3 高难度 / 需要先定方案

1. **assert 穿透错误与继续执行**
   - VM 的错误返回会从当前调用链一路返回到 `exec_drive`；
   - `func_vcall` 的 error 路径会销毁 callee 作用域子树，但**不走
     defer**（代码里明确避免触发 defer）；
   - 因此 assert 失败：
     - own 资源通常由作用域销毁释放；
     - 用户 `defer` 清理不会执行；
   - 如果测试允许 defer/临时清理，这是必须解决的点。

   可选方案：
   - 为 test 错误单独实现“干净展开”，执行 defer + FREE 后再返回；
   - 或明确测试失败时不保证 defer（不推荐作为长期语义）；
   - C 后端可用 `setjmp/longjmp` 或错误返回自然实现。

2. **“生成测试 main 顺序调用”与继续执行冲突**
   - 如果测试 main 是普通 clux 函数，第一个 assert 错误会终止整个
     main，后续用例不会执行；
   - 当前 VM 没有 try/catch，测试 main 内无法捕获并继续；
   - 因此当前实现应由 **host runner 逐个调用测试函数**：
     ```
     for each test:
         set in_test
         call test_fn()
         if error: record failure
         unmock_all
         clear in_test
     ```
   - “测试 main”可作为 C 后端的生成物；VM 阶段用 driver 循环更实际。

3. **mock struct 方法**
   - 调用点走 `MEMBER_CALL`，方法表在
     `struct_type->attributes`，运行期可替换；
   - 需要：
     - 保存原方法值 clone；
     - 在 member_scope/attributes 安装 replacement；
     - `unmock_method` 恢复；
     - 处理继承 owner、静态方法、泛型实例；
   - 方法 replacement 的签名必须包含 `self`（实例方法）；
   - 现有用户 API `mock("name", fn, repl)` 不适合方法，需要
     `mock_method(Type, "name", repl)`。

4. **测试失败的错误类型识别**
   - 需要区分：
     - assert 失败（用例失败，继续下一个）；
     - 普通运行错误/panic（用例失败，继续下一个，或中止整个测试？）；
     - 编译错误（不执行）。
   - 建议所有用例级错误都记录失败并继续，最后统一退出码。

### 6.4 暂不做

- 覆盖率、过滤、并发、参数化、setup/teardown；
- 实例级 mock；
- C 后端的具体生成（当前 VM 为临时态）。

### 6.5 推荐落地顺序

1. AST_TEST + 普通构建丢弃 + 单文件测试模式；
2. 生成测试函数 + host runner + gtest 风格输出；
3. test 上下文 + `assert(bool, "expr")` + 用例失败继续；
4. 解决 assert 错误的 defer/FREE 展开语义；
5. mock 自由函数 + 自动 unmock_all；
6. mock 实例/静态/泛型方法；
7. 项目模式 `clux.mod` 遍历；
8. 最后迁移到 C 后端时重写 runner（语义保持不变）。

---

## 待补充

- assert 的具体形式（`assert(expr)` / `assert_eq` / 消息参数）
- 测试块能否有返回值（当前按无返回值）
- 失败后的进程退出码
- 项目模式下源码遍历规则
- mock 接口与 test 块的作用域/自动清理规则
