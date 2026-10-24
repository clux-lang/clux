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
- FFI（`extern lib` 链接参数 + `extern cfunc` 声明/调用/回调/返回值 +
  `extern var` 数据符号读写）；
- sema 单态化后的具体函数覆盖；
- 与 VM 差分测试；
- 两阶段构建（`.cx → .c → 目标文件 → 链接`）与工具链参数
  （`--cc/--cflag/--ldflag/-I/-L/-l/-D`）；
- 构建产物导出（`--emit-c` / `--emit-obj`）。

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
- enum：**C 侧就是底层整数**（不生成 C enum 类型）：
  - `enum Color: i32 { ... }` 的变量/字段/数组元素/参数按底层 C 类型
    （`int32_t` 等）存放；`sizeof`/`alignof` 由 sema 折叠；
  - variant 引用 `Color::Red`：sema 已把底层值折叠进 `AST_SCOPE_REF.value`，
    lowering 直接出整数字面量并转成底层 C 类型（`(uint8_t)(-56)` 这类归一，
    与 VM 的“按底层宽度存字节”语义一致）；
  - 判等、`as` 转底层类型、switch 模式（`(Color::Red, Color::Blue) -> ...`）
    都按整数处理；
  - `extern cfunc` 的 enum 形参/返回直接用底层整数。
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
- `clux build` 选项与两阶段构建：
  - 流水线：`.cx → .c →（host cc -c）→ 目标文件 → 链接 → 可执行文件`；
  - **构建目录镜像**（类 cmake 的 `build/`，默认 `./build`）：
    `./cmd/main.cx` → `./build/cmd/main.c`、`./build/cmd/main.obj`（或
    `.o`）、`./build/cmd/main.exe`（或 `./build/cmd/main`）；源文件已经在
    构建目录内时按构建目录内的相对位置镜像（不会出现 `build/build/...`）；
    `--build-dir=DIR` / `-B DIR` 可改构建目录（支持绝对路径，逐级自动建目录）；
  - `--emit-c[=PATH]`：只生成 C 源（默认落在镜像位置 `build/<rel>.c`）；
  - `--emit-obj[=PATH]`：只编译到目标文件（默认 `build/<rel>.obj|.o`；
    对象可被 host cc 独立链接，供多文件构建复用）；
  - `--output=PATH`：只覆盖可执行文件路径（中间产物仍走镜像目录）；
  - 工具链参数（全部逐条透传，与代码声明的 `extern lib` 链接参数合并）：
    - `--cc=PATH`：C 编译器（缺省 `$CC`，再回退平台默认）；
    - `--cflag=FLAG` / `--ldflag=FLAG`：编译/链接附加参数（可重复）；
    - `-I DIR` / `--include=DIR`：include 搜索目录（编译阶段）；
    - `-L DIR` / `--lib-path=DIR`：库搜索目录（链接阶段，先于代码声明的
      `-l`；其后是产物目录及其上两级）；
    - `-l LIB` / `--link-lib=LIB`：链接库（链接阶段，排在代码声明的链接
      参数之后）；
    - `-D KEY=VALUE`：编译期键表（`extern lib ... assert(...)` 用）；
  - 编译成功删中间 `.c`、保留目标文件与头文件；链接失败保留 `.c`/`.obj`；
    编译失败删半成品目标文件、保留 `.c`。
- 编译单元（`.c` + `.h` 配对，为多文件/`import` 做准备）：
  - **模块前缀**：`clux_<tag>_`，tag 是**源文件相对项目根目录（cwd）的
    路径**的短 hash（FNV-1a 低 16 位，4 位十六进制，如 `a61b`）。相对路径
    就是模块在依赖图里的身份：同一路径在任何机器/构建目录下前缀稳定；
    不同目录的同名文件前缀不同。函数 `clux_<tag>_fn_<name>`、全局变量
    `clux_<tag>_v_<name>`、函数对象 `clux_<tag>_obj_<name>`、类型
    `struct clux_<tag>_s_<id>`（以及 `a_/sl_/opt_/u_/cu_`、`sig_`、`cfn_`、
    `cb_`）都带前缀；
  - **导出规则**（`export` 前缀，parser 支持 func/cfunc/var/type/enum/
    struct/union/cunion）：
    - `export func`：头文件 `extern` 原型（env-first ABI），`.c` 定义不加
      `static`；
    - `export cfunc`：内部仍是 env-first 的 static 函数，另生成一个
      **C ABI 包装** `clux_<tag>_<name>(...)`（无 env，str 参数/返回自动
      编组），头文件里是它的 plain C 原型；
    - `export var`：头文件 `extern <C 类型> <符号>;`；`.c` 定义非 static；
    - 其余 `func`/`cfunc`/`var` 全部 `static`（多编译单元链接不冲突）；
    - `extern cfunc`（外部库声明）仍按 `extern` 原型放在 `.c`（不是本模块
      定义）；
  - **头文件类型**：导出类型（无条件，即使源码没用到）+ 导出函数/全局/
    `export cfunc` 签名的类型闭包；按**拓扑序**（依赖先）输出完整定义：
    struct/cunion/union 字段、array/option/slice 元素、指针基类型、函数值
    类型的签名结构体；`struct clux_str` 用
    `CLUX_STR_STRUCT_DEFINED` 守卫以便多个模块头共存；
  - `.c` 以 `#include "<模块>.h"` 开头，只定义头文件之外的私有类型；
  - 库模块（没有 `main`）可以单独编译：`--emit-c` / `--emit-obj` 不要求
    `main`（链接可执行文件时仍要求）；导出符号是真正的 C ABI 入口，
    手写 C 驱动 `#include "模块.h"` 即可调用并与对象一起链接
    （回归测试 `CBackend.CompileUnitLinksAcrossC` 覆盖整条链路）；
  - CLI `--emit-c` 同时产出 `.c`/`.h`；普通构建成功后删 `.c`、保留 `.h`
    与 `.obj`；
  - 回归测试：`CBackend.GeneratesHeaderPairForExports`、
    `HeaderTypesAreTopological`、`ExportedCfuncUsesCApi`、
    `ModulePrefixFromRelativePath`。
- **import / 多文件编译**（C 后端多编译单元）：
  - 入口与每个依赖模块各自生成 `.c/.h/.obj`（镜像到构建目录），最后把
    所有对象**一次链接**成可执行文件；模块缓存保证菱形依赖只编译一次，
    路径栈检测循环依赖；
  - 后端走 driver 的**前端准备**路径（新增
    `compile_module_prepare`：lex→parse→desugar→sema + 泛型实例排空，
    **不生成字节码、不执行注册段**）——C 后端因此不会在编译期 dlopen
    `extern lib`，库交给链接器处理；
  - 跨模块引用 lowering：`mod::fn(...)` → `extern <ret>
    clux_<depTag>_fn_<name>(void *env, ...)` + 调用（env 传 NULL）；
    `mod::VAR` → `extern <type> clux_<depTag>_v_<name>`；`export cfunc`
    走普通 C ABI 入口 `clux_<depTag>_<name>`（str 参数/返回自动编组）；
    `mod::Type` 在类型位置由 sema 解析（新增 `resolve_type_expr` 的
    AST_SCOPE_REF 分支），C 侧按同一 VM type_t 生成布局一致的本地类型；
  - **代理导出**（`export * from`）：导出符号按指针身份在 `module_order`
    里反查**定义模块**，C 符号前缀用定义模块的 tag（而非 reexport 方）；
  - `--emit-c` 会写出所有参与模块的 `.c/.h`；`--emit-obj` 编译所有模块；
    入口模块在链接可执行文件时才要求 `main`（库模块可单独出对象）；
  - 回归测试：`BuildImportMultiFile`、`BuildImportChainAndDiamond`、
    `BuildImportReexport`、`BuildImportCrossModuleGeneric`、
    `RejectsCircularImport`；
  - **跨模块泛型实例**：`mod::f[i32](...)` 的实例由 sema 物化到**定义模块**
    的 prog（实例函数名带跨模块 mangling 前缀，如
    `md37d4525$id$i32`），调用点被改写成实例的 FUNC_REF；C 后端按 fid 在
    `vm->module_order` 的所有 `sema->funcs` 里反查定义模块与实例名，生成
    `extern ... clux_<ownerTag>_fn_<instance>(void *env, ...)` 并调用。
    泛型实例函数在所有模块里都是 **non-static**（mangling 名全局唯一），
    但不进头文件（`is_public` 与 `is_exported` 分离）；
  - 尚未覆盖：跨模块**方法**调用（`import` 的结构体实例方法，sema 目前
    报 "struct 'X' has no field 'm'"；属语言层 gap，不是 C 后端问题）、
    跨模块泛型**类型实例**出现在泛型函数体内（`wrap[T](): Box[T]` 跨模块
    实例化时 sema 报 "construct: expected 0 fields for ...$Box$i32"，同为
    sema 层问题；单文件内同一写法正常）、`clux test --backend=c` 的
    import（测试运行器仍是单文件前端路径）。
- M6 FFI：extern lib / extern cfunc（`include/cbackend/c_ir.h` +
  `src/cbackend/c_lower_ffi.c`）：
  - `extern cfunc <lib>::<fn>(params): ret;`：生成 `extern` 原型（C ABI
    类型：整数/浮点/bool/str/opaque/struct/cunion/指针/cfunc；enum 按底层
    整数渲染），调用点直接调用真实 C 符号；`extern func <lib>::<fn>` 是
    同一 AST；
  - `str` 的 C ABI 表示是 `const char *`（与 VM 的 str = char* 一致）：
    实参取 `(v).data`，返回值用 `clux_str_from_c(p)`（`strlen` 求 len）；
  - `data(s)` → `(s).data`（`is_data_call`），与 `len(s)` 组成 C 的
    `(T*, n)` 对；
  - 变参：`...` 固定参数之后的实参按 C 默认实参提升（host cc 完成）；
  - `cfunc_ptr(f)`：只有 **`cfunc` 定义**（C ABI 函数，无 env）才能取 C
    函数指针——生成本后端 trampoline
    （`static <ret> clux_cb_<f>(...) { return clux_fn_<f>(NULL, ...); }`）；
    `cfunc(...)` 参数/返回类型生成 `clux_cfn_<n>` typedef；普通 `func` /
    闭包没有 C 等价物 → 明确拒绝（闭包不能作为 C 函数，语言语义如此）；
  - `cfunc(...)` 类型本身可作变量/参数/返回值（C 侧就是函数指针变量）：
    `var cb: cfunc(i32,i32) -> i32 = ...`、`extern cfunc ...: cfunc(...)`
    （返回 C 函数指针，sema 侧走 `BCODE_FFI_RET_TYPED`，VM 与 C 后端一致）；
  - `extern var <lib>::<name>: T;`：库导出的**数据符号**（C 侧是
    `extern <C 类型> <符号>;`，Windows 下带 `__declspec(dllimport)`，因为
    数据导入没有 thunk）：
    - 读 `lib::var`、写 `lib::var = v`（含复合赋值）；
    - 支持整数/浮点/bool/opaque/enum/struct/cunion/str：enum 按底层整数，
      struct/cunion 要求布局与 C 一致（含 `str` 字段的聚合拒绝），
      `str` 读成 `clux_str`（`strlen`）、写取 `.data`；
  - `extern lib <alias> from "<name>"`：**不生成 C 代码**，只影响 host cc
    命令行：
    - `"*"` → 不链接（符号来自当前进程，如 CRT）；
    - 通用名 → `-l<name>`，并在产物目录及其上两级目录加 `-L`（找不到时
      回落到系统搜索路径）；
    - 显式路径 / 库文件名（`.so`/`.dylib`/`.dll`/`.a`/`.lib`）→ 原样交给
      链接器；
    - `assert(<cond>)` 为假时前脱糖已裁剪整条声明 → 不产生链接参数；
  - 同名 C 符号不同签名的两条 `extern cfunc` 声明在 lowering 阶段报错；
  - 本后端 `str` 是 `{char*, len}`（16 字节），与 C ABI 的 `char*` 不同 →
    含 `str` 字段的聚合按值传参与 C 不兼容，declaration 处直接拒绝；
  - 后端边界自检把 extern 符号登记为“外部定义”，不再误报
    `has no C definition`。
- VM 差分测试：`clux run` 现在把 `main` 的整数返回值作为进程退出码，
  与 `clux build` 产物一致；`tests/c_backend_diff_test.cpp` 对同一批程序
  同时跑两个后端并比较退出码。

尚未覆盖：

- 测试运行器的 `mock` / `unmock`（`clux test --backend=c` 暂不支持）；
- 测试运行器每个用例独立进程，全局变量在用例之间**不共享**（VM 运行器
  共享同一进程；依赖跨用例全局状态的测试需要显式重置）；
- `new` bitfield（sema 目前直接拒绝）；
- 依赖 FFI / `import` 的 examples（`import` 仍属第一版明确不做，见下）；
- 全量 `examples/` 与 VM 的差分校准（M7）。

### M7：默认切换

- C 后端跑通全量测试和 examples；
- `clux build/run` 默认 C；
- VM 保留为调试后端；
- 再决定删除字节码路径。

## 第一版明确不做

- `clux.mod`；
- import；
- 标准库；
- 跨文件链接（目标文件与工具链参数已就绪，见 M6；多文件驱动属后续阶段）。

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
