# clux 测试 mock 语法调研

> 目标：
>
> ```clux
> mock("mock_fn", fn, func(x: i32): i32 { return 7; });
> unmock("mock_fn");
> ```
>
> 状态：可行性调研（2026-10-23）。未实现。

---

## 1. 关键现状：普通函数调用按名字查绑定

当前 compiler 对普通函数调用发的是：

```text
PUSH "target"
PUSH <arg>
CALL 1
```

不是编译期写死的 `CALL <fid>`。反汇编示例：

```text
PUSH "half"
PUSH "x"
CALL 1
```

模块初始化时函数被存进作用域：

```text
LOAD_FUNCTION 64
PUSH_UNDEFINED
DEFINE "half"
```

因此**运行期替换 `"half"` 这个作用域槽位的函数值，后续
`target(...)` 调用就会走到替换值**——这是 mock 可行的基础。

## 2. 用户代码不能直接改函数绑定

普通赋值会被 sema 拒绝：

```clux
target = func(x: i32): i32 { return 7; };
```

```text
error: undefined variable 'target' in assignment
```

函数名是符号表里的函数，不作为普通变量暴露给赋值检查。
所以 mock 不能写成普通标准库函数，必须是：

- VM 内建函数（C 实现）；或
- compiler 识别的内建 intrinsic。

推荐 VM 内建：它可以直接访问作用域槽位。

## 3. 可行实现：VM 内建 `mock` / `unmock`

### 3.1 注册

```c
// mock(name: str, target: func, replacement: func) -> void
// unmock(name: str) -> void
```

sema 侧按 `len` / `data` / `take_opt` 的模式注册为 variadic 内建，
调用点只做基本参数检查；实际函数签名比较在运行期完成。

### 3.2 mock

1. 从 `args[0]` 取名字字符串（str data 是 `const char *`）；
2. 在 `vm->current_scope` / `vm->global_scope` 查找该名字的 slot；
3. 校验 slot 当前值类型是函数类型；
4. 校验 `target` 和 `replacement` 的 `func_type` 签名一致
   （参数数量/类型/返回类型）；
5. 保存原值的 clone 到 mock 栈；
6. `value_assign(vm, slot, replacement)`。

### 3.3 unmock

1. 按名字找 mock 栈顶；
2. 把原值 `value_assign` 回 slot；
3. 释放保存的原值；
4. 支持嵌套：`unmock` 恢复最后一次 mock。

### 3.4 测试隔离

- mock 栈按 VM 实例持有（或至少按测试进程清理）；
- 测试结束时 `unmock`；失败路径应在测试框架层统一 `unmock_all`；
- 不建议跨测试保留 mock。

## 4. 语法细节

用户示例：

```clux
mock("mock_fn", fn, func():{...});
```

当前匿名函数字面量需要返回类型：

```clux
func(x: i32): i32 { return 7; }
```

无返回值写：

```clux
func(): void { ... }
```

捕获测试局部变量用闭包环境：

```clux
var calls: i32 = 0;
mock("target", target, func(x: i32): i32 {
    calls = calls + 1;
    return 7;
});
```

`fn` 是原函数值，主要作为签名 witness；真正替换按字符串找到的
全局槽位。

## 5. 能 mock 什么

### 可以

- 普通自由函数：
  ```clux
  func target(x: i32): i32 { ... }
  ```
- 递归调用：递归体同样 `PUSH "target"`，会命中替换值；
- 跨模块自由函数：只要字符串用实际注册的限定名，槽位是同一个；
- 捕获闭包作为 replacement。

### 不可以/需要额外机制

- **方法调用**：`a.method()` 编译成 `MEMBER_CALL "method"`，
  通过 struct 类型的方法表/绑定查找，不是全局函数槽位；
- **静态方法**：`T::f()` 走类型成员，不是全局名；
- **C/FFI 函数**：`extern cfunc` 不经过 clux 函数绑定；
- **mock 前已取出的别名**：
  ```clux
  var f = target;   // 这里已经拿到原值
  mock(...);
  f();              // 仍调用原值
  ```
- **局部变量遮蔽**：测试函数内同名局部变量会遮蔽全局绑定；
- **编译期折叠/内联**：当前调用没有内联，但未来若加内联，
  mock 必须关闭对应优化。

## 6. 推荐 API

```clux
mock(name: str, target: F, replacement: F): void;
unmock(name: str): void;
```

为什么不只传名字：`target` 提供函数签名 witness，能在运行期拒绝
签名不兼容的 replacement。

可选扩展：

```clux
mock_once(...);       // 只生效一次
mock_count("name");   // 查询调用次数
unmock_all();
```

这些可以在 VM 内建层再包一层，不影响主语法。

## 7. 落地顺序

1. 加 VM 内建 `mock`/`unmock` 与 mock 栈；
2. sema 注册 variadic 符号 + 调用点参数检查；
3. 测试自由函数、递归、跨模块、嵌套 unmock；
4. 明确不支持方法 mock，后续若要支持，需要单独给方法表加
   “可替换绑定”机制；
5. 再考虑包装成测试库 API。

## 8. 结论

**可以实现**，而且不需要改调用约定：

- 当前普通函数调用按全局名字查绑定；
- `mock` 用 VM 内建直接替换该绑定，`unmock` 恢复；
- 主要限制是方法/静态方法/FFI 和已取出的别名，不是自由函数。

建议先只做自由函数 mock，方法 mock 另案设计。
