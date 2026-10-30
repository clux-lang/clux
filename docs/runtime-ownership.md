# 运行时所有权（runtime ownership）

> **状态（2026-10）**：§1–§13 记录所有权系统的演进过程；当前指针与内存模型见 **§14**。

> 状态：**当前只保留 C 裸指针**。编译期所有权语法 `own/ref/fatal` 和
> `move/clone` 已移除并直接报错；编译器内部残留的所有权机制（ownership_t、
> 符号/槽位所有权、fatal 变体、销毁函数生成、C 后端 own 释放分支）也已在
> 阶段 3 全部删除（见 §12）。魔法方法、`[[no_copy]]`/`[[auto_dispose]]`、
> `Own/Fatal/allocator` 全部**延后设计**，当前工作树里没有实现。
> 旧实现冻结在 `archive/compile-time-ownership` / tag `archive-ownership-09d31e4`。

## 1. 目标

把 clux 的所有权从**编译器类型系统**降级为**标准库类型 + 少量编译器内建规则**：

- 默认指针是裸指针 `*T`，编译器不做 move/borrow/聚合拥有分析。
- `Own[T, A]` / `Ref[T, A]` / `Fatal[T, A]` 是标准库泛型 struct，不是语法。
- 编译器只保留：
  1. `__init__` / `__assign__` / `__dispose__` 的调用规则；
  2. `[[no_copy]]` / `[[auto_dispose]]` 两个内建装饰器；
  3. `Ref` 的局部语法逃逸检查（不做完整逃逸分析）。
- UAF 策略由 allocator 的泛型参数 `A` 选择。

## 2. 非目标

- 不做全程序借用期分析。
- 不保证裸 `*T` 的安全。
- 不保证 `Ref` 指向栈对象时的运行时 UAF 检查（除非使用 `StackAllocator`）。
- 不在编译器里实现 `Own`/`Ref`/`Fatal`/allocator。

## 3. 阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 | 冻结旧实现、清点迁移面、写本文档 | 完成 |
| 1a | raw-pointer 迁移开关：`own/ref/fatal` 退化为裸类型 | 完成 |
| 1b | examples 迁移为裸指针 | 完成 |
| 1c | tests 迁移/删除旧所有权测试，切换默认 | 完成（raw 已是默认） |
| 2 | `__init__` / `__assign__` / `__dispose__` 魔法方法 | 延后 |
| 3 | `[[no_copy]]` / `[[auto_dispose]]` 装饰器 | 延后 |
| 4 | 标准库实现 `Own`/`Ref`/`Fatal`/allocator | 延后，不在本仓库 |

当前语言契约：

- `*T` / `[]T` 是唯一指针/切片写法；
- 分配/释放由用户通过 FFI（`libc::malloc` / `libc::free`）负责，`make(pointer, len)` 构造切片视图；
- `own` / `ref` / `fatal` / `move` / `clone` 直接报错，不再作为 no-op 接受；
- `share` / `weak` 的内建 RC 也已移除；需要 RC 时由标准库类型实现；
- 不生成 `__init__` / `__assign__` / `__dispose__` 调用，不识别
  `[[no_copy]]` / `[[auto_dispose]]`；
- 不再有 `CLUX_RAW_POINTERS` 开关，也不存在 legacy 所有权路径。

Phase 1 只做机械化迁移，不混入魔法方法。

## 4. `__init__` / `__assign__` / `__dispose__`

对应 C++ 特殊成员：

| C++ | clux |
|---|---|
| `T(const T&)` | `__init__(self: ref *T, other: ref *T)` |
| `T(T&&)` | `__init__(self: ref *T, other: Fatal[T])` |
| `operator=(const T&)` | `__assign__(self: ref *T, other: ref *T)` |
| `operator=(T&&)` | `__assign__(self: ref *T, other: Fatal[T])` |
| `~T()` | `__dispose__(self: ref *T)` |

初始化与赋值必须分开：

```clux
var a = b;   // a.__init__(b)   —— 目标未初始化
a = b;       // a.__assign__(b) —— 目标已初始化
return a;    // 返回槽的 __init__(a)
```

### 4.1 `__assign__` 与旧值

采用 C++ 风格：**编译器不自动 dispose 旧值**，由 `__assign__` 作者手工释放。

```clux
func __assign__(self: ref *S, other: ref *S): void {
    if (self == other) return;          // self-assignment 作者 guard
    self.o.__assign__(other.o.clone()); // 字段赋值内部释放旧值
    self.n = other.n;
}
```

漏写就是泄漏；这是 `operator=` 的契约。

### 4.2 `Own` / `Fatal` 的矩阵

```clux
Own[T].__init__(Fatal[T])      // 消费
Own[T].__init__(ref *Own)      // 不存在 → 拒绝 copy
Own[T].__assign__(Fatal[T])    // 先释放旧值，再消费
Own[T].__assign__(ref *Own)    // 不存在 → 拒绝 copy

Fatal[T].__init__(Fatal[T])    // move
Fatal[T].__assign__(Fatal[T])  // move；旧值有内容 → panic
Fatal[T].__init__(ref *Own)    // 不存在 → 拒绝
Fatal[T].__assign__(ref *Own)  // 不存在 → 拒绝
```

- `var f2 = f1;` → `Fatal.__init__(f1)`，move，`f1` 清空。
- `f2 = f1;` → `Fatal.__assign__(f1)`，move；旧值按 `__dispose__` 规则 panic。
- `var y: Own[T] = x.move();` → `Own.__init__(Fatal)`，合法。
- `var f: Fatal[T] = ownValue;` → 没有 `Fatal.__init__(Own)`，拒绝。

`S` 含 `Own` 时不需要 `move` 方法，也不返回 `Fatal[S]`；只实现
`__init__(ref *S)` / `__assign__(ref *S)`，内部自行决定 move 还是 clone：

```clux
func __init__(self: ref *S, other: ref *S): void {
    self.o.__init__(other.o.clone()); // 或 other.o.move()
    self.n = other.n;
}
```

## 5. 装饰器

### 5.1 `[[no_copy]]`

- 类型属性；在 copy 点检查。
- copy 版 `__init__(ref *T)` / `__assign__(ref *T)` 不存在 → 报错。
- 含 `Own` 的聚合没有 `__init__(ref *T)` → 字段 copy 遇到 `Own` 时失败。
- 不做全局 no-copy 传播标记；只在 copy 点递归检查字段。

### 5.2 `[[auto_dispose]]`

- 子类型/字段的 auto-dispose 传递给父类型。
- 显式 `__dispose__` 先执行函数体，再按字段声明逆序递归 dispose（C++ 顺序）。
- 覆盖正常块退出、`return`、`break`/`continue`、`?`/`try`、panic、条件初始化、
  按值参数。
- 泛型容器（`Vec[Own[T]]`）由容器自己的 `__dispose__` 遍历元素；编译器只负责调用。
- 条件初始化需要 definite-initialization 标记；未初始化不调用 `__dispose__`。

## 6. 分配器与 UAF

```clux
struct Own[T, A: Allocator = DefaultAllocator] { p: *T; a: A; }
struct Ref[T, A: Allocator = DefaultAllocator] { p: *T; a: A; }
struct Fatal[T, A: Allocator = DefaultAllocator] { ... }
```

```clux
interface Allocator {
    func alloc(size: usize, align: usize): *u8;
    func free(ptr: *u8): void;
    func check(ptr: *u8): void;   // 空实现 = 不检查
}
```

`Own.deref` / `Ref.deref` 先 `a.check(p)` 再返回 `p`。

| Allocator | deref 检查 | 成本 | 能抓什么 |
|---|---|---|---|
| `SimpleAllocator` | 无，`*p` | 0 | 不抓 UAF；只靠 Own 的 move/no-copy |
| `CheckedAllocator` | header magic + generation | 1–2 次 load | 堆 UAF、double free、stale Ref |
| `ArenaAllocator` | epoch | 一次检查 | arena reset 后 UAF |
| `MmapAllocator` | page protection | 很高 | 堆 UAF（fault） |
| `RcAllocator` | refcount | 原子操作 | 生命周期，不抓循环泄漏 |

`CheckedAllocator` 布局：

```text
[ header: magic | generation | size | state ][ payload T ]
```

`free` 标记 dead、generation++、quarantine；`check` 校验后返回。

### 6.1 默认策略

暂定：

- debug / 测试默认 `CheckedAllocator`；
- release 默认 `SimpleAllocator`；
- 跨包 API 建议显式写 `Own[T, A]`，避免 profile 改变类型身份。

默认策略由标准库决定，不是编译器语法。

## 7. `Ref` 逃逸

只做局部语法检查：

- 拒绝 `return &local`；
- 拒绝 `return Ref(&temporary)`；
- 不做完整逃逸分析；存入字段/全局/闭包的间接逃逸交给运行时。

运行时 UAF 的边界：

- 堆 `Ref`：`CheckedAllocator` 的 generation 检查。
- 栈 `Ref`：没有 allocator header，`CheckedAllocator` 查不到。第一版建议
  `Ref` 只用于堆对象；栈借用继续用裸 `*T`（unsafe）。
- 如以后需要栈 `Ref` 的运行时检查，再引入 `StackAllocator` + frame token。

## 8. Phase 1 迁移规则

- `own *T` / `ref *T` / `fatal *T` → `*T`
- `own Agg` → `Agg`
- 指针/切片的分配与释放由 FFI/标准库负责；`make(pointer, len)` 构造视图
- `move` / `clone` 不再是编译器原语；由普通函数/库提供
- 方法接收者 `ref *T` → `*T`
- 移除作用域标注、借用期检查、聚合拥有判定、C ABI 所有权契约
- `share` / `weak` 内建 RC 已移除；由标准库实现
- 旧所有权测试在 Phase 1 删除或改写为裸指针清理测试；历史实现保留在冻结分支/tag

## 9. 迁移清点（2026-10）

- examples: 58 个 `.cx`，`own` 743 处、`ref` 483 处、`fatal` 92 处（词边界统计）
- tests: 76 个 `.cpp`，ownership 相关测试 20+ 个
- 重点测试：`borrow_move_test` / `borrow_sources_test` / `deep_ownership_test` /
  `own_aggregate_test` / `own_slot_copy_test` / `ref_slot_carrier_test` /
  `type_ownership_test` / `share_weak_test` / `explicit_destroy_test` /
  `tag_union_own_test` / `union_member_move_test` / `closure_value_ownership_test`

Phase 1 先迁移 examples，再逐组改测试；每组保持全量测试绿。

## 10. Phase 1 完成

- 裸指针是**唯一模式**；没有环境变量开关，也不存在 legacy 所有权路径。
- 删除 13 个纯所有权测试文件、共 67 个用例；迁移/删除 Driver、CBackend、
  Interface、Generic、Fmt、ShareWeak 中的旧语义断言。
- 全量：**1631/1631**（raw-only，share/weak 已移除）。
- 所有可运行 examples 在 raw 模式下通过。
- `own` / `ref` / `fatal` 关键字保留为**已移除语法**：解析到即报错并提示改成
  `*T` / `[]T`。

Phase 2 开始实现 `__init__` / `__assign__` / `__dispose__`。

## 11. 延后项

以下内容保留为设计讨论，不在当前实现里：

- `__init__` / `__assign__` / `__dispose__` 的调用 lowering；
- `[[no_copy]]` / `[[auto_dispose]]`；
- `Own[T]` / `Fatal[T]` / allocator；
- 字段/容器 drop glue、跨函数 move、`Ref` 逃逸检查。

重新实现前应先决定语义放在 sema/IR 还是后端；当前不应在 C 后端继续堆叠。

## 12. 阶段 3：编译器内所有权死代码清理（2026-10）

阶段 1 只切断了语法；阶段 3 把编译器内部仍然残留的"所有权基础设施"全部删除，
使编译器只剩裸指针语义：

- 删除 `vm/ownership.h` / `vm/type_ownership.h`：`ownership_t`、
  `type_ownership_of/apply/strip` 等恒等空壳；
- 槽位层：`struct_field_t` / slice / tuple / union / option / array 不再有
  ownership 字段，`type_has_own_slots`、`type_owns_resource`、
  `type_name_with_ownership` 一并删除；
- 符号/值层：`sema_symbol_t::ownership`（派生入口 `sema_sym_ownership` /
  `sema_sym_is_fatal`）、`value_type_ownership` / `value_set_type_ownership`、
  `sema_value_is_fatal` / `sema_value_holds_own`、fatal 变体
  （`sema_fatal_variant_of`）与 `value_take_fatal_into` 全部移除；
- sema 分支：变量定义/赋值/聚合构造/调用/闭包捕获/`try` 中的
  own/fatal 接收规则、`sema_check_slot_flow`、fatal 参数消费检查、ref 返回
  标注检查、销毁函数生成（`__destroy_<T>` / env vtable fid）删除；
- C 后端：`c_lower_stmt` 的 own 释放条件、闭包/defer 捕获槽释放、
  `__destroy_<T>` 合成、`capture_clone_kind` 深拷贝判定删除；`FREE` 仍负责
  闭包 env / 显式 free；
- 解析层：AST 指针/切片节点的 `ownership` token 字段与 `AST_OWN_*` 编码、
  未再生成的 `AST_MOVE` 节点删除；`extends` pattern 里的
  `own/ref/fatal/share/weak` 前缀改为与类型语法一致的"已移除"诊断。

## 13. 阶段 3b：内部生成器与死节点清理（2026-10）

阶段 3 之后，编译器里只剩"没有任何产生路径"的生成器与直接形态节点，本阶段
一并删除：

- **生成的 clone 函数**整条子系统（`__clone_*` / `__cloneptr_*` /
  `__cloneslice_*`、`sema_clone_fn_t` 注册表、`sema_gen_clone*` /
  `sema_find_clone_fn` / `sema_attach_clone_fns`）：`clone()` 已从语言移除，
  仅保留运行期 `__deep_clone` 用于闭包值拷贝；
- **死直接形态 AST 节点**：`AST_CLONE`、`AST_TAKE_SLOT`、`AST_IS_TAG`、
  `AST_OPT_IS_SOME`、`AST_FOR_INDEX` 及其 C 后端 / CIR lowering、walker 与
  名字表；用户 `is` 与 `nil` 比较继续走既有的 `AST_BINARY` lowering；
- `stmt_destroy.c` 收缩为"闭包 env 的 FREE 生成"（`gen_destroy` / 销毁函数
  注册表 / `gen_destroy_slot` 等全部删除）；
- 槽位级所有权谓词的最后残留（`type_contains_ptr_or_slice`、comptime 的
  `type_contains_own` 全局 own 检查）删除；
- VM 侧未使用 API（`scope_untrack`、test-mode cleanup names、
  `value_alloc_data_own` / `value_own_copy_into` 包装）删除，`ptr_free_owned_*`
  等所有权命名改为中性名（`ptr_free_recursive` / `ptr_clear_recursive`）。

注意：**`defer` 是正常语法，不在清理范围内**——解析、`desugar_defers` 展开、
CTFE 执行、C 后端 lowering、fmt 与 VM 的 defer 注册/异常展开调用全部保留，
defer 相关用例（含 `Foreach`/`Desugar`/`Fmt`）保持通过。

回归（阶段 3b 当时）：全量 1631 个用例，2 个 `Install.*` 环境相关失败为既有问题。

## 14. 指针与内存（当前模型）

指针生命周期完全照搬 C：编译器不插入任何分配/释放，也不预设销毁逻辑。

- 分配：通过 FFI 调用 `libc::malloc`，`opaque` → `as *T`，字段逐个初始化；
- 释放：`libc::free(p as opaque)`；切片先 `data(s)` 取指针再释放；
- `make(pointer: *T, len: usize): []T` 只构造切片视图，不分配内存；
- 闭包 env 是内联值（无独立堆块），捕获的裸指针按“借用/所有权转移”约定
  由用户负责（借用由来源释放，转移由接收方释放）；
- `defer` 只负责调用时机，不附带任何自动释放。

回归：全量 **1620** 个用例，**1618 通过**；仅两个 `Install.*` 环境相关失败为
既有问题。示例与测试使用 `libc::malloc/free` + 数组/切片视图。
