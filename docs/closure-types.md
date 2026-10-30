# 闭包类型设计（closure types）

> **状态（2026-10）**：本文档描述闭包内部实现的历史形态。其中的 own/fatal 捕获与自动清理已随编译期所有权移除；闭包 env 仍是内联值，指针生命周期由用户负责。

状态：设计定稿，**阶段 A–D 已完成验收**（C 静态 clone/内联 env、VM 内联实例 env +
值语义拷贝 + 可写捕获、tests/examples 迁移、旧表示清理、文档/ABI 收尾）。
残余为不阻塞验收的优化项（见文末"后续可选工作"）。

## 1. 核心决策

闭包**不擦除**：捕获列表是类型的一部分。

```clux
func |a: i32, b: own *i32| (i32, i32) -> i32
```

- `func(...) -> R`：**无捕获**函数类型（旧的普通函数值）。
- `func |a: T, b: U| (params) -> R`：闭包类型，捕获槽位类型（按声明序）进入
  类型身份；捕获**名**不参与类型相等（布局/可赋值性只看类型序列）。
- 需要"可调用对象"作为参数时用泛型，而不是擦除：

```clux
func apply[F](f: F, x: i32): i32 { return f(x); }

var base: i32 = 3;
var f: func |base: i32| (i32) -> i32 =
    func |base| (x: i32): i32 { return base + x; };

if (apply[func |base: i32| (i32) -> i32](f, 4) != 7) { return 1; }
```

泛型单态化后 `F` 是具体闭包类型，`f(x)` 按该签名直接检查/调用；没有
`void*` 擦除，也没有函数指针 + 类型转换。

## 2. 值表示：捕获内联 + clone/dispose

闭包值是一个聚合结构，捕获块**内联**在值里（不再单独 `malloc` env）：

```c
/* C 侧（每个闭包类型一个 struct；<id> 为类型 id） */
struct clux_<tag>_cl_<id> {
    <ret> (*fn)(void *env, <params...>);
    struct {                       /* 内联捕获块，声明序 */
        <cap0_type> clux_f_cap0;
        <cap1_type> clux_f_cap1;
        ...
    } env;
};
```

- 调用：`f(args)` → `f.fn(&f.env, args)`（实现函数首参仍是 `void *env`，
  便于与普通函数共用 ABI）。
- 函数体内捕获访问：`((struct env *)env)->clux_f_cap`（**指针**语义，写回
  闭包实例）。
- `env_clone` / `env_dispose` **不作为函数指针存进闭包值**：它们是按闭包
  类型**静态生成**的普通函数（每个 env 布局一个），调用点已知具体闭包类型，
  直接调用它们（`clux_env_clone_<id>(&src.env)` / `clux_env_dispose_<id>(
  &value.env)`），零额外指针、零间接开销。
- 捕获里可能有 `own` / `fatal` / 含 own 的聚合：拷贝闭包必须深拷贝、销毁
  必须释放。捕获块内联只是把"env 的存储"放进闭包值，生命周期规则不变。
- `move`：按位移动，源失效（不再 dispose）；`clone`：`fn` 不变，
  `env = env_clone(&src.env)`；`dispose`：`env_dispose(&value.env)`。
- 无捕获函数值：`struct { fn; }`（`env` 为空，调用传 `NULL`）。

## 3. 所有权规则

- 闭包类型是否"含 own"由捕获槽位递归决定（`type_has_own_slots` 扩展覆盖
  捕获列表）：
  - 含 own → 闭包值 move-only；`clone` 仅在 `clone` 语境/显式克隆时可用；
  - 不含 own → 可自由 copy（但 `env_clone` 仍生成，统一代码路径）。
- `FREE closure` → `env_dispose(&value.env)`（不再有 `clux_func_release`
  的单独 env 指针）。
- 不允许把带捕获闭包赋给 `func(...)`（擦除）。过渡期可保留"加宽"并告警，
  最终删除；迁移方式：泛型参数或显式闭包类型注解。

## 4. 各层改动清单

1. **vm/type_func**：`func_type_t` 增加 `capture_types[]`；`type_func_closure`
   按 (签名, 捕获类型序列) intern；类型名 `func |T, U| (...)`；访问器
   `func_type_is_closure / func_type_capture_count / func_type_capture_type`。
2. **parser/AST**：`ast_func_type_t` 增加捕获列表（`AST_VAR_DEF` 链，每项
   `name: Type`，类型上下文禁止初始值）；`func |a: T| (...) -> R`。
3. **sema**：
   - 闭包字面量/局部函数的类型 = 捕获槽位类型构成的闭包类型（不再返回普通
     `func(...)`）；`capture_type` 结构体的字段顺序即声明序；
   - 泛型 mangle 纳入捕获类型，避免 `apply[闭包A]` / `apply[闭包B]` 合并；
   - 兼容性：闭包类型 ≠ 普通 func；`apply[F]` 调用检查在实例化后自然成立。
4. **compiler**：新增 `FUNC_TYPE_CAPTURE` 字节码（hoist 时在参数/返回后追加
   捕获类型）；`MAKE_FUNCTION` 把捕获内联进新值表示；`SET_CLOSURE` 改写为
   对值内 env 字段赋值（或由新的 CLOSURE 构造序列完成）。
5. **VM 值表示**：闭包类型的 value data = `{ fnptr; caps... }`（类型 size
   不再是 `sizeof(func_t*)`，而是按捕获布局计算）；`func_vcall` 按类型分派，
   从内联块取 env 指针；`func_clone/dispose/assign` 调 `env_clone/env_dispose`
   （函数指针由编译期生成，同 C 后端）。
6. **C 后端**：
   - 闭包类型 → 上述 C struct（字段顺序固定）；调用 `f.fn(&f.env, ...)`；
   - 闭包字面量构造：`{ .fn = impl, .env = { .clux_f_a = a, ... } }`；
     clone/dispose 由使用点按类型**静态调用** `clux_env_clone_<id>` /
     `clux_env_dispose_<id>`（值里不存函数指针），不再 `clux_new`；
   - 函数体：`struct env *clux_v___ctx_env = (struct env *)env;`，捕获访问
     走 `->`；
   - `FREE` / 赋值 / 返回按内联聚合处理：own 字段释放、clone 调 `env_clone`；
   - 删除 `clux_func_release` 中"释放 env 指针"的路径。
7. **测试/示例迁移**：
   - 新增：闭包类型注解、`apply[F]`、含 `own` 捕获的 clone/move/dispose、
     C 与 VM 差分（env 内联后无泄漏、无双释放）；
   - 现有 300+ 处 `func(...)` 承载捕获闭包改为泛型/闭包类型；过渡期可先
     允许加宽（deprecated），最终删除。

## 5. 分阶段落地（每步保持套件绿色）

1. 类型系统 + 语法 + sema 解析闭包类型（只允许显式注解，字面量仍可推断为
   普通 func）；
2. C 后端内联 env + clone/dispose（先不改类型层面擦除），验证含 own 捕获；
3. VM 内联值表示 + compiler 捕获类型 hoist；
4. 闭包字面量类型 = 闭包类型，打开 no-erasure 检查；
5. 迁移 tests/examples，删除加宽；
6. 文档/ABI 收尾（导出 C ABI 的闭包约定、跨模块闭包类型）。

## 6. 验收用例

```clux
func apply[F](f: F, x: i32): i32 { return f(x); }

func main(): i32 {
    var base: i32 = 3;
    var f: func |base: i32| (i32) -> i32 =
        func |base| (x: i32): i32 { return base + x; };
    if (apply[func |base: i32| (i32) -> i32](f, 4) != 7) { return 1; }

    var p: own *i32 = new i32{ 5 };
    var g: own func |p: own *i32| (i32) -> i32 =
        move(func |(p = move(p))| (x: i32): i32 { return p.* + x; });
    if (apply[func |p: own *i32| (i32) -> i32](g, 4) != 9) { return 2; }
    return 0;
}
```

C 与 VM 都必须：无泄漏、无环境堆分配（C 代码中无 `clux_new` 闭包 env）、
`own` 捕获在作用域结束时释放一次。

---

# 实施进度（交接文档）

更新时间：2026-10-25，基于 `da76c01`（C 静态 clone + 阶段 D 迁移）+ 之前的
`3712386`（阶段 B + 文档）。

## 已完成

### 阶段 A：类型携带闭包（提交 `d25140f`）

- `func_type_t` 增加 `capture_types[] / capture_count`；`type_func_closure(vm, params,
  n, ret, variadic, capture_types, n)` 按 (签名, 捕获类型序列) intern；类型名形如
  `func |i32| func(i32): i32`；访问器：
  `func_type_is_closure / func_type_capture_count / func_type_capture_type`。
- parser/AST：`func |a: i32, b: own *i32| (i32, i32) -> i32` 类型表达式
  （`ast_func_type_t.captures`，捕获项必须 `name: Type`，不能带初始值）；
  `type F = ...` 别名路径（CTFE `AST_FUNC_TYPE`）同样支持。
- sema：闭包字面量/局部函数值类型 = 闭包类型；不擦除（闭包类型 ≠ 普通
  `func(...)`）；泛型 mangle 纳入捕获类型；`sema_check_func_literal` 在
  `capture_type_id` 字段处构造闭包类型并作为字面量值类型返回。
  - 运行期函数对象仍用普通签名 id（`fn->sig_id` 不改），避免 VM/C 运行期类型
    未构造的问题；闭包类型目前是 **sema 类型身份**。
- 新 gtest：
  - `CBackend.BuildClosureTypeAnnotationAndGenericCall`
  - `CBackend.RejectsClosureErasureToPlainFunc`

### 阶段 B：C 后端内联 env（提交 `a216acd`）

- `cir_signature_t` 增加 `env_type`；`signature_for_env(lc, ret, pc, pt, env_type)`
  按 (签名, env 类型) 去重；普通函数的 env 是共享 `struct clux_<tag>_empty_env
  { char clux_unused; }`（统一结构形状，调用点无需分支）。
- `map_type(TYPE_KIND_FUNC)`：
  - 普通函数：`struct sig_N { ret (*fn)(void *env, ...); struct empty_env env; }`；
  - 闭包：env 结构 `struct clux_<tag>_env_<typeid>`，字段按捕获序
    `clux_f_0..N-1`；签名 struct 为 `{ fn; struct env_N env; }`。
- writer：
  - 签名定义输出 env 字段；函数对象 `{ fn, { 0 } }`；
  - `CIR_EXPR_CLOSURE` → `{ .fn = impl, .env = (struct env_N){...} }`（无
    `clux_new`，无 clone/dispose 指针字段）；
  - `CIR_EXPR_FUNC_CALL` → `(callee).fn((void *)&(callee).env, args)`；
  - 函数体：`struct env_N *clux_v___ctx_env = (struct env_N *)env;`；
  - `__ctx.env.<cap>` 降级为 `(*clux_v___ctx_env).clux_f_<声明序>`。
- `lower_closure_literal` 重写：捕获类型数组 → `type_func_closure` → `map_type`
  → 内联 env 构造；删除堆 env/clone 包装；body 捕获走位置索引
  （`lower_capture_t.index`）。
- `FREE`：
  - 闭包：对 own/fatal 捕获槽位逐字段 `clux_free((f).env.clux_f_i)`；
  - 普通函数：no-op；`clux_func_release` 宏改为 no-op。
- `type_has_own_slots` 增加 FUNC 分支（捕获递归判断），闭包含 own 时为
  move-only。
- 新 gtest：`CBackend.BuildClosureOwnCaptureInlineEnv`（own 捕获 + `move(...)`，
  C 构建/运行 exit 0，生成 C 中 own 字段在返回路径释放）。

### 阶段 C（C 侧 clone）与跨模块修复（本次）

- sema：`gen_clone_expr_impl` 缺 `TYPE_KIND_FUNC` 分支——`var g = clone(f)` 在
  "扁平化 clone"路径退化成**浅拷贝**（两个闭包共享 env，随后双重释放）。抽出
  `gen_deep_clone_call`，`gen_clone_expr` 与 `gen_clone_expr_impl` 共用。
- sema 新增 `sema_find_clone_fn`：只**查找**已生成的 clone 函数（kind + 类型 →
  fid/名字），供 lowering 阶段引用（此时再生成不会进入程序函数链）。
- C 后端 `CIR_EXPR_FUNC_CLONE`：闭包类型含 own 捕获时生成并缓存静态辅助函数
  `static struct <sig> <sig>__cl(struct <sig>)`，逐捕获槽调用生成的
  `__cloneptr_*` / `__cloneslice_*` / `__clone_*`；表达式为 `helper(value)`
  （单次求值）。无 own 捕获仍是浅拷贝。`FREE` 不再依赖释放指针。
- CIR 类型发射：函数值作为聚合字段（`struct Holder { cb: F; }`）时按依赖先发
  签名 struct（`cw_emit_type` 新增 FUNC 分支），避免 "field has incomplete
  type"；已被头文件/前面发射过的签名用 `emitted` 去重。
- 跨模块：签名 struct 的 env 字段类型（`empty_env` / 闭包 env）现在递归进头文件
  （`cw_mark_header_type` 增加 `sig->env_type`），修复 import 构建的
  "incomplete type `struct clux_xxxx_empty_env`"；`empty_env_type` 原来用函数级
  static `cached_lc` 缓存，多模块 lowering 时 `lower_ctx` 栈地址复用会拿到**别的
  模块**的 env 类型，改为按 `cir_unit_t` 缓存。
- 新 gtest：`CBackend.BuildClosureInStructClone`（`clone(Holder{ cb: own F })`，
  源/副本各自独立调用 + 各释放一次）；`BuildCloneCaptureClosure`、
  `BuildOwnCaptureClosure`、`BuildFunctionValuesAndClosure` 迁移为闭包类型后通过。

### 阶段 D（迁移）：存量擦除用例

- `CBackend` / `CBackendDiff` / `ClosureValueOwn` / `ComptimeClosure` /
  `Decorator` / `Driver` 中"用 `func(...)` 承载捕获闭包"的用例已改成闭包类型 +
  泛型调用：
  - `var f: func(i32) -> i32 = func |a| ...` →
    `var f: func |a: i32| (i32) -> i32 = ...`（或 `type F = ...`）；
  - 高阶函数 `func apply(f: func(i32)->i32, ...)` → `func apply[F](f: F, ...)`，
    调用点 `apply[func |a: i32| (i32) -> i32](f, 4)`；
  - 聚合字段/数组/可选/slice 写完整闭包类型（如
    `own ?own func |q: own *i32| () -> i32`）。
- 下列套件当前通过：`CBackend` 的闭包/import 用例、`CBackendDiff`、
  `ClosureValueOwn`、`ComptimeClosure`、`Decorator`（21 个）、`Driver` 的
  closure/func-value 用例。
- `ClosureValueOwn.OwnCaptureCloneAndGenericCall`（新增）：own 捕获 + `clone` +
  泛型调用，VM 侧结束时 allocator 无泄漏。
- examples 迁移：`examples/closure`、`examples/decorator`、
  `examples/decorator_param` 改成闭包类型返回（C 与 VM 均运行 exit 0）。
- comptime 折叠出的闭包常量现在带**闭包类型**（`sema_ct_encode`）；C 后端仍保留
  `decl_value_type` 兜底：声明类型是普通 `func(...)` 而初始化器是带 env 闭包时，
  按初始化器恢复闭包类型，否则内联 env 被截断（历史注释路径，仍用于显式
  `func(...)` 注解 + 闭包字面量的迁移过渡）。
- 推断型 `var` 的 sema 类型写回 `resolved_type`（`shadow_var_def`），后端不再
  用 `func_type_of_def` 重新推断（否则闭包字面量与变量类型是两个 sig struct）。

### 阶段 C（VM，部分）：env 内联进函数实例（本次）

- `func_instantiate_raw`：闭包实例改为**一次分配** `sizeof(bcode_function_t) +
  env_size`，env 指向结构体尾部对齐后的内联块；函数对象新增 `env_inline` 标记。
- 所有 env 释放点（`exec_memory.c` 的 FREE、`type_ptr.c` 的递归销毁、
  `func_destroy`）在 `env_inline` 时只清槽位、不单独 `allocator_free(env)`
  （随实例分配块一起回收）。
- `func_clone`（vtable）仍是**指针共享**：`move(f)` 在运行期就是字节拷贝，
  无条件深拷贝会让被移走的源实例无人释放（实测 own 捕获 move 进聚合后泄漏
  4 字节）。显式 `clone(f)` 由 sema 折叠成 `__deep_clone` → `ptr_clone_block`
  的 FUNC 分支（新建实例 + env vtable 深克隆）。
- 结果：VM 闭包不再为 env 单独 malloc；`ClosureValueOwn` 全组（含 own 捕获
  move/clone/嵌套/容器）无泄漏。实例本身仍挂在 `vm->functions`，随 VM 销毁
  统一回收（未做按值回收）。
- **捕获可写 + 值语义拷贝**：捕获赋值在 sema 里与读取同构改写为
  `__ctx.env.<cap> = ...`（VM 走 FIELD_SET 闭包分支写回实例 env，C 走
  `(*ctx_env).clux_f_i`）；非 own 捕获闭包的**拷贝**（`var g = f`、`g = f`）
  由 sema 插入生成的 `__deep_clone`，env 深拷贝、两份独立（字面量/move 不
  插入）。C 与 VM 行为一致（差分用例 `CBackendDiff.ClosureCopyIsValueSemantic`、
  `ClosureValueOwn.CaptureMutation*`）。
- **聚合内部的闭包槽位**同样按值语义：sema 对"含闭包槽位且无 own 槽位"的
  struct/tuple/array/option/union/slice 拷贝也插入 `__deep_clone`；VM 的
  `ptr_clone_block` 用 `type_has_func_slots`（运行期闭包字段的类型可能已折叠成
  签名类型）进入聚合，再按运行期 `func_t->capture_type` 深拷贝闭包槽位。
  C 侧 struct/数组赋值本身就是内联 env 的字节拷贝，`__deep_clone` 降级为
  恒等（无需额外代码）。用例：`ClosureValueOwn.StructClosureFieldCopyIsIndependent`、
  `ArrayClosureElementCopyIsIndependent`、`CBackendDiff.StructClosureCopyIsValueSemantic`。

### 装饰器链修复（本次，顺带）

- `ds_take_metadata` / `ds_take_noreturn` 的过滤循环先把 `d->next = NULL` 再让
  `for` 的递增读 `d->next` → 链在第一个非 metadata 装饰器处被截断：**多个函数
  装饰器只保留一个**（值装饰器不走这两个过滤器，所以 `[[a]] [[b]] var` 正常）。
  改为先保存 `nx = d->next`。
- 修复后 `[[add100]] [[add100]] f` / 参数化链 `[[scale(10)]] [[add(5)]] f` 真正
  按源码序嵌套；`Decorator.ParameterizedDecoratorChain` 迁移为泛型装饰器
  （`comptime func add[F](n: i32, f: F): func |n: i32, f: F| ...`）并真正验证
  `combo(1) == 70`（VM 通过）。
- 注：C 后端的全局初始化器问题也已修复——`sema_ct_encode` 的 FUNC 分支现在把
  常量类型记为**闭包类型**（捕获槽位类型进入身份），`sema_ct_lit` 重建嵌套捕获
  的 `type_expr` 时得到的是内层闭包类型，C 后端按捕获类型重算 env 布局不再
  退化成内层/模板的 sig struct。`examples/decorator_param` 重新演示装饰器链
  （`combo(1)=70`，C + VM 均通过）。

### 阶段 C（C 侧补充）：嵌套闭包捕获与聚合捕获
- `capture_clone_kind` 新增 FUNC 分支（own 闭包捕获 → kind 3）：闭包的静态
  clone 辅助函数会递归调用**嵌套闭包类型**的 `<sig>__cl`，深拷贝内层 env。
- `lower_stmt` 的 FREE 改为递归 `emit_closure_free`：嵌套闭包捕获下钻到内层
  捕获槽，不再对闭包结构体值调用 `clux_free`。
- raw helper 改为**追加**（`cir_unit_add_raw_append`）：被调用的嵌套 clone
  helper 先于调用者发射（原来的 prepend 顺序会先引用后定义）。
- 新 gtest：`CBackend.BuildNestedClosureCaptureClone`（own 闭包捕获 own 闭包
  + clone，两层 env 独立释放）、`CBackend.BuildClosureAggregateCaptureFree`
  （捕获"含 own 槽位的聚合"时 FREE 走生成的 `__destroy_<T>(ref *T)`，不再对
  结构体值 `clux_free`）。

### 导出 C ABI 的闭包约定（本次）

- `export func` 的参数/返回可以是闭包类型：签名 struct 与内联 env struct
  （含捕获槽位类型）都进模块头文件；跨模块 import 调用按 extern 原型传值。
- 新 gtest：`CBackend.BuildExportClosureABI`（a 模块导出 `mk(base) ->
  func |b: i32| (i32) -> i32` 与 `call(f, x)`，main 模块 import 后调用，
  C 构建/运行 exit 0）。
- `CBackend.BuildExportClosureAcrossModules` 覆盖更多形态：
  - 跨模块**泛型** `mk[T](v: T): func |v: T| () -> T`，main 用 `ma::mk[i32](42)`；
  - 闭包捕获模块内 struct（`func |b: Box| () -> i32`，Box 来自导出模块）；
  - `fatal func |p: own *i32| ...` 返回 own 闭包，所有权跨模块转移、调用方
    作用域释放一次。
  C 与 VM 均通过。
- 值拷贝优化：`__deep_clone` 只对**左值来源**（ident/member/index/deref/unwrap）
  插入；调用/构造/new/make 产物是新实例，不再多克隆一层（否则跨模块调用
  返回的闭包会被再克隆，C 侧生成的 `__deep_clone` 调用无定义）。
- 清理：删除已无生成点的 `CIR_EXPR_CLOSURE_RELEASE` / `clux_func_release`
  no-op 宏（值里不再有 clone/dispose 函数指针，普通函数值的 FREE 是 no-op）。

## 当前状态 / 已知红

- 设计上 **不擦除**：`func(...)`（无捕获）与闭包类型是不同类型，赋值/传参必须
  用泛型或闭包类型注解。存量闭包用例已按此迁移（见上）。
- 全量 gtest 的剩余失败以**环境问题**为主，与闭包无关：
  - `FfiStructTest` / `FfiCallbackE2E` / `DriverM6`：本机 SEH 0xc0000005
    （libffi 调用路径）；
  - `MemoryBudget` / `TestSyntax` / `VmLifecycle` 等：gtest death test 无法在
    `%TEMP%` 建临时文件（error 5），进程 exit 3；
  - `CBackend` 个别用例经 `std::system()` 拉起子进程时偶发 0xC0000005（重跑
    常绿）。
- 因此全量 gtest 仍不宜作为唯一验收标准；按套件运行并区分环境性失败。

## 后续可选工作（不阻塞验收）

1. **VM 按值实例回收（优化）**：env 已内联进函数实例分配，变量/赋值/聚合内部
   闭包槽位拷贝都按值语义深拷贝 env，own 捕获无泄漏（`ClosureValueOwn` 全组
   验证）；实例仍统一挂在 `vm->functions` 直到 VM 销毁。按值回收属于内存上限
   优化，需要引用计数/值内联捕获块，并牵动 `func_assign`/`value_clone`（move
   是运行期字节拷贝，不能无条件深拷贝）。
2. **运行期闭包值的类型身份仍是签名类型**：sema 的闭包常量/变量是闭包类型
   （捕获进入身份），但 `func_instantiate` 产出的运行期 value type 仍是普通
   `func_type_t`（sig 与闭包类型共享布局；`capture_type` 挂在函数对象上）。
   目前 inline env + 静态分派工作正常；若要让运行期类型反射、跨模块 ABI、
   `cfunc_ptr` 看到完整闭包类型，需要把 value type 也切到闭包类型——试过直接
   改会让 `ClosureValueOwn` 整组失败（move/赋值路径共享同一实例），需要与
   值生命周期改造一起做。
3. **闭包捕获聚合的 clone**：C 的 `emit_closure_free` 对 FUNC（递归）、
   own 指针/切片、含 own 槽位的聚合（`__destroy_<T>`）已覆盖；clone 侧对
   聚合捕获走生成的 `__clone_<T>`（kind 0），也已覆盖。剩余：捕获槽为
   cunion / share 指针时的销毁语义（沿用旧行为，未按 RC 计数）。
4. **清理旧表示**：C 后端值里已无 clone/dispose 指针，`CIR_EXPR_CLOSURE_RELEASE`
   与 `clux_func_release` 已删除；`capture_clone_kind` 与 sema
   `clone_needs_deep` 的规则仍重复（后续可共享），VM 侧 `env_*_fid` 仍是
   clone/free 的运行期入口（保留）。
5. 文档/ABI 收尾：导出函数的闭包参数/返回已验证（`BuildExportClosureABI` /
   `BuildExportClosureAcrossModules`，含跨模块泛型与 fatal own 闭包）；
   剩余：`cfunc_ptr` 与闭包类型的交互（带捕获闭包无 C 等价物，已有拒绝用例）、
   导出闭包值在**纯 C 调用方**里的生命周期约定（当前按值结构体，调用方负责
   传递/释放约定需要文档化）。

## 关键代码位置（继续实施时）

- 类型：`include/vm/type_func.h`、`src/vm/type_func.c`（`type_func_closure`、
  `func_type_seal` 里的 `func_caps_eq`、名称生成）。
- 解析/AST：`include/parser/ast_func_type.h`、
  `src/parser/parse_func_def.c`（`parse_capture_item`/`parse_capture_list`、
  `func |...|` 类型分支、`stop_at_pipe`）、`src/parser/parse_expr.c`。
- sema：`src/sema/sema_type.c`（`AST_FUNC_TYPE` 分支）、
  `src/sema/stmt_build_func.c`（`sema_check_func_literal` 末尾闭包类型）、
  `src/ctfe/ctfe_expr.c`（类型别名路径的 `AST_FUNC_TYPE`）、
  `src/sema/sema_generic.c`（mangle 捕获）、
  `src/sema/expr_clone.c`（`gen_deep_clone_call`、`sema_find_clone_fn`、
  `clone_needs_deep`）。
- C 后端：`include/cbackend/c_ir.h`（`cir_signature_t.env_type`、
  `cir_type_t.closure_clone`、`cir_unit_t.empty_env`）、
  `src/cbackend/c_lower_type.c`（`signature_for_env`/`empty_env_type`/
  `map_type` FUNC 分支）、`src/cbackend/c_lower_literal.c`
  （`lower_closure_literal`）、`src/cbackend/c_lower_expr.c`
  （`__ctx.env` 位置捕获、`closure_clone_helper`、`__deep_clone` lowering）、
  `src/cbackend/c_lower_stmt.c`（`FREE` 闭包字段）、
  `src/cbackend/c_writer_stmt.c`（签名 env 字段、函数体 env 指针）、
  `src/cbackend/c_writer_expr.c`（CLOSURE/FUNC_CALL/FUNC_CLONE）、
  `src/cbackend/c_writer_types.c`（类型依赖顺序 / 头文件 env 类型）、
  `src/cbackend/c_lower_infer.c`（函数字面量推断为闭包类型）、
  `src/cbackend/c_lower_util.c`（`decl_value_type`：变量落 C 类型修正）。
- 测试：`tests/c_backend_test.cpp` 末尾闭包用例（含
  `BuildClosureInStructClone`、`BuildNestedClosureCaptureClone`）、
  `tests/closure_value_ownership_test.cpp`（`OwnCaptureCloneAndGenericCall`）。
- VM 内联 env：`include/vm/function.h`（`env_inline`）、
  `src/vm/bcode_function.c`（`func_instantiate_raw` 单次分配）、
  `src/vm/exec_memory.c` / `src/vm/type_ptr.c` / `src/vm/function.c`
  （释放点跳过内联块）。

## 构建/运行备忘

```powershell
cmake --build build
# 若 link 报 clux.exe permission denied：先杀掉残留 clux/clux_test 进程再构建
# 全新 build 目录：cmake -S . -B build 会重新生成
# build/third_party/icu/icu_data_gen.c（clean 后必须 reconfigure 生成）

# 新增/迁移后的闭包用例（按套件，比全量 gtest 可靠）
.\build\clux_test.exe --gtest_filter="CBackend.BuildClosure*:CBackend.BuildOwnCapture*:CBackend.BuildCloneCapture*:CBackend.BuildFunctionValuesAndClosure"
.\build\clux_test.exe --gtest_filter="ClosureValueOwn.*:ComptimeClosure.*:Decorator.*:CBackendDiff.*"
```

## 验收目标（阶段 C/D 完成后）

```clux
type F = func |a: i32| (i32) -> i32;
func apply[G](f: G, x: i32): i32 { return f(x); }
var f: F = func |a| (x: i32): i32 { return x + a; };
apply[F](f, 4) == 7

type O = func |p: own *i32| (i32) -> i32;
var g: own O = move(func |(p = move(p))| (x: i32): i32 { return p.* + x; });
g(4) == 9
```

C 生成代码：闭包值 `{ fn; env struct }`、无 env 堆分配、无
`env_clone/env_dispose` 函数指针；own 捕获在作用域结束释放一次；VM 同样无泄漏。
