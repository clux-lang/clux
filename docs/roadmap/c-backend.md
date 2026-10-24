# C 后端路线

> 本页是工程路线，不是语言规范。语言语义以 `docs/whitepaper/` 为准。

## 目标

实现 `clux build <file.cx>` 的 C 后端：

- 单个 `.cx` 入口；
- 无 `clux.mod`；
- 不处理动态库/静态库链接参数；
- 生成 C 源文件；
- 调用宿主 C 编译器生成可执行文件。

第一版保留 VM 后端作为语义参照，C 后端通过全部测试后再切换默认后端。

## 分层

```text
clux AST（sema 之后、已单态化）
    → lowering（clux 语义 → C 级语义）
    → C IR（typed）
    → C IR optimize
    → writer（纯输出）
    → C 源文件
    → host cc
    → 可执行文件
```

### C IR 约束

- C IR 只描述 C 级构造；
- 函数统一使用 env-first ABI；
- writer 不负责类型、名字、清理或求值顺序决策；
- C IR 保留源位置，生成 `#line`。

### 函数表示

每个 clux 函数先表示为：

```c
struct clux_fn_i32_void {
    int32_t (*fn)(void *env);
    void *env;
};
```

非捕获函数的 env 为 `NULL`。

C IR 优化阶段可以消除未逃逸的函数对象，退化为直接函数调用；第一版保留对象。

## 里程碑

### M0：最小链路（已完成）

- C IR 节点与 writer；
- `func main(): i32 { return 0; }`；
- 生成函数对象、函数声明、函数实现、C `main` wrapper；
- `clux build x.cx` 调用宿主编译器；
- 生成可执行文件并以退出码验证。

当前支持：

- 无参 `main`，返回 `i32` 或 `void`；
- `return <整数表达式>`（整数、`+ - * / %`、一元运算符）；
- 生成 C 函数对象 struct（`fn + env`）；
- 生成 C `main` wrapper；
- 生成文件按类型声明/定义、函数声明、对象定义、函数实现分节。

验收：

```sh
clux build hello.cx
./hello
# exit code 0
```

### M1：标量与控制流（已完成基础版）

- i32/u32/usize/bool；
- 局部变量、赋值；
- 算术、比较、逻辑、位运算；
- if / else；
- while；
- break / continue；
- return；
- 函数参数、直接函数调用；
- 多函数声明/定义；
- 局部变量类型推断（字面量与简单表达式）。

当前已支持：

- `func add(a: i32, b: i32): i32` 等普通函数；
- 直接调用 `add(x, y)`；
- `var x = 1` 的类型推断；
- `if` / `else` / `while`；
- `break` / `continue`；
- 复合赋值 `+=` 等。

尚未覆盖：

- `for`、`do while`；
- 全局变量；
- struct/数组/指针；
- 字符串；
- 函数值/闭包。

验收：一组返回不同退出码的程序与 VM 后端结果一致。

### M2：标量补全（已完成基础版）

- `for`；
- `do while`；
- 全局变量；
- 全局变量读取/写入；
- 复合赋值的表达式形式（用于 for update）。

当前支持：

- `var counter: i32 = 1;` 全局；
- `for (var i = 0; i < 4; i = i + 1)`；
- `do { ... } while (...)`；
- 函数读取全局变量。

尚未覆盖：

- 非常量全局初始化器（调用函数/依赖运行期值）；
- 全局变量的跨模块/导出语义。

验收：for/do while/全局变量程序退出码与 VM 后端一致。

### M3：复合类型（已完成基础版）

- struct 声明/定义与依赖排序；
- struct 构造与字段读写；
- 数组类型、数组构造与下标读写；
- 指针类型、取地址、解引用；
- 指针字段自动解引用（由 sema 已改写的 AST_DEREF 承接）；
- 函数按值接收 struct。

当前支持：

- `struct Point { x: i32; y: i32; }`；
- `.Point{ .x = 2, .y = 3 }`；
- `p.x` / `p.x = 5`；
- `var a: [3]i32 = .[3]i32{ ... }` / `a[i]`；
- `ref *Point` / `p.&` / `r.y`；
- 按值 struct 参数与返回类型；
- 数组用 `struct { elem data[N]; }` 包装，不使用会退化的 C 数组。

尚未覆盖：

- 嵌套复杂布局/联合体/enum；
- slice / str；
- 堆分配 `new` / `make`；
- 数组/结构体的所有权 cleanup（M4）。

验收：struct/数组/指针程序退出码与 VM 后端一致。

- own；
- ref；
- defer；
- drop；
- move / clone；
- 作用域清理顺序。

### M4：所有权与清理（已完成基础版）

- sema 生成的 `AST_FREE`；
- `own *T` 的自动释放；
- 复合类型 own 槽位的销毁函数；
- 数组 own 元素的逐元素释放；
- return 前先求值、再执行清理；
- 退出点清理按 sema 生成顺序执行。

说明：`defer` / `move` / `clone` 在 sema 之后已经消减为退出点调用/数据流结果，C 后端不重新实现这些语言原语。

当前支持：

- `var p: own *Point = new Point{ ... };`；
- `own *T`、`own *own *T`；
- struct 的 own 字段；
- `[N]own *T` 的逐元素 free；
- `own []T` / `ref []T`；
- `str` 字符串字面量与按字节 `len`；
- `str` 比较 `==` / `!=`；
- `str[low:high]` 转为 `[]const u8`；
- tag union 构造 / `is` / 成员读写；
- `?T` 构造 / `nil` / `!= nil` / `.!` / 隐式 some 包装；
- optional 字段读写；
- union own payload 清理；
- cunion 构造 / 成员读写 / 按值赋值；
- `len(s)`；
- `make(T, N, ...)`；
- `s[low:high]`；
- slice 堆块 free；
- 由 sema 生成的 `__destroy_*` 函数；
- `new T{...}` 使用 `clux_new`（calloc + memcpy）。

尚未覆盖：

- `str` 拼接 / 格式化 / FFI C ABI 兼容；
- cunion 的 FFI 布局/生命周期细节；
- 泛型实例的完整覆盖；
- 异常路径 / 全部跨作用域路径的清理验证。

### M5：函数值、闭包与方法（已完成基础版）

- 函数值 struct；
- 捕获环境；
- 方法接收者；
- 方法调用；
- 函数对象逃逸分析（后续优化）。

当前支持：

- `func(T) -> R` 函数类型；
- 命名函数作为函数值；
- 函数值调用、作为参数传递；
- 非捕获函数字面量；
- `func |cap|` 显式捕获闭包；
- struct 实例方法调用（`p.m()`）；
- struct 静态方法调用（`T::f()`）；
- 未被作为函数值引用的函数对象不生成（C IR 优化）；
- 函数对象携带 `env_clone` / `env_dispose`，指向 sema 生成的 env 函数；
- `clone(f)` 调用 sema 生成的 env clone 函数，得到独立 env；
- 函数值销毁直接调用 sema 生成的 env dispose，并释放 env；
- C 后端不引入引用计数，生命周期由 sema 的 FREE / CLONE 节点驱动。

尚未覆盖：

- struct/数组字段中的函数值复制路径；
- 跨模块方法。

说明：泛型在 sema 阶段完成单态化/消减，C 后端只接收具体函数，不需要独立处理泛型。

### M6：语言特性收敛（已完成基础版）

- bitfield；
- metadata 全局常量；
- test 运行器；
- sema 单态化后的具体函数覆盖；
- 与 VM 差分测试；
- 构建产物导出（`--emit-c`）。

当前支持：

- bitfield：
  - C 布局 = 隐藏字数组（`struct { <underlying> data[N]; }`），与 VM 的
    `ceil(total_bits / word_bits)` 存储逐字节一致；
  - `.Flags{ ... }` 构造：全常量时直接生成隐藏数组的常量初始化器（可用于
    全局变量），含运行期值时生成 `clux_bf_make_<type>` 辅助函数；
  - 字段读 `f.a` / 写 `f.a = v` / 复合赋值 `f.b += v`：`clux_bf_get` /
    `clux_bf_set`，位流算法与 `vm/exec_bitfield.c` 相同（跨字、掩码）；
  - 指针接收者自动解引用（`p.a`，sema 已改写成 `AST_DEREF`）；
  - bitfield 作为 struct 字段 / 数组元素 / 参数 / 返回值；
  - `sizeof` / `alignof` 由 sema 折叠。
- metadata：
  - `get_metadata` / `get_field_metadata` / `get_method_metadata` 由 sema
    折叠为字面量，C 后端零运行期查找；
  - **全局常量初始化器**可以读取 metadata：sema 的顺序调整为
    `全局 comptime var → 字段/方法 metadata + 类型装饰器 → 普通全局变量`；
  - 泛型实例的 metadata 由 sema 在实例化时应用（`Box[i32]` 等）。
- 显式类型转换 `as`：`CIR_EXPR_CAST` → C cast。
- 三元表达式 `cond ? a : b`：`CIR_EXPR_TERNARY`。
- switch：按 `if / else if` 链展开（cond 只求值一次 → 临时变量），无
  fallthrough；字符串模式用 `clux_str_eq`，模式惰性求值。
- 复合值判等：`cunion` 用 `memcmp`（与 VM 字节比较一致）、slice 用
  `(data, len)`、函数值用 `(fn, env)`；`!=` 取反。
- struct 构造的 optional 字段：`nil` → none、非 optional 值 → 隐式 some
  （sema 会把构造字段链重排成类型表序并去掉字段名，所以字段类型按**下标**
  对齐）。
- 错误处理语法糖 `.?' / `.!'（`Result` 风格 struct）：
  - 变量初始化 / 赋值：消费 sema 生成的 `prelude` / `postlude`（临时变量 +
    `if (ok) ... else ...`）；
  - `return expr.?`：sema 生成 `AST_TRY`，lowering 展开成
    `{ setup; if (!ok) { tail } return value; }`；
  - `clux_assert`/`prelude`/`postlude` 的语句链拼接要求尾指针走到链尾
    （否则后续语句会截断链）。
- 泛型与后端边界（架构契约）：**泛型必须在 sema 内实例化完毕**，后端不处理
  任何泛型构造。
  - 函数实例 / 类型实例：sema 从定义 token 区间**重新解析**出干净 AST（
    `sema_generic.c` 的 `parse_func_def` / `parse_struct_def`），把类型参数
    替换成具体类型后追加到 `prog->funcs`；
  - 泛型 struct 的实例方法走同一条路：实例的结构体定义（名字含 `$`）连同
    方法一起重新解析，`T` 在实例方法签名里已经变成具体类型（可直接
    `sema_resolve_type_slot`，不依赖实例绑定）；
  - 泛型方法（`func map[U]`）保留**模板**在成员表里，调用点再实例化成
    具体函数（同样追加到 `prog->funcs`）；
  - 模板（顶层泛型函数/结构体、类型参数的成员模板）不生成代码：后端按
    `generic.count` 跳过（与 VM compiler 的 `compile_func.c` 一致）；
  - 后端边界自检：`cbackend_lower` 结束时检查所有 CIR 调用目标都有定义
    （本 unit 函数或已知运行期辅助函数），缺失时报内部错误——把
    “sema 漏实例化”变成可诊断的失败，而不是 host cc 的
    `undeclared function`；
  - 回归测试：`SemaTest.GenericInstancesAreMaterializedBeforeBackends`
    （sema 侧断言实例产物与具体签名）、
    `CBackend.BuildGenericStructInstanceMethods` /
    `BuildGenericMethodInsideGenericStruct` /
    `BuildNestedGenericAndRvalueReceiver`。
- test 运行器：`clux test <file.cx> --backend=c`
  - 顶层 `test "Suite.Name" { ... }` 改写成无参 void 函数；
  - 生成 `int main(int argc, char **argv)`：`argv[1]` = 用例下标，执行单个
    用例后退出（0 = 通过）；
  - CLI 以子进程逐个运行用例，panic/assert 只影响当前用例；
  - `assert(cond)` 映射为生成的 `clux_assert`（失败打印 `assert failed: ...`
    并以非 0 退出）；
  - 输出与 VM 运行器同款 gtest 风格汇总。
- `clux build` 选项：
  - `--emit-c[=PATH]`：只生成 C 源，不调用 host cc；
  - `--output=PATH`：指定可执行文件路径。
- VM 差分测试：`clux run` 现在把 `main` 的整数返回值作为进程退出码，
  与 `clux build` 产物一致；`tests/c_backend_diff_test.cpp` 对同一批程序
  同时跑两个后端并比较退出码。

尚未覆盖：

- 测试运行器的 `mock` / `unmock`（`clux test --backend=c` 暂不支持）；
- 测试运行器每个用例独立进程，全局变量在用例之间**不共享**（VM 运行器
  共享同一进程；依赖跨用例全局状态的测试需要显式重置）；
- `new` bitfield（sema 目前直接拒绝）；
- 依赖 FFI / `import` 的 examples（第一版明确不做，见“第一版明确不做”）；
- 全量 `examples/` 与 VM 的差分校准（M7）。

### M7：默认切换

- C 后端跑通全量测试和 examples；
- `clux build/run` 默认 C；
- VM 保留为调试后端；
- 再决定删除字节码路径。

## 第一版明确不做

- `clux.mod`；
- import；
- extern lib / extern cfunc / extern var；
- 动态库/静态库编译参数；
- 标准库；
- 跨文件链接；
- C 编译器参数透传。

## 诊断

- clux 语义错误在 lowering 阶段报告，定位到 `.cx`；
- 生成 C 的编译错误通过 `#line` 映射回 `.cx`；
- 内部错误必须附带 C IR 节点或生成 C 行号。

## 源码布局

```text
include/cbackend/
  c_backend.h       入口
  c_ir.h            C IR 数据结构
  c_lower.h         lowering 入口
  c_writer.h        writer 入口

src/cbackend/
  c_backend.c       单文件驱动：lex/parse/sema → C IR → C 源 → host cc

  c_ir_unit.c       unit / 全局 / 签名 / 函数链表
  c_ir_type.c       C IR 类型与字段
  c_ir_expr.c       C IR 表达式
  c_ir_stmt.c       C IR 语句
  c_ir_func.c       C IR 函数签名/函数

  c_lower.c         lowering 编排、函数、方法提升
  c_lower_internal.h
  c_lower_util.c    arena 字符串 / 名字
  c_lower_type.c    clux type → C IR type
  c_lower_expr.c    表达式 lowering
  c_lower_infer.c   表达式类型推断
  c_lower_literal.c slice/make/len/闭包等字面量
  c_lower_stmt.c    语句 lowering

  c_opt.c           C IR 优化（函数对象可达性）

  c_writer.c        writer 编排
  c_writer_internal.h
  c_writer_util.c   字符串缓冲辅助
  c_writer_types.c  类型 section
  c_writer_expr.c   表达式输出
  c_writer_stmt.c   语句 / 函数输出
```
