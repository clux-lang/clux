# 运行期可变参数：编译期展开设计

> 状态：**已实现（2026-10-22）**。本文替代早期“tuple 形参 / 运行期描述符”
> 草案。参数包不落运行期：`args` 不是变量，而是一组在实例化入口就能
> 完全展开的 AST 变换。

---

## 1. 结论

`args: ...Args` **不生成运行期 tuple 局部变量**，也不改变函数调用 ABI。
实例化时 `Args` 已完全确定，因此：

| 源写法 | 实例化后的 AST |
|---|---|
| `args[i]`（i 为编译期常量） | `__pack_arg_i` |
| `len(args)` | 整数字面量 `Args` 元素个数 |
| `...args`（调用/构造展开） | `__pack_arg_0, ..., __pack_arg_N-1` |
| 裸 `args` | **诊断**：没有运行期值；需要 tuple 时写 `.{ ...args }` |
| `.{ ...args }` | `.<T0,...,TN-1>{ __pack_arg_0, ... }` |

实例函数仍然是普通定长函数：

```clux
// 源
func print[...Args](fmt: str, args: ...Args): void { ... }

// print[i32, i32] 实例
func print$i32$i32(fmt: str, __pack_arg_0: i32, __pack_arg_1: i32): void
```

没有额外参数、没有 tuple 描述符、没有新 opcode。`args` 只在 AST 中存在，
compiler/VM 完全看不到它。

**不同参数数量就是不同实例**，这是单态化的正常结果，不是 ABI 问题。

---

## 2. 实例化入口的纯 AST 变换

`materialize_pack_params` 在重新解析定义后：

1. 删除 `args: ...Args` 形参；
2. 为每个包元素合成 `__pack_arg_i: Ti` 形参（类型来自实例实参表）；
   - 推断参数包时，fatal 实参先归一化为 `own Ti`（形参不能是 fatal）；
     传 own（未 move）按所有权规则拒绝；
   - 若 `Ti` 是**含 own 槽位但未标 own** 的聚合（如 `Outer` 里套
     `own *i32`），合成形参自动写成 `own Ti`，否则 `move(__pack_arg_i)`
     会被当作 move 借用而报错；
3. 记录 `pack_param_name = "args"`；
4. **不再**插入 `var args: <...> = ...` 聚合语句。

随后 sema 在函数体 walk 时按 `Args` 做纯 AST 替换（等价于入口一次性
变换；由于 `Args` 在实例化开始时已知，所有替换都不依赖运行期信息）：

```text
ARGS[INDEX]    -> __pack_arg_INDEX       (INDEX 必须编译期常量)
LEN(ARGS)      -> INT_LIT(pack_count)
SPREAD(ARGS)   -> __pack_arg_0, ..., __pack_arg_N-1
BARE ARGS      -> error
```

之所以可以在 sema 表达式访问点做，而不是先写一个完整 AST walker：
sema 本来就会访问每个表达式；替换后的节点直接成为实例 AST 的一部分，
compiler 只看到普通形参/普通构造。若后续需要把变换提前到独立 pass，
替换逻辑是同一组 `sema_generic_pack_*` 函数，可直接复用。

---

## 3. 各类用法的语义

### 3.1 `args[i]`

```clux
func second[...Args](args: ...Args): T1 { return args[1]; }
```

- 索引必须是编译期整数（字面量或 CTFE 可求值表达式）；
- 变换为 `__pack_arg_1`；
- 运行期没有 tuple、没有下标、没有越界检查；
- 越界在编译期诊断；
- 若元素是 `own T`，直接读取得到借用视图；要取出所有权写
  `move(args[1])`，变换为 `move(__pack_arg_1)`（合法，因为移动的是
  整个局部形参，不是聚合字段）。

### 3.2 `len(args)`

```clux
func count[...Args](args: ...Args): u64 { return len(args); }
```

- 编译期折叠为 `pack_count` 字面量；
- 类型 `u64`；
- 没有运行期 `len` 调用。

### 3.3 `...args`

```clux
func forward[...Args](args: ...Args): i32 {
    return first(...args);
}
```

- 展开为 `__pack_arg_0, ...`；
- 对 `own` / `fatal` / 含 own 槽位的元素**自动包 `move(...)`**：
  `first(move(__pack_arg_0), ...)`；
- 因此 `...args` 是所有权转移语义：转发后原隐藏形参被消费；
- 若目标泛型函数只有单一类型参数包，展开时同时把 `Args` 元素作为
  显式类型实参，避免 `move` 的 fatal 值状态污染类型推导；
- 目标是非泛型定长函数时，按普通实参展开并走正常参数校验；
- 空包展开为 0 个实参。

### 3.4 裸 `args`

```clux
return args;          // 错误：args 没有运行期值
var t = args;         // 错误
```

诊断：

```text
variadic parameter 'args' has no runtime value; use 'args[i]', 'len(args)',
'...args', or explicitly construct a tuple with '.{ ...args }'
```

### 3.5 显式 tuple

需要把包当 tuple 值使用时，手工构造：

```clux
var t = .{ ...args };              // 推断为 <...Args>
var t2: <...Args> = .{ ...args };  // 显式类型
return .{ ...args };
```

- 只有在用户显式构造时才产生 tuple 值/布局；
- 构造字段来自隐藏形参，own 元素自动 `move`；
- tuple 类型仍是普通静态 tuple，`.cxb` 正常序列化；
- 没有为每个变参函数隐式创建 tuple。

---

## 4. 所有权与 move

参数包的隐藏形参是普通函数形参：

- 类型为实例实参 `Ti`；
- `own Ti` 形参由函数负责释放；
- `fatal Ti` 形参必须在函数退出前消费；
- `ref` 无所有权。

`...args` 和 `.{ ...args }` 是**消费式展开**：

- 元素需要所有权转移（own/fatal/含 own 槽位）时自动插入 `move`；
- 因此不会出现“不能 copy 局部字段”的问题；
- 展开后隐藏形参的值状态为已 move，后续再使用由常规 TDZ/移动检查诊断；
- `args[i]` 本身不自动 move：读的是借用；需要转移时显式 `move(args[i])`。

这解决了早期“临时 tuple 变量 + own 字段无法 move 局部”的死路：现在
聚合字段只在用户明确写 `.{ ...args }` 时出现，且构造时直接 move 隐藏形参，
不需要对 tuple 字段做 per-field TDZ。

**函数返回含 own 槽位的聚合**：返回类型**必须显式标 `fatal`**
（如 `fatal <...Args>`）；裸 `<...Args>` 按值返回会直接诊断，因为调用方
无法安全接管。构造出来的聚合本身是 `own <...Args>`，因此 `return` 时
还必须写成 `return move(.{ ...args });`（或 `clone(...)`），不能直接
`return .{ ...args };`。调用结果是将亡值（FATAL）。VM 不再对返回 tuple
做 `value_clone`（clone 会让 callee 的返回临时无人释放），而是
`value_move` 并标 FATAL；sema 的 call shadow 同样标 FATAL。于是
`var t = pack[own *i32](move(x));` 自动推断为 own 绑定并生成销毁序列，
`.cxb` 往返也不泄漏。

这些检查与普通函数走同一条所有权路线：`resolve_instance_sig` 调用
`sema_check_ret_ref_annot`，实例 body 的 `walk_return` 也执行构造返回
检查；嵌套的 `own`（struct 字段、tuple 元素、array/option 内部）都由
`type_has_own_slots` 递归覆盖。

---

## 5. ABI / 字节码 / 容器

- **ABI 不变**：实例是普通定长函数；不同的包长度是不同 mangling 实例。
- **无新 opcode**：`args[i]`/`len`/`...args` 在 sema 完成；compiler 只看到
  普通标识符/literal/实参链。
- **容器不变**：`.cxb/.cxs` 不需要 tuple 变参描述符；隐藏形参名
  `__pack_arg_i` 只是普通函数参数名。
- **FFI 不变**：`extern cfunc ...` 的 C ABI variadic 仍走 libffi；不与
  参数包混淆。

---

## 6. 边界

- `args[i]` 的 `i` 必须编译期常量；不能用运行期循环变量。需要遍历时，
  目前只能展开/手写固定下标（运行期异构遍历不在本设计内）。
- 裸 `args` 不再可用；原来的 `return args` / `var t = args` 需要改为
  `.{ ...args }`。这是有意的 API 收紧：参数包不是运行期 tuple。
- 不支持动态 arity / 异构运行期 tuple / `any`。
- `...args` 转发会消费隐藏形参；多次转发同一次包需要显式重新构造，不能
  依赖隐式 copy。
- C variadic 与 clux 参数包不能互相展开。

---

## 7. 实现落点

- `src/sema/sema_generic.c`：
  - `materialize_pack_params`：删除 tuple 聚合局部，只合成隐藏形参；
  - `sema_generic_pack_is_param`
  - `sema_generic_pack_arg_expr`（可选 move）
  - `sema_generic_pack_tuple_type_expr` / `sema_generic_pack_tuple_expr`
- `src/sema/expr_access.c`：`args[i]` 折叠为 `__pack_arg_i`
- `src/sema/expr_call.c`：
  - `len(args)` 折叠为 literal
  - `...args` 展开 + 自动 move
  - 简单单包泛型转发补显式类型实参
- `src/sema/expr_construct.c`：`.{ ...args }` 展开为显式 tuple 构造
- `src/sema/expr_ops.c`：裸 `args` 诊断

### 测试

- `GenericPack.TupleTypeExpansion`：`return .{ ...args }` / `var copy = .{ ...args }`
- `GenericPack.OwnElementMoveAndForward`：`move(args[0])` + `...args` 转发 own
- `GenericReject.BareParameterPackName`：裸 `args` 诊断
- 既有 `GenericPack.*`、`GenericValueParam.*` 全量回归

---

## 8. 非目标

- 运行期动态 tuple 描述符；
- `any` / 动态类型；
- C `va_list` 语义；
- 运行期循环遍历异构包。
