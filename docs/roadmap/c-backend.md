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
- `len(s)`；
- `make(T, N, ...)`；
- `s[low:high]`；
- slice 堆块 free；
- 由 sema 生成的 `__destroy_*` 函数；
- `new T{...}` 使用 `clux_new`（calloc + memcpy）。

尚未覆盖：

- `str` 拼接 / 格式化 / FFI C ABI 兼容；
- cunion；
- 泛型实例的完整覆盖；
- 异常路径 / 全部跨作用域路径的清理验证。

### M5：函数值、闭包与方法

- 函数值 struct；
- 捕获环境；
- 方法接收者；
- 方法调用；
- 函数对象逃逸分析。

### M6：语言特性收敛

- bitfield；
- metadata 全局常量；
- test 运行器；
- 泛型实例完整覆盖；
- 与 VM 差分测试。

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
