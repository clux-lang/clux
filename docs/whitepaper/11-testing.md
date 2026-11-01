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

## 11.5 mock / unmock

```clux
func compute(batch: []i32): i32 { return batch[0] * 2; }

test "Order.Total" {
  mock("compute", func(batch: []i32): i32 { return 42; });
  var b: [1]i32 = .[1]i32{ 7 };
  assert(compute(b[:]) == 42);
  unmock("compute");
  assert(compute(b[:]) == 14);
}
```

| API | 说明 |
| --- | --- |
| `mock(name: str, replacement: F)` | 把自由函数的实现替换为 `replacement` |
| `unmock(name: str)` | 恢复原实现 |
| `mock_method(T, name: str, replacement: F)` | 类型级替换 struct 方法（实例方法与静态方法走同一张方法表） |
| `unmock_method(T, name: str)` | 恢复方法 |

- 只在 `clux test` 构建里可用；普通 `build` / `run` 里 `mock` 等是未定义符号。
- `name` 必须是**字符串字面量**：目标在编译期解析，因此笔误是编译错误。
  `"module::fn"` 指向被导入模块的导出函数。
- `replacement` 必须是**同签名**的函数值（可以带捕获）；方法 replacement 的第一个
  形参是 `self`（`*T`），与目标方法一致。
- `T` 是类型（支持泛型实例 `Box[i32]`）。mock 是**类型级**的：该类型的所有实例
  都受影响；没有 per-instance mock。
- 实现在被调函数入口查一个函数槽，因此 mock 对**已经取出的函数别名**同样生效；
  但 `extern cfunc`（C/FFI 函数）不在 clux 函数体系里，不能 mock；泛型函数不能
  直接 mock（先按具体实例类型 mock 方法，或 mock 非泛型包装函数）。
- 每个 test 用例开始前，测试运行器会清空所有 mock 槽：上一个用例里忘记
  `unmock` 不会影响下一个用例（用例内仍可显式 `unmock` 提前恢复）。
- mock **不嵌套**：对同一目标 mock 两次时后一次覆盖前一次，一次 `unmock` 即恢复
  原实现（`unmock` 未 mock 的目标是空操作）。
