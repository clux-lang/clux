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

- 每个 test 前设置 test 上下文与失败标志；
- 调用 test 函数；
- assert 失败不会以普通 error 中断 main，而是让 test 函数提前
  return，main 继续调用下一个；
- 记录成功/失败与耗时；
- 每个 test 结束后 `unmock_all`；
- 继续下一个 test。

### 4.3 assert 等价于当前 test 的提前 return

`assert(expr)`：

- `expr` 必须为 true；
- 失败时打印表达式文本；
- 语义上等价于**当前 test 的一条 `return`**：
  - 设置当前 test 的失败标志；
  - 退出当前 test 函数；
  - 不继续执行 test 中 assert 之后的语句；
  - 下一个 test 继续执行；
- 不是普通运行错误，也不是 panic。

因为允许 helper 使用 assert，所以需要“穿透到 test 边界”的
return：

- 在 test 函数体内：直接提前 return；
- 在 helper 内：把“test 失败 return”信号穿过 helper 调用帧，
  直到当前 test 函数；
- 沿途每个帧按正常 return 语义执行 defer / own FREE 清理；
- test 函数返回后，main 已能继续下一个用例。

因此 assert 的展开类似：

```clux
if (!expr) {
    print("assert failed: <expr source>");
    __test_fail_return();   // 等价 return，穿透到 test 边界
}
```

上下文和 mock 清理由运行器统一完成；业务代码不处理。

### 4.4 保留字与诊断

- `test`、`assert`、`mock`、`unmock`、`mock_method`、
  `unmock_method` 按保留字处理，不允许用户定义同名普通函数/变量；
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

1. **assert 作为跨帧 test-return 的清理**
   - assert 语义是当前 test 的 return，不是普通 error；
   - helper 内 assert 需要把 return 信号传到 test 边界；
   - 沿途每个帧都要执行正常 return 的 defer / FREE；
   - 当前 VM 的 error 路径会销毁作用域子树但**不走 defer**，
     不能直接复用为 assert；
   - 需要单独实现“test-return 信号 + 正常 return 清理”，
     或生成清理路径；这是主要实现难点。

2. **生成测试 main**
   - assert 等价于 test 函数提前 return，test main 可以继续调用
     下一个 test，不再需要 try/catch；
   - 前提是 test 函数能设置失败标志并正常返回；
   - helper 的跨帧 return 仍是难点 1 的一部分；
   - 当前 VM 也可以由 host runner 逐个调用，效果等价。

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
4. 实现 assert 的 test-return 信号 + 沿帧 defer/FREE 清理；
5. mock 自由函数 + 自动 unmock_all；
6. mock 实例/静态/泛型方法；
7. 项目模式 `clux.mod` 遍历；
8. 最后迁移到 C 后端时重写 runner（语义保持不变）。

---

## 已确认决定

- assert 失败等价于当前 test 的提前 return；
- 沿途按正常 return 语义执行 defer / own FREE 清理；
- 失败只终止当前 test，下一个 test 继续；
- 每个 test 结束自动 `unmock_all`；
- test 块无参、无返回值、不允许嵌套；
- 方法 mock 使用独立 API：
  ```clux
  mock_method(Type, "method", replacement);
  unmock_method(Type, "method");
  ```

## 待补充

- assert 的具体形式（`assert(expr)` / `assert_eq` / 消息参数）
- 失败后的进程退出码
- 项目模式下源码遍历规则
