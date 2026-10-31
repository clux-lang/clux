# 11. 测试

## 11.1 test 块

```clux
test "Suite.Name" {
    assert(1 + 1 == 2);
}
```

语法：

```text
test "<suite>.<name>" { <block> }
```

- test 块只能出现在顶层。
- 同一个文件可以有多个 test 块。
- test 块不嵌套。
- 普通 `build` / `run` 不执行 test 块。
- `clux test` 执行 test 块。

## 11.2 运行方式

单文件：

```sh
clux test path/to/file.cx
```

项目：

```sh
clux test
```

有 `clux.mod` 时从清单解析入口模块，编译入口及其 import 依赖中的 test 块。
测试只使用 C 后端（`--backend=c` 仅用于兼容旧脚本）。

## 11.3 assert

```clux
assert(x == 4);
```

- `assert` 只在测试模式可见；普通构建中使用会得到未定义符号诊断。
- 失败时终止当前 test，继续执行下一个 test。
- 失败信息包含表达式源码。
- 失败按“当前 test 提前返回”处理：继续执行 defer。

## 11.4 测试输出

结果按 suite/name 输出，并给出总数、通过数、失败数；有失败时退出码非零。
