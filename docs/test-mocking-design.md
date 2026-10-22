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

## 4. struct 方法 mock：架构上同样可行，而且更重要

### 4.1 方法调用怎么走

```clux
p.move(1, 2)
```

compiler 发：

```text
PUSH "p"
PUSH 1
PUSH 2
MEMBER_CALL "move" 2
```

`op_member_call`：

1. 取 receiver 的类型；
2. 沿嵌入链 `find_attr_owner` 找到拥有该方法的 struct 类型；
3. 从 `owner->attributes`（`name → value_t*`）取方法 func value；
4. 实例方法把 receiver 变成 `self` 参数，再 `value_call`。

因此 **方法表就是运行期可变映射**。替换 `owner->attributes["move"]`
的函数值，后续所有 `MEMBER_CALL "move"` 都会命中 mock。

`sema` 禁止用户写：

```clux
S::f = func(...) {...};
```

报：

```text
cannot assign to 'S::f': it is a `func` member (a method, not a variable)
```

但 VM 内建可以直接改 `attributes` / member scope，类似内部
`STORE_ATTR`。

### 4.2 类型级 mock，不是实例级

当前结构体没有 per-instance vtable。因此：

- **类型级 mock**：`S` 的所有实例 `s.f()` 都替换成 mock；
- **实例级 mock**：只让 `s1.f()` 变化、`s2.f()` 不变，需要额外
  per-instance 分派表，当前没有。

测试里通常 mock 一个类型的方法（类似 `patch.object(S, "f")`），
类型级已经覆盖绝大多数需求。

如果确实要 per-instance：

- 在 VM 增加 `(receiver data pointer, method name) → replacement`
  的 side table；
- `op_member_call` 在查类型表前先查 side table；
- 只对稳定地址的 receiver（指针/变量）可靠；按值 receiver 的临时
  副本地址不稳定，不能保证命中；
- 这会改变每次方法调用的热路径，建议作为可选测试模式。

### 4.3 推荐 API

```clux
mock_method(S, "move", func(self: ref *S, dx: i32, dy: i32): void {
    ...
});
unmock_method(S, "move");
```

- 第一个参数是 **type value**，不是字符串：泛型实例
  `Box[i32]` 与 `Box[str]` 的方法表不同；
- 第二个参数方法名；
- 第三个参数 replacement，签名必须与原方法一致（含 self）；
- `unmock_method` 恢复原 func value。

静态方法同样适用：

```clux
mock_method(S, "origin", func(): S { ... });
```

`op_member_call` 根据 receiver 是否为 type value 决定 static/instance，
mock 只换 func value，不改分派规则。

### 4.4 继承/嵌入

`find_attr_owner` 先查自身 attributes，再沿 embeds 查：

- mock 基类：所有嵌入它的子类型都受影响；
- mock 子类型：在子类型 attributes 里插入覆盖项，只影响该子类型；
- `unmock` 要按“实际安装覆盖的 owner 类型”恢复，不能只按调用点类型。

### 4.5 生命周期

方法 func value 归 struct 的 `member_scope` 拥有，`attributes` 只借用。
mock 实现应：

1. `value_clone` 保存原方法值；
2. 在 `member_scope` 里 `scope_set` replacement（或直接
   `value_assign` 原 slot）；
3. `unmock` 时把 clone 写回；
4. 释放 clone；
5. mock 栈按 `(type*, method_name)` 键，支持嵌套。

不要只改 `strmap` 指针而让 replacement 无主，否则 struct 销毁/
unmock 后悬垂。

### 4.6 方法 mock 的限制

- `a.f` 先取成函数值再调用，已持有原值，mock 不影响；
- 方法表是全局类型级，不能只 mock 一个实例（见 4.2）；
- 泛型方法要按具体实例类型 mock；
- decorator 包装后的方法：mock 替换的是最终 attributes 里的函数值，
  会绕过 wrapper；
- C/FFI 静态函数不是 clux 方法，不适用。

## 5. 语法细节

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

## 6. 能 mock 什么

### 可以

- 普通自由函数：
  ```clux
  func target(x: i32): i32 { ... }
  ```
- 递归自由函数；
- 跨模块自由函数（字符串用实际注册名）；
- **struct 实例方法**（类型级，见第 4 节）：
  ```clux
  mock_method(S, "move", func(self: ref *S, x: i32, y: i32): void { ... });
  ```
- **struct 静态方法**；
- **泛型 struct 的具体实例方法**：`Box[i32]` / `Box[str]` 分别 mock；
- 捕获闭包作为 replacement。

### 不可以/需要额外机制

- **单个实例的方法**：当前没有 per-instance vtable，只能类型级；
- **C/FFI 函数**：`extern cfunc` 不经过 clux 函数绑定；
- **mock 前已取出的别名**：
  ```clux
  var f = target;
  var g = s.f;   // 已持有原方法值
  mock(...);
  f(); g();      // 仍调用原值
  ```
- **局部变量遮蔽**；
- **编译期内联**：当前无内联，未来若加要能关闭。

## 7. 推荐 API

```clux
mock(name: str, target: F, replacement: F): void;
unmock(name: str): void;

mock_method(T: type, name: str, replacement: F): void;
unmock_method(T: type, name: str): void;
```

`target` 提供自由函数签名 witness；方法用 type value 作为 key，
因为方法表属于类型，且泛型实例各有自己的表。

可选扩展：

```clux
mock_once(...);
mock_count("name");
unmock_all();
```

## 8. 落地顺序

1. 加 VM 内建自由函数 `mock`/`unmock` 与 mock 栈；
2. 加方法 mock：
   - 解析 `type value + 方法名` 找到 owner；
   - 保存原方法值 clone；
   - 在 member_scope / attributes 安装 replacement；
   - `unmock_method` 恢复；
3. sema 注册 variadic 内建 + 参数检查；
4. 测试自由函数、递归、跨模块、嵌套 unmock；
5. 测试实例方法、静态方法、泛型实例方法、继承覆盖；
6. 再考虑测试库 API；per-instance mock 作为独立可选特性。

## 9. 结论

**可以实现**，自由函数和方法都有运行期可替换的绑定：

- 自由函数：全局作用域槽位；
- struct 方法：`struct_type->attributes` 方法表；
- 调用点每次查表，替换立即生效。

限制是 **实例级 mock** 需要额外的 side table；类型级方法 mock
已经能覆盖主要测试场景，建议先做类型级。
