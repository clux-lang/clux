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

有 `clux.mod` 时，遍历包内 `.cx` 源文件并执行所有 test，跳过 `deps/`。

## 11.3 assert

```clux
assert(x == 4);
```

- assert 只允许在测试上下文使用。
- 失败时终止当前 test，继续执行下一个 test。
- 失败信息包含表达式源码。
- 失败按“当前 test 提前返回”处理：执行 defer 和 own 清理。

## 11.4 mock 自由函数

```clux
mock("target", target, func(x: i32): i32 { return 7; });
unmock("target");
```

- `mock` 替换全局函数绑定。
- replacement 的签名必须与目标兼容。
- `unmock` 恢复原函数。
- 每个 test 结束自动执行 `unmock_all`。

## 11.5 mock 方法

```clux
mock_method(Type, "method", replacement);
unmock_method(Type, "method");
```

- 方法 mock 是类型级的：该类型的所有实例都使用替换实现。
- replacement 的第一个参数是接收者。
- 泛型实例使用 `typeof(instance)` 作为类型值：

```clux
mock_method(typeof(box), "get", func(self: ref *Box[i32]): i32 {
    return 8;
});
```

## 11.6 测试输出

结果按 suite/name 输出，并给出总数、通过数、失败数；有失败时退出码非零。

## 11.7 测试上下文

`assert`、`mock` / `unmock`、`mock_method` / `unmock_method` 只在 test
运行时可用；从 test 调用的辅助函数共享同一测试上下文。
