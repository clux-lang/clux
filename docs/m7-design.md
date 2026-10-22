# clux M7 设计文档：泛型（参数化多态）

## 目标

为 clux 引入**参数化多态**（parametric polymorphism），覆盖函数、类型（struct/enum/union/cunion）与类型别名三种声明形态。类型参数与 comptime 值参数共存。实例化策略采用**全单态化**（C++ template 风格）——每个用到的类型实参组合生成一份特化代码，运行期零开销、无 vtable 间接。

设计原则：

- **复用已有基础设施**：类型值已是 clux 的一等值（M2 §1 关键架构决策），CTFE 已具备类型值传参/返回能力（`docs/ctfe-review.md` §2），struct 方法本质是 `T::name` 命名空间成员。泛型是这些既有机制的**自然延伸**，不是另起炉灶。
- **运行期零感知**：单态化在 sema/CTFE 层完成，实例化产物是普通的 `ast_func_def_t` 副本与普通的 `type_t` 实例。compiler/VM 不新增"泛型"概念，只看到具体化的函数与类型。
- **鸭子类型 + 可选结构约束**：不引入 trait/interface/concept。类型参数默认无约束，在函数体内的操作在实例化时按实际类型检查（不支持则报错）。可选地用 `extends` + 通配符（§27）对类型实参做**结构前置过滤**，复用 M2 `extends` 兼容判定的递归解构语义。这与 clux 现有鸭子类型协议（M2 §2 struct 兼容判定、M2 §8 鸭子类型协议）一致。
- **无 borrow checker 全局推断**（M3 §目标）：泛型不引入生命周期参数、不引入跨函数借用求解。所有权约束在实例化时按具体类型复用现有 §5/§6/§7 局部检查。

所有语法决策与补充决策由用户 2026-10-17 逐项确认。

> **实现状态：✅ M7 已完成（2026-10-22）**。P0 前端语法、P1 单态化地基、
> P2 泛型函数、P3 调用点推导 + 默认类型实参、P4 泛型类型
> （struct/enum/union/别名 + 方法 + 递归 struct）、P4b 泛型方法、P4c 静态泛型
> 方法、P5a comptime 值参数、P5b 类型参数包、P5c `<...Args>` 类型展开 +
> 别名/struct 参数包、P6a `extends` 结构约束、P6b `interface`
> （含 `&` 组合与 `...Base[args]` 成员复制）、P7 跨模块泛型实例化
> （含 `export * from` 泛型 reexport）、P9 comptime **值**参数包均已落地。
> 已知边界：跨模块 interface 的 `implements`/约束写法、预编译模块作为
> **泛型定义来源**（`.cxs/.cxb` 只承载已单态化产物）、跨模块实例的诊断
> 位置已重绑 token pool，签名期错误的实例链 note 仍可补强。
> **跨模块实例归属（2026-10-22 已落地）**：两阶段模块流水线——所有模块先
> prepare（保留 sema/prog/funcs/types），跨模块解析走 sema 符号表，全局
> worklist 把实例直接加入定义模块；最后按依赖序 compile/register。类型登记
> 改为全 VM 共享 `sema_types_global`，compiler 全局只 `DEFINE_TYPE` 一次。
> 已删除 `SET_ROOT_SCOPE`/相对 IMPORT 运行时补丁；`.cxb` 往返与全量
> `clux_test`/examples 均通过。实现记录见
> [m7-implementation-plan.md](m7-implementation-plan.md)。
> 实现记录见 [m7-implementation-plan.md](m7-implementation-plan.md)。

---

## M7 范围

| 特性 | 状态 |
|------|------|
| 泛型函数 `func f[T, U](...)` | 纳入 |
| 泛型 struct `struct Vec[T]{...}` | 纳入 |
| 泛型 enum / union | 纳入（名义匹配 + 类型参数递归，§29） |
| 泛型 cunion | **不纳入**（cunion 仅用于 FFI/optional/tag union，不参与泛型体系） |
| 泛型类型别名 `type Pair[T, U] = <T, U>;` | 纳入 |
| 类型参数（`T`, `U`, ...） | 纳入 |
| comptime 值参数（`N: comptime u64`） | 纳入 |
| 调用点类型实参推导（函数调用） | 纳入 |
| 类型上下文显式标注（`var v: Vec[i32]`） | 纳入 |
| 类型参数默认值（`T = i32`） | 纳入 |
| 递归实例化（带深度上限） | 纳入 |
| 泛型 struct 字段使用类型参数 | 纳入 |
| `extends` 结构约束 + 通配符 `?` / `<?>`（§27） | 纳入（可选，默认无约束） |
| `extends` 析取 `|` / 合取 `&` 组合（§28） | 纳入（`&` 优先级高于 `|`，支持括号） |
| `extends` 通配符捕获变量（infer） | **不纳入**（只匹配不绑定） |
| `extends` 重载分派 | **不纳入**（只过滤不分派） |
| 类型约束（where/trait/concept） | **不纳入**（鸭子类型，`extends` 仅结构过滤） |
| `interface` 编译期实体（§31） | 纳入（纯编译期，约束 struct 属性/方法） |
| interface 继承 | **不纳入**（用 `...A;` 成员复制 + `&` 组合替代） |
| interface 运行时多态 / vtable | **不纳入**（纯编译期，无运行时实体） |
| 泛型 cfunc / extern cfunc | **不纳入**（C 无泛型，cfunc 保持非泛型） |
| `comptime func` + 泛型参数组合 | **不纳入**（§26，语义冲突需独立设计） |
| 生命周期参数 / 借用求解 | **不纳入**（与 M3 一致） |
| 泛型特化（partial specialization） | **不纳入** |
| variadic 泛型 / 参数包 `...Args`（§30） | 纳入（**仅编译期可变**，非运行期可变） |
| 参数包值展开 `...args` 转发 | **不纳入**（M7 仅设计语法预留，实现留待后续） |
| 运行期可变参数（C `va_list` 风格） | **不纳入**（M7 仅编译期可变，单态化定长） |

---

## 语法设计决策

### 1. 类型参数语法：方括号 `[T, U]` 前置

类型参数列表紧跟声明关键字与名字之后，用方括号包裹，逗号分隔：

```
func map[T, U](xs: ref []T, f: func(T) -> U): ref []U { ... }
struct Vec[T] { data: *T; len: u64; }
enum Option[T] { Some(T), None }
type Pair[T, U] = <T, U>;
```

**决策依据**（2026-10-17 用户确认）：

- **避免与比较运算符 `<` `>` 冲突**：clux 的 `extends`/`==`/`?:` 类型计算已重度使用 `<` `>`（`type T = A extends B ? i64 : i32`）。若用 `<T,U>`，parser 需在 `func <` 后做回溯区分泛型参数与比较表达式，与现有 Pratt 解析器交互复杂。方括号无此歧义。
- **与 Go 泛型语法一致**：Go 1.18+ 的泛型采用 `func F[T any](...)`，已证明方括号在类 C 语法中可行且无致命歧义。
- **与现有 `[]T` 切片语法的视觉区分**：`Vec[T]` 是具名类型实例化（名字在前），`[]T` 是匿名切片类型（括号在前）——位置不同，parser 按上下文区分（见 §6 parser 改动）。

### 2. comptime 值参数：`N: comptime <type>`

值参数在类型参数列表中用 `: comptime <type>` 显式标注编译期性质：

```
func make_array[T, N: comptime u64](): [N]T { ... }
func fixed_buf[T, CAP: comptime u64](xs: ref []T): [CAP]T { ... }
```

**决策依据**：

- **`comptime` 关键字已存在**（M2 §10 `comptime var` / `comptime func`）：复用同一关键字表达"编译期已知"，语义统一。
- **必须显式标注 comptime**：若仅写 `N: u64`，与普通运行期参数无区分（普通参数也写 `: u64`）。显式 `comptime` 让"这是泛型值参数"在语法层明确，parser/sema 无需推断参数种类。
- **类型限定为编译期可求值的标量**：`comptime u64` / `comptime i32` / `comptime bool` / `comptime str`。不允许 `comptime *T`（指针非编译期常量）、`comptime Vec[T]`（复合值参数暂不支持，见 §11 留待后续）。

### 3. 类型参数默认值：`T = i32`

类型参数可带默认值，调用点可省略带默认值的类型参数：

```
func default_vec[T = i32, N: comptime u64 = 10](): Vec[T] { ... }

default_vec()              // T=i32, N=10
default_vec[f64]()         // T=f64, N=10
default_vec[f64, 20]()     // T=f64, N=20
```

**规则**：

- 默认值从右向左省略（与 C++/Python 默认参数一致）：省略中间参数报错。
- 默认值表达式在**定义点**编译期求值（CTFE），不依赖调用点上下文。
- 默认值可引用更早的类型参数：`func f[T, U = T](...)`。
- 值参数默认值是编译期常量表达式：`func f[N: comptime u64 = sizeof(i32) * 2]()`。

### 4. 调用点推导：仅函数调用，类型上下文必须显式

```
// 函数调用：从实参推导类型参数
func map[T, U](xs: ref []T, f: func(T) -> U): ref []U { ... }

var arr: [3]i32 = .{1, 2, 3};
var doubled = map(arr[:], func(x: i32): i32 { return x * 2; });
// 推导：xs: ref []T = ref []i32（arr[:] 切片化） → T=i32（切片元素类型）
//        f: func(T)->U = func(i32)->i32 → U=i32
// 结果类型：ref []i32

// 类型上下文：必须显式标注
var v: Vec[i32] = Vec[i32]::new();    // 显式 Vec[i32]
var p: Pair[i32, str] = .Pair{1, "x"}; // 显式 Pair[i32, str]
```

**决策依据**（2026-10-17 用户确认）：

- **函数调用推导**：从实参类型反向推导类型参数，与 C++ template argument deduction 一致。推导失败（歧义/无法确定）时要求显式标注：`map<i32, f64>(arr, ...)`。
- **类型上下文不推导**：`var v: Vec[T] = ...` 中的 `T` 不从初始化值推导。理由：clux 的 `var v: T = expr` 声明侧类型是**约束**而非推断目标（M2 §3.4 类型推断规则），泛型类型实例化必须由用户显式指定，避免"初始化值决定容器元素类型"的隐式行为。
- **推导只作用于函数调用**：包括普通调用 `f(args)`、方法调用 `recv.m(args)`（接收者类型已知，方法类型参数从 args 推导）、构造调用 `Type::method(args)`。

#### 4.1 所有权的不对称推断（2026-10-22 补充）

调用点推导在形参类型模式与实参类型**逐节点对位**时，所有权修饰按不对称规则归一：

- `fatal *T` 实参可满足 `own *T` 形参（将亡值被接收为 own，调用方交出所有权）；
- `own *T` / `fatal *T` 实参可满足 `ref *T` 形参（借用；`ref` 形参不取得所有权）；
- 反向不成立：`own` / `ref` 实参不能流向 `fatal` 形参（fatal 只描述调用表达式的值）；
- `ref` 形参接受 `own`/`fatal` 实参时，推导出的类型实参**不含**形参侧的所有权修饰：
  `func f[T](p: own *T)` 以 `move(x)`（`fatal *i32`）调用 → `T = i32`，`own` 只是
  形参的接收能力，不是 T 的一部分。

这与值层 `value_implicit_cast` 的所有权阶梯（`FATAL → OWN → REF → NONE`）一致；
约束 `extends` 是**类型身份**判断，不参与这条不对称规则（见 §29.1）。

### 5. 泛型 struct 方法：接收者显式写完整泛型类型

```
struct Vec[T] {
    data: *T;
    len: u64;

    func new(self: ref *Vec[T]): Vec[T] { ... }          // 构造（静态方法）
    func push(self: ref *Vec[T], v: T): void { ... }     // 接收者 + 值参数都用 T
    func get(self: ref *const Vec[T]): T { ... }         // 只读方法
    func len(self: ref *const Vec[T]): u64 { ... }       // 返回具体类型
}
```

**决策依据**（2026-10-17 用户确认）：

- **与现有 struct 方法机制一致**：M2 struct 方法本质是 `T::name` 命名空间成员（`struct_members.cx` 示例），接收者是第一个参数（`self` 不是关键字）。泛型方法只是接收者类型写成 `ref *Vec[T]` 而非 `ref *Vec`，sema 在实例化 `Vec[i32]` 时把 `Vec[T]` 的方法注册为 `Vec[i32]::push` 等。
- **不引入 `Self` 关键字**：`Self` 会引入"方法所属类型的实例化形态"这一隐式作用域规则，与 clux"显式优于隐式"的风格不符。显式写 `Vec[T]` 虽冗余但无歧义。
- **方法自身的类型参数**：方法可额外声明自己的类型参数（与 struct 的 T 独立）：
  ```
  struct Vec[T] {
      func map_into[U](self: ref *const Vec[T], f: func(T) -> U): Vec[U] { ... }
  }
  ```
  调用 `v.map_into(func(x: i32): f64 { return x as f64; })` 推导 `U=f64`，`T` 从接收者 `v: Vec[i32]` 已知。

### 6. 泛型类型别名

```
type Pair[T, U] = <T, U>;
type IntVec = Vec[i32];                    // 部分应用（固定 T=i32）
type Matrix[T, N: comptime u64] = [N][N]T; // 嵌套泛型
type Result[T, E] = union { Ok(T), Err(E) };
```

**规则**：

- 泛型别名是**编译期类型计算函数**：`Pair[i32, str]` 在 CTFE 层求值为 `<i32, str>` 元组类型。
- **部分应用**：`type IntVec = Vec[i32]` 固定部分参数，产生非泛型别名。`type Mat64[T] = Matrix[T, 64]` 固定部分参数，产生剩余参数的泛型别名。
- 别名展开在 sema 类型槽位解析时发生，**不产生新的 type_t 实例**（展开到具体类型后走正常 intern）。这与 M2 §5"type 别名 = 类型计算表达式"一致——别名不是新类型，是 type value 绑定。
- 别名不能递归引用自身（`type A[T] = A[*T]` 非法），避免无限展开。

### 7. 递归实例化：允许 + 深度上限

```
// 合法：递归实例化（深度受上限保护）
func recurse[T](depth: u64): void {
    if (depth == 0) { return; }
    recurse[*T](depth - 1);    // 递归实例化（类型参数变化）
}
```

**规则**：

- 允许泛型函数直接或间接实例化自身（类型参数可变化）。
- 实例化深度上限默认 **128**（与 CTFE `max_depth` 一致，`docs/ctfe-review.md` §3-C 建议复用同一套保护）。
- 超限报错：`generic instantiation depth exceeds limit (128): recurse[*T] -> recurse[**T] -> ...`，诊断需展示实例化链（见 §10 诊断）。
- **mangling 先查缓存**：每次实例化前先计算 mangling，查实例化缓存命中则直接复用，不重复展开。`recurse[*T]` 与 `recurse[*T]` 第二次出现命中缓存，不重复实例化——只有**类型参数不同**的递归（`recurse[*T]` → `recurse[**T]` → `recurse[***T]`）才真正展开。

---

## 语义规则

### 8. 实例化时机：惰性、按需

泛型定义本身**不产生任何运行时实体**。实例化在**第一次被使用**时发生（惰性）：

- 泛型函数：在**调用点**或**取地址点**（`var f = map;`）实例化。
- 泛型类型：在**类型槽位解析**时实例化（`var v: Vec[i32]`、函数签名 `func(x: Vec[i32])`、struct 字段 `data: Vec[*T]` 在外层实例化时）。
- 泛型别名：在**别名引用**时展开（`var p: Pair[i32, str]` → CTFE 求值 `<i32, str>`）。

**实例化粒度**：

- 函数：每个 `(泛型定义, 类型实参组合)` 一份实例，生成独立的 `ast_func_def_t` 副本（类型槽位替换为 `AST_TYPE_REF`，见 CTFE review §3-D）+ 独立的 `func_t`（独立 fid、独立 entry_pc）。
- 类型：每个 `(泛型定义, 类型实参组合)` 一份 `type_t` 实例，经 intern 去重（按字段表 + 实参去重，与现有 struct intern 一致）。
- 别名：不产生实例，CTFE 求值到具体类型后直接走类型槽位。

### 9. 单态化产物：AST 副本 + 类型替换

实例化的核心是**生成一份类型参数已替换的 AST 副本**：

```
// 泛型定义（未实例化）
func map[T, U](xs: ref []T, f: func(T) -> U): ref []U {
    var out: [xs.len]U = .{};
    for (i = 0; i < xs.len; i += 1) {
        out[i] = f(xs[i]);
    }
    return out;
}

// 实例化 map[i32, f64]（T=i32, U=f64）
// → 生成 ast_func_def_t 副本，类型槽位替换：
func map$i32$f64(xs: ref []i32, f: func(i32) -> f64): ref []f64 {
    var out: [xs.len]f64 = .{};
    for (i = 0; i < xs.len; i += 1) {
        out[i] = f(xs[i]);
    }
    return out;
}
```

**替换规则**：

- **类型槽位**：AST 中所有引用类型参数 `T` 的位置（参数类型、返回类型、局部变量类型、字段类型、嵌套类型表达式的子类型）替换为对应的 `AST_TYPE_REF`（指向具体类型）。
- **comptime 值参数**：所有引用 `N` 的位置（数组边界 `[N]T`、条件分支 `if N > 0`、运算 `N * 2`）替换为对应的字面量 AST 节点（`AST_INT_LITERAL` 等）。
- **函数体共享 vs 深拷贝**：优先**深拷贝**函数体（避免实例间 AST 节点共享导致的 sema 状态污染——如 `flow_init`/`moved` 等符号状态）。内存开销可接受（实例数受使用场景约束，且有缓存去重）。
- **mangling 名**：实例名 = `原名$T1$T2$...`（如 `map$i32$f64`、`Vec$i32`）。mangling 是 sema 层的**唯一标识**，不暴露给用户（用户代码写 `map`、`Vec[i32]`，sema 内部映射到 mangling 名）。

### 10. 鸭子类型检查：实例化时

泛型函数体在**实例化后**走正常的 sema 类型检查：

```
func add[T](a: T, b: T): T {
    return a + b;    // 要求 T 支持 + 运算
}

add[i32](1, 2);        // ✓ i32 支持 +
add[str]("a", "b");    // ✗ str 不支持 +（编译期报错，指向实例化点）
```

**错误诊断**（CTFE review §3-E）：

- 报错需同时指出**泛型定义点**与**实例化点**：
  ```
  error: cannot apply '+' to values of type str
   --> generics.clux:2:12
    |
  2 |     return a + b;    // 要求 T 支持 + 运算
    |            ~~~~~
    |
   instantiated from:
     add[str]("a", "b")
   --> app.clux:10:5
    |
  10|     add[str]("a", "b");
    |     ^~~~~~~~~~~~~~~~~~
  ```
- 实例化上下文记入诊断结构（不是拼字符串），支持多层嵌套实例化链。

### 11. 所有权与泛型的交互

实例化后类型参数替换为具体类型，所有权走现有 M3 规则——**无新增**：

- `func take[T](p: own *T): void { ... }` 实例化 `take[i32]` 后，`p: own *i32` 走 §3.1 own 不可 copy 规则。
- `struct Vec[T] { data: own *T; }` 实例化 `Vec[i32]` 后，`data: own *i32` 走 struct 含 own 字段的 clone/move 规则（M3 §7）。
- `func borrow[T](p: ref *T): ref *T { return p; }` 实例化后走 §6 返回 ref 来源检查。
- **泛型参数本身不携带所有权**：`T` 是任意类型，实例化时才确定是否是指针/own/ref。泛型定义内对 `T` 的操作必须对**所有合法实例类型**成立（鸭子类型）——若 `func f[T](x: T) { x.*; }` 要求 T 是指针，则 `f[i32]` 实例化时报错（`i32` 无 `.*` 操作）。

### 12. 作用域与可见性

- **类型参数作用域**：类型参数在声明体内可见（函数体 / struct 体 / enum 体 / 别名右值）。
- **类型参数遮蔽**：类型参数名遵循普通标识符遮蔽规则——内层声明同名类型参数遮蔽外层：
  ```
  func outer[T](x: T): void {
      var inner = func[T](y: T): T { return y; };  // 内层 T 遮蔽外层 T（独立参数）
      _ = inner(x);
  }
  ```
- **跨声明引用**：类型参数不能跨声明引用——`func f[T](x: T)` 的 `T` 在 `func g(y: T)` 中不可见（`g` 需声明自己的 `T`）。
- **泛型定义可见性**：泛型函数/类型遵循普通名字可见性（全局/局部/模块 import）。泛型定义可被 import 后实例化（模块间泛型共享）。

---

## 关键架构决策

### 13. 实例化缓存与去重

实例化缓存挂在 **sema**（与 `sema->funcs` / `sema->types` 并列），CTFE 只负责求值类型实参：

```
sema->generic_instances: map<mangling, instance_entry>
    mangling = "map$i32$f64" / "Vec$i32" / ...
    instance_entry = {
        kind: FUNC / STRUCT / ENUM / UNION / CUNION / ALIAS,
        def: 原始泛型定义 AST 节点（借用）,
        type_args: [type_t* / ct_value]（实参列表）,
        instance: ast_func_def_t* / type_t*（实例化产物）,
        instantiating: bool（实例化在途标记，用于环检测）,
        depth: u32（实例化深度，用于递归保护）
    }
```

**决策依据**（CTFE review §3-B）：

- sema 拥有实例的生命周期（实例随 sema 销毁）。
- CTFE 只负责**求值类型实参**（`Vec[i32+sizeof(x)]` 这类复杂类型表达式 → 具体 `type_t*`），不持有实例表。
- mangling 是天然锚点：`sema_type_t::name` 已是 `"__type_N"` 形式，泛型实例的 mangling 可复用此命名空间（`__type_N` 之外加 `$T1$T2` 后缀，或单独的 `__gen_<hash>` 命名段）。

### 14. 实例的 AST 表示：替换后的 ast_func_def_t 副本

实例不新增 AST 节点种类，表示为**类型槽位替换后的 `ast_func_def_t` 副本**（CTFE review §3-D）：

- 函数实例：`ast_func_def_t` 副本，params/return_expr/body 内的类型槽位替换为 `AST_TYPE_REF`，name 改为 mangling 名。
- 类型实例（struct/enum/union/cunion）：在 vm 层经 `type_*_intern` 构造新的 `type_t` 实例（字段表按替换后的类型重新布局），登记进 sema->types。
- 别名实例：无 AST 副本，CTFE 求值到具体 `type_t*` 后直接用于类型槽位。

**深拷贝策略**：

- 函数体 AST 深拷贝（避免 sema walk 状态污染）。
- 类型实例不涉及 AST 深拷贝（直接构造 `type_t`）。
- arena 分配：实例 AST 副本分配在 sema arena（随 sema 销毁，与现有 AST 同生命周期）。

### 15. 实例化的流水线位置

实例化发生在 **sema Pass 3b（walk 阶段）**，具体在**类型槽位解析**与**函数调用点检查**时：

```
sema 流水线：
  pass1_names      → 名字注册（含泛型定义的签名预登记）
  pass1b_types     → 类型定义求值（struct/enum/union/别名）
  speculative_pass → 函数签名投机解析
  pass2_types      → 函数签名正式解析
  sema_build_scope_tree (3a) → 作用域树 + 符号注册
  sema_walk (3b)   → 遍历函数体，此处触发实例化：
    - 类型槽位解析到 Vec[i32] → 查缓存/实例化 → 登记类型
    - 函数调用 map(args) → 推导类型实参 → 查缓存/实例化 → 生成调用
    - 取地址 var f = map → 推导（或要求显式标注）→ 实例化 → 注册函数值
```

**实例化递归**：实例化 `map[i32, f64]` 产生新的 `ast_func_def_t` 副本，该副本需走 sema walk（类型检查其函数体）——这会递归触发其内部的实例化。递归深度受 §7 上限保护。

### 16. compiler / VM 零感知

实例化完成后，产物是普通的函数与类型：

- **compiler**：看到的是 `map$i32$f64`（普通函数名）与 `Vec$i32`（普通类型，经 `sema_type_register` 分配了 id）。hoist 区按正常流程发 `PUSH_FUNC_TYPE` / `DEFINE_TYPE` / 字段定义 / `SEAL`；函数体按正常流程编译。compiler **不知道**这是泛型实例。
- **VM**：看到的是普通的 `func_t`（独立 fid/entry_pc）与普通的 `type_t`（独立 id/字段表）。运行期**无泛型概念**，无 vtable 间接、无类型描述符字典、无动态分派。
- **字节码**：实例的字节码与手写的非泛型版本逐字节一致（`map[i32, f64]` 的字节码 = 手写 `func map_i32_f64(xs: ref []i32, f: func(i32)->f64): ref []f64 { ... }` 的字节码）。

---

## mangling 方案

### 17. mangling 规则

mangling 是 sema 内部的唯一标识，**不暴露给用户**。规则：

```
<原名>$<实参1>$<实参2>$...

实参编码：
  类型实参 → 类型的规范显示名（type->name，如 "i32" / "*i32" / "[3]i32"）
            嵌套类型递归编码（Vec[i32] → "Vec$i32"，整体作为实参时 → "Vec$i32"）
  值实参   → 值的规范十进制表示（N=10 → "10"，N=sizeof(i32) → "4"）
            布尔 → "true"/"false"，字符串 → 转义后的字面量
```

示例：

| 泛型定义 | 实参 | mangling |
|---|---|---|
| `func map[T,U](...)` | `T=i32, U=f64` | `map$i32$f64` |
| `struct Vec[T]` | `T=i32` | `Vec$i32` |
| `func make_array[T,N: comptime u64]()` | `T=i32, N=10` | `make_array$i32$10` |
| `func f[T](x: *T)` | `T=*i32` | `f$*i32` |
| `struct Mat[T,N: comptime u64]` | `T=f64, N=4` | `Mat$f64$4` |
| `type Pair[T,U]` | `T=i32, U=str` | `Pair$i32$str`（别名展开后无需 mangling） |

**mangling 碰撞**：

- 类型名可能含 `$`（若用户类型名含 `$`）——clux 标识符不允许 `$`（M1 词法规则），故无碰撞。
- 嵌套实例 `Vec[Vec[i32]]` → `Vec$Vec$i32`，层次清晰。

---

## 错误诊断

### 18. 诊断可定位性

实例化错误需展示**实例化链**（CTFE review §3-E）：

```
error: cannot apply '+' to values of type str
 --> lib.clux:2:12
  |
2 |     return a + b;
  |            ~~~~~
  |
 instantiated from: add[str]
 --> app.clux:5:5
  |
5 |     add[str]("a", "b");
  |     ^~~~~~~~~~~~~~~~~~
```

**实现要点**：

- sema 在 walk 实例化产物时，维护一个**实例化栈**（`sema->instantiation_stack`），每层记录 `(定义点 loc, 实参列表, 调用点 loc)`。
- 诊断产生时，沿栈展开输出实例化链。
- 栈在实例化开始时 push、结束时 pop（与 §15 实例化递归同步）。

### 19. 推导失败的诊断

```
func map[T, U](xs: ref []T, f: func(T) -> U): ref []U { ... }

// 推导失败：xs 是 i32（非切片），无法推导 T
map(42, func(x: i32): i32 { return x; });

// 报错：
error: cannot deduce type argument T for map[T,U]
 --> app.clux:3:5
  |
3 |     map(42, func(x: i32): i32 { return x; });
  |     ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 note: parameter 'xs' expected ref []T, got i32
 note: add explicit type arguments, e.g. map<i32, i32>(...)
```

---

## 与现有机制的交互

### 20. 与 CTFE 的交互

泛型类型实参的求值**复用 CTFE**：

- `Vec[i32 + sizeof(x)]` 这类复杂类型实参表达式经 CTFE 求值为具体 `type_t*`。
- comptime 值参数（`N: comptime u64`）的实参表达式同样经 CTFE 求值。
- CTFE 的 budget / max_depth 保护**复用于实例化层的求值**（不是实例化深度——实例化深度有独立上限，见 §7）。

### 21. 与 struct 方法（M5/M6 运行时成员模型）的交互

泛型 struct 的方法是 `struct_type_t::attributes` / `methods` 的成员。实例化 `Vec[i32]` 时：

- 构造 `struct_type_t` 实例（`Vec$i32`，字段表按 `T=i32` 替换后布局）。
- 方法的 `ast_func_def_t` 副本实例化（`push` → `push$i32`），注册到 `Vec$i32` 的 `attributes` / `methods`。
- 方法调用 `v.push(1)`（`v: Vec[i32]`）经现有 `MEMBER_CALL` 机制分派到 `Vec$i32::push`。

### 22. 与 FFI 的交互

- **泛型 cfunc 不支持**（2026-10-17 用户确认）：cfunc 是 C ABI 桥接，C 无泛型。
- **泛型函数内调用 FFI**：允许。实例化后函数体内的 `extern cfunc` 调用按具体类型走正常 FFI 路径。
- **FFI 回调接收泛型函数**：不允许——`cfunc_ptr(map)` 中 `map` 是泛型定义，无具体 fid。必须先实例化：`cfunc_ptr(map[i32, i32])` 传递实例化的具体函数。

### 23. 与模块系统（M5）的交互

- 泛型定义可 `export`：`export func map[T,U](...)` / `export struct Vec[T]{...}`。
- import 后实例化：`import lib; var v: lib::Vec[i32];` ——实例化在**使用方模块的 sema** 完成，实例化产物登记进使用方的 sema->types/funcs（与现有跨模块类型引用走 `GET_MEMBER` 一致，见记忆 `cross-module-type-value-via-get-member-not-fold`）。
- **潜在代码膨胀**：若两个模块都实例化 `Vec[i32]`，各自 sema 各有一份实例。这是单态化的固有代价，与 C++ 静态库/动态库的 template 实例重复一致。后续可考虑链接期去重（不在本轮范围）。

---

## 补充决策（2026-10-17 定稿）

### 24. 类型参数在类型计算表达式中的作用域穿透

泛型定义体内定义的局部类型别名（`type` def）右值**可引用外层类型参数**——类型参数作用域是整个声明体（§12），type def 在体内定义，右值在体内求值，T 自然可见。

```
struct S[T] {
    type U = Vec[T];          // U 的右值引用外层 T
    data: U;
}

func f[T]() {
    type W = *T;               // 函数体内局部别名引用 T
    var x: W = ...;
}
```

实例化 `S[i32]` 时，体内 `type U = Vec[T]` 的右值 `Vec[T]` 替换为 `Vec[i32]`，`U` 成为 `S$i32` 实例的局部类型别名（不产生新 type_t，展开到 `Vec[i32]` 走正常 intern）。

**规则**：

- **嵌套泛型别名**：`struct S[T] { type U[V] = Vec[V]; ... }` —— 内层 `V` 是 `U` 自己的参数，`T` 也能在右值引用（外层可见）。遮蔽规则同 §12（内层 V 遮蔽外层同名）。
- **类型计算表达式中的引用**：`type W = Vec[T] extends Vec[*T] ? i64 : i32;` —— T 在 extends 左右两侧都可见，整个右值在实例化时按 T 的具体值 CTFE 求值。
- **跨声明不可见**（§12 已规定）：`func g[T]()` 体内的 `type W = *T;` 在 `func h()` 中不可见。

### 25. 泛型函数的函数值类型

**未实例化的泛型定义只是符号，不是值**——无具体签名、无 fid/entry_pc，无法分配 `func_t`。clux 的函数值类型是具体签名 `func(i32)->i32`，未实例化的 `map` 签名是 `func(∀T,U)(ref []T, func(T)->U)->ref []U`——这种多态签名不是 clux 类型系统能表达的类型。

**只有实例化后才是实体**：

```
var f = map;                    // ✗ 未实例化，只是符号不是值
var f: func(?)->? = map;        // ✗ 即使有类型上下文也不推导（与 §4 一致）
var f = map[i32, f64];          // ✓ 实例化后是普通函数实体，可取地址
```

**规则**：

- **作函数参数**：`func apply(g: ???) { ... }` 传 `map` 不允许。必须传实例 `map[i32,f64]`，参数类型写具体签名 `func(ref []i32, func(i32)->f64)->ref []f64`。
- **高阶泛型场景的替代方案**：用泛型函数包裹——
  ```
  func apply_map[T, U](g: func(ref []T, func(T)->U)->ref []U, xs: ref []T, f: func(T)->U): ref []U {
      return g(xs, f);
  }
  // 调用：apply_map(map[i32, f64], arr, func(x: i32): f64 { ... });
  ```
  `g` 是普通函数值（具体签名），调用者传实例 `map[i32,f64]` 进来。不支持"传未实例化 map 进去再在 apply_map 内部推导 map 的类型参数"。
- **取地址点实例化**：`var f = map[i32, f64];` 是"取地址点"（§8 已规定实例化时机含取地址点），实例化后 f 是普通函数值。

### 26. 泛型定义的 comptime 性质

**统一原则：单态化这一行为发生在编译期**。泛型定义的"类型参数化"是编译期概念——实例化是编译期动作，定义体在实例化前不产生运行时实体（无 fid/id，不进 sema->funcs/types）。实例化后的实例是普通运行时函数/类型，运行时执行。`map[i32,f64](arr, f)` 是运行时调用，**不**编译期折叠。

**关键区分**：

| | 生成时机 | 执行时机 |
|---|---|---|
| 泛型实例 `map[i32,f64](...)` | 编译期（单态化） | 运行时 |
| `comptime func fib(n)` 调用 `fib(5)` | 编译期 | 编译期（CTFE 折叠为字面量） |

即：泛型实例 = 编译期生成 + 运行时执行；comptime func = 编译期生成 + 编译期执行。两者正交。

**规则**：

- 类型实参的求值是 comptime（复用 CTFE，§20），与"单态化发生在编译期"一致——实参必须是编译期已知的类型/值。
- **泛型函数体内调用 comptime func**：允许。实例化后函数体内的 `comptime func` 调用按正常 comptime func 规则（编译期折叠）。
- **`comptime func` + 泛型的组合**（`comptime func fib[T](n: T): T { ... }`）：**M7 不纳入**。sema 在声明点拒绝 `comptime` 与泛型参数列表的组合（报错：`comptime func cannot be generic in M7`）。理由：comptime func 的调用点编译期折叠与泛型的运行时执行语义冲突，组合的语义（实例后编译期折叠 vs 运行时执行）需独立设计，超出本轮范围。
- **泛型类型的 comptime 性质**：泛型类型 `Vec[T]` 在实例化前无 type_t/id（不进 sema->types）；实例 `Vec[i32]` 是普通类型，有 id，可 `LOAD_TYPE`，运行时正常使用。与泛型函数同构。

---

## 约束机制：`extends` 结构约束 + 通配符

### 27. `extends` 结构模式匹配与通配符 `?` / `<?>`

类型参数可带**可选的 `extends` 约束**，对实例化时的类型实参做**结构前置过滤**。约束机制复用 M2 `extends` 的结构兼容判定语义（鸭子类型兼容，非名义子类型），并引入两个通配符：

- **`?`** = 任意单类型通配符（type wildcard），匹配任意具体类型。
- **`<?>`** = 任意元数元组通配符，匹配 `<T>` / `<T,U>` / `<T,U,V>` / ... 任意元数的元组。

```
func f[T extends *?](p: T) { ... }              // T 必须是指针
func g[T extends [?]?](a: T) { ... }            // T 必须是任意数组
func h[T extends <?>](t: T) { ... }             // T 必须是元组（任意元数）
func k[T extends ?](x: T) { ... }               // 显式无约束（等价省略 extends）
```

**匹配规则（递归向下解构）**：

`T extends PATTERN` 把 T 的具体类型与 PATTERN 作为两棵类型树，**递归向下逐节点对位匹配**：

- PATTERN 节点是 `?` → 该位置任意匹配（递归终止，通过）。
- PATTERN 节点是具体类型构造（`*` / `[]` / `[N]` / `<>` / `Vec[...]` 等）→ T 必须是同构构造，且子节点递归匹配。
- PATTERN 节点是具体类型（`i32` / `str` 等）→ T 必须名义相等。
- 构造不同（如 PATTERN=`*?` 而 T=`[3]i32`）→ 不通过。

`?` 可出现在 PATTERN 的**任意深度**，匹配该位置的任意类型——递归向下自然解析到结构内部的局部通配符：

```
T extends ?              // 恒为通过（显式无约束）
T extends *?             // T = *X（X 任意）：pointee 通配
T extends *(*?)          // T = **X（X 任意）：双层指针，内层通配
T extends []?            // T = []X（X 任意）：切片，元素通配
T extends [?]?           // T = [N]X（N、X 任意）：数组，长度+元素都通配
T extends [?]i32         // T = [N]i32（N 任意）：长度通配，元素固定
T extends <?>            // T = <X1, X2, ...>（元数+元素都任意）
T extends <i32, ?>       // T = <i32, X>：首元素固定，次元素通配
T extends <?, ?>         // T = <X, Y>：二元组，两元素都通配
T extends Vec[?]         // T = Vec[X]：Vec 实例，元素通配
T extends *[]?           // T = *([]X)：指针指向切片，元素任意
T extends ?*?            // T = 任意**非 RC**所有权指针（own/ref/fatal *X；见下）
T extends share *?       // T = share *X（RC 必须显式，?*? 不兜住）
T extends weak *?        // T = weak *X
```

**所有权通配 `?*?`（2026-10-22 补充）**：

- PATTERN 中 `*` 的**所有权位置**可以写 `?`（`?*?`），表示"任意非 RC 的所有权
  修饰"——匹配 `own *T` / `ref *T` / `fatal *T` 三种指针；
- **不包括 RC 系**：`share *T` / `weak *T` 必须显式写 `share *?` / `weak *?`，
  `?*?` 不匹配它们（RC 是 `TYPE_KIND_PTR` 的 rc 形态，属于独立类型身份）；
- `?` 只做匹配，不捕获所有权修饰（不引入 infer 绑定，与 §27"不捕获"一致）；
- 裸 `*?`（无所有权前缀）只匹配"无所有权修饰的指针"——源码层不存在这种
  值类型写法，实际表达"任意所有权指针"请用 `?*?`。

**与"无约束"的关系**：

- **省略 `extends` 等价于 `extends ?`**——约束可选，不强制写。原"无约束（鸭子类型）"决策保持：类型参数默认无约束，函数体操作在实例化时按实际类型检查（§10）。
- `extends` 只是对类型实参做**结构前置过滤**：实例化时先匹配 PATTERN，不通过则报错（不进入函数体检查）。

**通配符的合法位置**：

- **`?` / `<?>` 仅在 `extends` 约束 PATTERN 中合法**。
- 普通类型表达式（`var x: T`、函数签名、字段类型、struct 字段、comptime 值参数类型）**不允许** `?`——具体类型必须是具体类型。
  - `func f[T extends [?]i32](a: T)` ✓（约束模式里用 `?`）
  - `var x: [?]i32 = ...` ✗（值类型不能用 `?`，没有具体大小）
  - `func f(a: [?]i32)` ✗（参数类型不能用 `?`）

**不引入的能力（M7 边界）**：

- **不捕获**：`?` 只"匹配/不匹配"，**不把匹配到的类型绑定为新名字**（无 `*U` 把 pointee 命名为 U 的 infer 能力）。需要 pointee 作类型参数时，直接声明独立参数：`func f[T, U extends *?](p: *U, x: T)` 中 U 是独立参数而非从 T 捕获。
- **不参与重载分派**：`extends` 只过滤，不分派。同名同元数泛型 = 重复定义报错（与现有规则一致），不允许靠 `extends` 不同的候选做 overload resolution：
  ```
  func f[T extends *?](p: T) { ... }       // ✗ 与下面冲突
  func f[T extends [?]?](a: T) { ... }     // ✗ 不允许靠 extends 分派
  ```
- **comptime 值参数不写 `extends`**：`extends` 仅用于类型参数。comptime 值参数 `N: comptime u64` 不写 `extends`（值域约束超出本轮，后续再说）。

**错误诊断**：

约束不通过时，报错指向实例化点与约束声明点，展示 PATTERN 与实际类型：

```
error: type argument 'str' does not satisfy constraint 'T extends *?'
 --> app.clux:5:5
  |
5 |     f[str](&p);
  |     ^^^^^^^^^^^
 note: constraint declared here
 --> lib.clux:1:11
  |
1 | func f[T extends *?](p: T) { ... }
  |           ^~~
 note: 'str' is not a pointer type
```

诊断记入实例化栈（§18），与实例化链一同展开。

### 28. `extends` 组合：析取 `|` 与合取 `&`

单个泛型定义内部，对同一类型参数的多个 `extends` 子模式可用 `|`（析取/OR）与 `&`（合取/AND）组合：

- **`extends A | B`**：T 匹配 A **或** B 任一即通过。
- **`extends A & B`**：T 必须**同时**匹配 A 和 B。

```
func f[T extends *? | [?]?](x: T) { ... }     // T 是指针 或 任意数组
func g[T extends *? & <?>](x: T) { ... }       // T 同时是指针和元组（通常无解，但语法合法）
func h[T extends *? | []? | <?>](x: T) { ... } // T 是指针 或 切片 或 元组
func k[T extends <*? & []?>](x: T) { ... }      // T 是二元组，首元是指针且次元是切片
```

**这是单个泛型定义内部的结构匹配组合**，不是多个候选之间的重载分派——仍是"一个 PATTERN 一次匹配"（与 §27"不参与重载分派"边界不冲突）。

**优先级与括号**：

- **`&` 优先级高于 `|`**（与布尔运算一致）：`A | B & C` = `A | (B & C)`。
- **支持括号**显式分组：`(A | B) & C`、`<(*? | []?) & ?>` 等。
- 通配符 `?` / `<?>` 作为原子 PATTERN 参与组合。

**组合的递归匹配**：

`|` 与 `&` 是 PATTERN 树上的逻辑节点，递归向下解构匹配照常进行：

- `T extends A | B` → 递归匹配 A，若不通过再递归匹配 B，任一通过即整体通过（短路：第一个通过即停止）。
- `T extends A & B` → 递归匹配 A 和 B，两者都通过才整体通过（短路：第一个不通过即停止，不继续匹配后续）。
- 括号内子表达式作为子 PATTERN 树参与递归。

**诊断**：

- **`|` 失败**：展示所有分支都不匹配。
  ```
  error: type argument 'str' does not satisfy constraint 'T extends *? | [?]?'
   note: 'str' matches neither '*?' (not a pointer) nor '[?]?'
  ```
- **`&` 失败**：展示第一个不通过的分支（短路）。
  ```
  error: type argument 'i32' does not satisfy constraint 'T extends *? & <?>'
   note: 'i32' does not match '*?' (not a pointer)
  ```

**语法合法性**：

- 不允许空组合：`extends |` 或 `extends &` 非法（至少需要两个子 PATTERN）。
- 单元素括号等价：`extends (A)` = `extends A`。

### 29. `extends` 特殊匹配规则

`extends` 递归向下解构匹配（§27）在以下类型构造上有特殊规则：

#### 29.1 限定符与所有权按类型身份匹配

`const` 与所有权修饰符（`own` / `ref` / `fatal`）都是**类型构造**：`const T` 与 `T`、`own *T` / `ref *T` / `*T` 在类型系统中都是**不同的类型**（各自 intern）。`extends` 在 PATTERN 与 T 的对应节点上按**结构身份**匹配——PATTERN 带限定符时 T 必须带同一个限定符，PATTERN 不带时 T 也不带。**不引入协变/逆变**：`const i32` 能赋给 `i32` 是值层的 const 规则，与约束判断无关；`extends` 判断的是类型身份，不是赋值兼容性。

- **`const`**：`T extends const ?` 只匹配 const 类型；`T extends ?` 匹配任意完整类型（含 const）。
- **所有权**：`own *T` / `ref *T` / `*T` 是三个独立类型。`T extends *?` 只匹配**裸指针**；要匹配带所有权的指针必须显式写 `own *?` / `ref *?` / `fatal *?`。
- **`?` 是整棵类型的通配符**：`*?` 里的 `?` 匹配被指向的任意完整类型（含 `const i32`、`own *i32`）；`own *?` 已固定指针节点的所有权，`?` 只通配 pointee。
- **限定符嵌套按节点顺序匹配**：`own const *T` 与 `const own *T` 是两棵不同的类型树，PATTERN 必须逐层同构；`type_qualifier_sub` 只用于剥限定链，不改变匹配的严格性。
- **所有权通配 `?*?`**：`*` 前可以写 `?` 表示任意非 RC 的所有权修饰（`own` / `ref` / `fatal`），见 §27；RC 系必须显式 `share *?` / `weak *?`。`?` 只匹配不绑定，不产生类型实参。
- **约束匹配 ≠ 调用点所有权推断**：`T extends own *?` 要求**类型实参本身**是 `own *T`；把 `fatal *T` 值传给 `own *T` 形参属于值层的不对称接收（§4.1），不会让 `fatal *T` 满足 `own *?` 约束。两者不要混用。
- **`share` / `weak` 是 `TYPE_KIND_PTR` 的 rc 形态**（0 普通 / 1 share / 2 weak），属于指针类型身份：`*?` 只匹配**普通指针**（rc=0），`share *?` / `weak *?` 分别只匹配对应 RC 变体。PATTERN 本身就是类型表达式，这三种写法天然可用。
- **`volatile` 在类型解析阶段已折叠为 sub**（m2-design §10），不会出现在匹配用的类型图里——它对匹配自然透明，不是特殊规则。

示例：

```
func f[T extends const ?](x: T) { ... }
f[const i32](c);    // ✓ const i32 是 const 类型
f[i32](v);          // ✗ i32 不是 const 类型（没有 const 协变）

func g[T extends *?](p: T) { ... }
g[*i32](mp);        // ✓ 裸指针
g[*const i32](cp);  // ✓ 指针节点无所有权修饰；? 匹配 const i32
g[own *i32](op);    // ✗ own *i32 的指针节点是 own，不是裸 *
g[ref *i32](rp);    // ✗
g[share *i32](sp);  // ✗ rc=1，不是普通指针（要匹配 RC 变体需写 share *?）

func h[T extends own *?](p: T) { ... }
h[own *i32](op);    // ✓
h[*i32](mp);        // ✗ 裸指针不是 own *T
h[ref *i32](rp);    // ✗

func k[T extends *const ?](p: T) { ... }
k[*i32](mp);        // ✗ ? 要求 const i32
k[*const i32](cp);  // ✓
```

#### 29.2 struct 只检查字段表

`T extends StructPattern` 当 T 是 struct 时，只匹配**字段表**（字段名 + 字段类型 + 字段顺序，复用 M2 `struct_type_compatible` 的字段表递归匹配），**不检查** `attributes`（静态属性）与 `methods`（方法）。

```
struct Point { x: i32; y: i32; }
struct Pixel { x: i32; y: i32; color: u32; func dump(self: ref *const Pixel): void {...} }

func f[T extends Point](p: T) { ... }
f[Point](p);      // ✓ Point extends Point 通过（字段表完全相等）
f[Pixel](px);     // ✗ Pixel extends Point 不通过（字段数不同：3 ≠ 2）
                   //    ——Pixel 的 dump 方法不参与匹配（即使字段表匹配也不看方法）
```

**字段表完全相等**（与 M2 `struct_type_compatible` 一致）：字段数 + 字段名 + 字段类型 + 字段顺序都一致才通过。不引入"前缀匹配"或"字段子集"——若需忽略某字段，struct 字段是命名的，`?` 难以表达，M7 不支持字段子集匹配。

**类型别名天然透明**：类型别名在引擎内部就是它展开的类型（M2 §5，别名不产生新 type_t 实例，CTFE 求值到具体类型后直接 intern）。`extends` 匹配时别名天然透明，无需特殊规则——`type IntVec = Vec[i32];` 后 `IntVec` 在 sema 层就是 `Vec[i32]`，`IntVec extends Vec[?]` 自动匹配。

#### 29.3 `?` 匹配 void

`?` 在 `extends` PATTERN 中匹配 **void**（无例外）——void 也是类型，`?` 匹配任意具体类型含 void。

```
func f[T extends ?](x: T) { ... }
f[void](...);      // ✓ extends ? 通过（void 也是类型）

func g[T extends func(?)->?](fn: T) { ... }
g[func(i32)->void](fn);  // ✓ 返回 void 通配（? 匹配 void）
```

**void 的使用限制由现有类型系统处理**：`?` 匹配 void 是 `extends` 层面的事实，但 void 在非函数返回值场景因**无效类型**被现有 sema 拦截（`[]void`、`var x: void` 等是类型构造错误）。错误性质区分：

- **无效类型**（`[]void`）= 类型构造错误（现有 sema 规则拦截），不是泛型约束失败。
- **约束不通过**（`str extends *?` 失败）= 泛型拦截（§27/§28）。

两者独立，不混淆。`extends ?` 不会因 T=void 而拦截，但 T=void 在后续使用点（如 `var x: T`）会被现有类型系统拦截。

#### 29.4 comptime 值参数在 PATTERN 中

PATTERN 中的数组长度等 comptime 值位置只能使用：

- **具体字面量值**：`[10]i32`、`[3][4]f64`
- **通配符 `?`**：`[?]i32`（长度任意）、`[?]?`（长度+元素都任意）

**PATTERN 中不能引用 comptime 值参数变量**（如 `[N]i32` 中 N 是外层参数）——语法非法，报错引导用 `?`。与 §27"不捕获"边界一致：PATTERN 里的变量若允许，要么是捕获（违反边界），要么是歧义。

```
func f[T extends [10]i32](a: T) { ... }
f[[10]i32](arr);     // ✓ 长度匹配
f[[20]i32](arr);     // ✗ 长度不匹配（10 ≠ 20）

func g[T extends [?]i32](a: T) { ... }
g[[10]i32](arr);     // ✓ 长度通配，元素 i32 匹配
g[[20]i32](arr);     // ✓ 长度通配

func k[T extends [N]i32](a: T) { ... }      // ✗ 非法：PATTERN 不能引用变量 N
                                                //    报错：use '?' for wildcard length in extends pattern
```

需要长度通配用 `?`，需要具体长度写字面量。comptime 值参数作为独立类型参数声明（`func f[T, N: comptime u64](...)`），不通过 PATTERN 引用。

#### 29.5 函数类型严格匹配

`func(A)->R extends func(P)->Q` 按签名**严格结构同构匹配**：

- **参数数量一致**——PATTERN 与 T 的参数个数必须相同，否则不通过。
- **参数类型逐个递归匹配**——按 §27 递归向下解构匹配（通配符 `?` 可用）。
- **返回类型递归匹配**——同上。
- **不引入参数逆变/返回协变**——函数类型在 clux 是名义的（签名即身份），按结构同构匹配，不做方向性协变/逆变。

```
func f[T extends func(i32)->i32](g: T) { ... }
f[func(i32)->i32](g);      // ✓ 完全匹配
f[func(i32)->f64](g);      // ✗ 返回类型不匹配
f[func(f64)->i32](g);      // ✗ 参数类型不匹配
f[func(i32, i32)->i32](g); // ✗ 参数数不匹配（1 ≠ 2）

func g[T extends func(?)->?](g: T) { ... }
g[func(i32)->i32](g);      // ✓ 参数+返回都通配
g[func(str)->void](g);     // ✓ void 返回通配（规则 29.3）

func h[T extends func(i32, ?)->?](g: T) { ... }
h[func(i32, f64)->str](g); // ✓ 首参数 i32 固定，次参数+返回通配
h[func(f64, f64)->str](g); // ✗ 首参数不匹配
h[func(i32)->str](g);      // ✗ 参数数不匹配（2 ≠ 1）
```

**可变参数（variadic）M7 不纳入**：clux 当前无 variadic 函数类型，`extends` 对函数类型的匹配仅针对固定参数数量。若后续引入 variadic 函数类型，匹配规则另定。

#### 29.6 enum / union 名义匹配，cunion 不参与泛型

**enum 与 union 参与泛型**，按**名义 + 类型参数递归匹配**：

- **构造器（类型名）名义匹配**——PATTERN 与 T 的类型名必须相同。
- **类型参数递归匹配**——泛型 enum/union 的类型参数按 §27 递归向下解构匹配（通配符 `?` 可用）。
- **变体表不参与结构匹配**——enum/union 是名义类型，变体（`Some`/`None`/`Ok`/`Err`）的内部结构不参与 `extends` 匹配。变体表相同但名字不同的类型不通过。

```
enum Option[T] { Some(T), None }
union Result[T, E] { Ok(T), Err(E) }

func f[T extends Option[?]](x: T) { ... }
f[Option[i32]](x);      // ✓ Option 名义匹配，i32 通配
f[Option[str]](x);      // ✓
f[Result[i32, str]](x); // ✗ 名义不匹配（Option ≠ Result）

func g[T extends Result[?, ?>](x: T) { ... }
g[Result[i32, str]](x); // ✓ Result 名义匹配，两参数通配
g[Option[i32]](x);      // ✗ 名义不匹配 + 元数不匹配

// 变体表相同但名字不同——不通过（名义）：
enum MyOption[T] { MySome(T), MyNone }
func k[T extends Option[?]](x: T) { ... }
k[MyOption[i32]](x);    // ✗ 名义不匹配（Option ≠ MyOption）
```

**cunion 不参与泛型体系**：cunion 仅用于 FFI/optional/tag union，不参与泛型。`extends` PATTERN 中不能出现 cunion 类型名，泛型类型实参也不能是 cunion 实例。与 cfunc 同属 FFI 边界，不参与泛型是一致的（§22）。

---

## 可变参数：泛型参数包

### 30. 参数包 `...Args`（仅编译期可变）

clux 的可变参数（variadic）采用**泛型参数包**（type parameter pack）实现，C++ template parameter pack 风格。**M7 仅支持编译期可变**——每次实例化时参数包展开为确定数量的参数，单态化为定长函数，**无运行期可变参数**（无 C `va_list` 风格）。

#### 30.1 参数包声明

类型参数列表中用 `...` 前缀声明**参数包**，吸收 0 个或多个类型实参：

```
func print[...Args](fmt: str, args: ...Args): void;
func f[T, ...Args](x: T, rest: ...Args): void;
type Tuple[...Args] = <...Args>;
struct TupleStruct[...Args] { data: <...Args>; }
```

**规则**：

- **参数包必须在类型参数列表最后**（与 C++/Go 一致）：`func f[...Args, T]` 非法。
- **每个声明最多一个参数包**，避免推导歧义。
- 参数包可与普通类型参数、comptime 值参数共存：`func f[T, N: comptime u64, ...Args](...)`。
- **comptime 值参数包** `...N: comptime u64`：纳入，吸收多个编译期值，在实例化时同样转换为编译期 tuple 消费（与类型参数包同构）。
  > **实现语义（2026-10-22）**：`N` 绑定为编译期 `[M]<elem>` 序列；
  > 函数体内 `N[i]`、`len(N)` 在 sema 折叠为常量（可用于数组长度
  > `[N[0]]T`）。值参数包不能从运行期实参推导，调用点必须显式给出
  > 实参（`f[1u64, 2u64](...)`；空包 `f[](...)`）。裸 `N` 不作为标量。

#### 30.2 可变参数语法 `args: ...Args` 与实例化签名

函数参数类型写 `...Args` 表示该参数是**可变参数**，吸收调用点的多余实参：

```
func print[...Args](fmt: str, args: ...Args): void { ... }
```

**实例化后的函数签名是分离参数**——参数包展开为 N 个独立参数（N = Args 包元素数）。`print("%d %d\n", 123, 234)` 实例化为：

```
func print(fmt: str, arg_0: i32, arg_1: i32): void;
```

> 注：`arg_0`/`arg_1` 仅是本文档的**伪代码概念名**，用于说明实例化后的签名形态。**函数体内无法访问这些名字**——它们不是真实标识符。函数体内只能访问聚合后的 `args` 元组（见 §30.3）。

#### 30.3 函数体内 args 是元组（sema 入口聚合）

实例化后签名是分离参数，但**用户在函数体内访问的 `args` 是一个元组值**。sema 在函数实例入口处**自动插入聚合代码**，把分离的可变参数 `arg_0, arg_1, ...` 聚合成元组，绑定到 `args` 名字：

```
// 用户写的函数体：
func print[...Args](fmt: str, args: ...Args): void {
    // args 是元组，类型 <...Args>
    var n = len(args);       // 元组长度（编译期常量）
    var first = args[0];     // 按下标访问（clux 元组下标语法）
    var second = args[1];
    for (i = 0; i < len(args); i += 1) {
        // ... 遍历（for-index，元组本身支持下标+长度）
    }
}

// 实例化 print[i32, i32] 后，sema 等价于生成：
func print(fmt: str, arg_0: i32, arg_1: i32): void {
    var args: <i32, i32> = .{arg_0, arg_1};   // sema 自动插入的聚合
    // 用户函数体（args.len=2, args[0] 类型 i32, args[1] 类型 i32）
}
```

**访问语法遵守 clux 元组规则**：
- `args[0]`/`args[1]`/... —— 按下标访问（clux 元组下标语法，非 `args.0`）。
- `len(args)` —— 元组长度（clux `len()` 内建，编译期常量，= Args 包元素数）。
- 遍历用 **for-index**（元组本身支持下标+长度，已支持，无需新增语法）。

**compiler/VM 零感知**：
- compiler 看到的是普通函数 `print(fmt: str, arg_0: i32, arg_1: i32)`（分离参数）。
- 调用点 `print("...", 123, 234)` 是普通调用（独立实参，不是元组）——与 C `printf` 调用形态一致。
- 元组聚合是 sema 在实例化时插入的局部变量构造，对 compiler/VM 透明。

#### 30.4 展开语法 `...`（类型展开纳入，值展开预留）

`...` 前缀有两种展开：

- **类型展开 `<...Args>`**（M7 纳入）：Args 包展开为元组类型，元素类型按包顺序。`<...Args>` 是普通类型表达式，可用于变量声明、字段类型、类型别名右值等。

  ```
  type Tuple[...Args] = <...Args>;
  struct TupleStruct[...Args] { data: <...Args>; }
  // Tuple[i32, str] = <i32, str>
  ```

- **值展开 `...args`**（M7 仅设计语法预留，实现留待后续）：将编译期已知长度的元组/数组展开为独立实参序列，用于函数调用转发。`...` 可用于编译期已知长度的元组和数组做参数展开，例如 `fprintf(stdout, fmt, ...args)` 的再传递场景。

  ```
  func forward[...Args](f: func(...Args)->void, args: ...Args): void {
      f(...args);    // 值展开：args 元组展开为独立实参传给 f（M7 预留，后续实现）
  }
  ```

  **M7 不实现值展开**——这是未来的单独语法，需再设计（涉及展开位置语义、与元组/数组的交互、嵌套展开等）。M7 仅在语法层预留 `...expr` 形式，sema 遇到时报错引导（`value pack expansion not implemented in M7`）。

#### 30.5 类型别名与部分应用

参数包用于定义元组类型别名：

```
type Tuple[...Args] = <...Args>;
type IntStr = Tuple[i32, str];   // = <i32, str>
type Empty = Tuple[];            // = <>（空元组）
```

**部分应用允许**：函数参数推导本来就支持部分传参+推导混合，参数包同样适用：

```
type First[T, ...Args] = T;           // 只取首元素，忽略包（合法）
func f[T, ...Args](x: T, rest: ...Args): T { return x; }
f[i32, str, f64](1, "x", 3.14);       // 显式 T=i32, Args=[str, f64]
f[i32](1, "x", 3.14);                 // 显式 T=i32, 推导 Args=[str, f64]
f(1, "x", 3.14);                      // 全推导 T=i32, Args=[str, f64]
```

#### 30.6 空包

`print("fmt")`（无可变实参）→ Args=[]，args 类型=`<>`（空元组）：

```
func print[...Args](fmt: str, args: ...Args): void { ... }
print("fmt");     // ✓ Args=[]，args 类型=<>, len(args)=0
print("fmt", 1);  // ✓ Args=[i32], args 类型=<i32>, len(args)=1
```

#### 30.7 调用点推导

从可变实参按顺序推导 Args 包：

```
func print[...Args](fmt: str, args: ...Args): void { ... }

print("fmt", 1, "x");            // 推导 Args=[i32, str]
print("fmt");                    // 推导 Args=[]
print[i32, str]("fmt", 1, "x");  // 显式标注 Args=[i32, str]（实参类型必须匹配）
```

推导规则：可变实参的类型按顺序组成 Args 包。显式标注时实参类型必须与标注匹配。

#### 30.8 参数包在 `extends` PATTERN 中

参数包 `...Args` 在 `extends` PATTERN 中**大多无意义**——只有 `extends <?>` 有意义（匹配任意元数元组）。其他写法语法允许但**退化为确定参数长度**，最多只是局部的类型变化：

```
func f[...Args extends <?>](args: ...Args): void { ... }    // ✓ 唯一有意义的包约束
// <?> 匹配任意元数元组，Args 包展开后是任意长度的元组类型

func g[...Args extends <i32, ?, i32>](args: ...Args): void { ... }
// 语法允许，但退化为：Args 包固定 3 个元素，首尾 i32，中间任意
// 等价于固定 3 参数的泛型，只是用参数包语法表达——局部类型变化，无包的"可变"语义

func h[...Args extends <?>](args: ...Args): void { ... }    // ✗ 其他形式无意义或不合法
```

**推荐**：参数包约束只写 `extends <?>`（或不写约束），其他形式虽然语法允许但无实际价值（退化为定长）。

#### 30.9 mangling

参数包实例的 mangling 与普通多参数一致——包元素按顺序编码：

```
print[i32, str]    → print$i32$str
print[]            → print$（空包，空 mangling 段）
print[i32, i32]    → print$i32$i32
```

空包的 mangling 需注意歧义——`print$` 与 `print`（非泛型）如何区分？由 mangling 的 `$` 后缀存在性区分（泛型实例必有 `$`，非泛型无 `$`）。具体编码实现时定稿。

#### 30.10 编译期可变 vs 运行期可变

**M7 仅支持编译期可变**：

- 每次实例化时参数包展开为**确定数量**的参数，单态化为定长函数。
- `print("...", 1, 2)` 与 `print("...", 1, 2, 3)` 是**两个不同的实例**（不同 mangling、不同 fid、不同签名）。
- **无运行期可变参数**（无 C `va_list` 风格）——不能写一个接受任意数量实参的运行期函数。若需运行期不定长，用切片 `[]T` 或元组值显式传递。

这是单态化的固有约束：实例化时类型实参组合必须完全确定，参数包在实例化时展开为定长。

---

## struct 属性/方法约束：`interface` 编译期实体

### 31. `interface` 声明与实现

§29.2 规定 struct 的 `extends` 只检查字段表不看属性/方法。属性/方法的约束引入 `interface`——**纯编译期实体**，不产生运行时 vtable、不产生 type_t 实例、不进 sema->types。

#### 31.1 interface 声明

`interface` 是顶层声明（与 struct/enum/func 同级），内部只允许两类成员：

- **属性签名**：`var fd: i32;` —— 无初始化，必须标注类型。
- **方法签名**：`func write(self: *T, data: ref []u8);` —— 无函数体。

```
interface IWriter[T] {
    var fd: i32;
    func write(self: *T, data: ref []u8);
    func flush(self: *T): void;
}
```

**规则**：

- interface 可泛型：`interface IWriter[T]`，T 通常是**被约束类型的 self 类型**（CRTP 风格），但不强制——T 可以是任意类型参数（见 §31.7）。
- interface 内**不允许**：有初始化的 var、有函数体的 func、type def、嵌套 struct/enum、其他 interface 声明。
- interface 内的 `self` 不是关键字（与 struct 方法一致，§5），只是约定第一个参数名。
- **interface 是纯编译期实体**：不进 sema->types，无 type_t 实例，无 id，不产生运行时 vtable。

#### 31.2 struct 实现 interface（CRTP）

struct 用 **`implements`** 声明实现 interface，把自身作为类型实参传入（CRTP）。**用 `implements` 而非 `extends`** 避免与 §29.2 struct `extends`（结构字段匹配）歧义——`extends` 是类型参数约束与结构模式匹配，`implements` 是 struct 声明实现 interface：

```
interface IWriter[T] {
    var fd: i32;
    func write(self: *T, data: ref []u8);
}

struct Writer implements IWriter[Writer] {
    var fd: i32;
    func write(self: *Writer, data: ref []u8) { ... }
}
```

**检查时机**：struct 声明 `implements IWriter[Writer]` 时，sema 检查 Writer 是否有 interface 声明的所有属性与方法，签名匹配：

- **属性签名匹配**：Writer 必须有 `var fd: i32`——类型完全相等，复用 §29.1 的类型身份匹配（限定符与所有权严格一致）。
- **方法签名匹配**：Writer 必须有 `func write(self: *Writer, data: ref []u8)`——self 类型 `*T` 替换为 `*Writer` 后，**参数数量 + 参数类型 + 返回类型严格匹配**（§29.5，不引入协变/逆变）。
- 缺少任一属性/方法或签名不匹配 → 编译期报错，指向 struct 声明点与 interface 声明点。

#### 31.3 interface 作为类型参数约束

interface 可用于 `T extends IWriter[T]` 约束类型参数：

```
func serialize[T extends IWriter[T]](w: *T, data: ref []u8): void {
    w.write(data);    // ✓ T 约束为实现 IWriter[T]，write 方法可用
}

serialize(&writer, data);   // ✓ Writer implements IWriter[Writer]，通过约束
serialize(&123, data);      // ✗ i32 未实现 IWriter[i32]
```

**与 §27 结构 PATTERN 的关系**：`T extends PATTERN` 中 PATTERN 若是 **interface 名**则走 interface 实现检查，若是**结构模式**（`*?`/`[?]?`/`<?>` 等）则走结构匹配。sema 按 PATTERN 名字分派——interface 名查 interface 登记表，结构模式走递归解构。

#### 31.4 interface 纯编译期，无运行时多态

**interface 只作为约束使用，不是类型**——interface 不产生运行时实体，在**所有值类型位置都是语法错误**：

- 不进 sema->types，无 type_t 实例，无 id。
- **不能用作变量类型**：`var w: IWriter` 非法。
- **不能用作参数类型**：`func f(w: IWriter)` 非法。
- **不能用作返回类型**：`func f(): IWriter` 非法。
- **不能用作字段类型**：`struct S { w: IWriter; }` 非法。
- **不能用作类型别名右值**：`type W = IWriter;` 非法。
- interface 唯一合法位置是 `extends` 约束 PATTERN：`T extends IWriter[T]`、`struct W implements IWriter[W]`。

```
// ✗ 全部非法（interface 不是类型）：
var w: IWriter;
func f(w: IWriter): IWriter;
struct S { w: IWriter; }
type W = IWriter;
func g(x: *IWriter);     // 即使包成指针也不是类型

// ✓ 合法（interface 只作约束）：
func f[T extends IWriter[T]](w: *T): void;   // T 约束为实现 IWriter[T]
struct Writer implements IWriter[Writer] { ... }
```

**不引入运行时多态**：无 vtable、无动态分派。`serialize(&writer, ...)` 单态化为 `serialize$Writer(w: *Writer, ...)`，`w.write(data)` 静态分派到 `Writer::write`。若需运行时多态，用 union/cunion 手动管理（clux 风格：显式优于隐式）。

#### 31.5 多个 interface 用 `&` 组合

struct 实现多个 interface 与类型参数约束多个 interface **统一用 §28 的 `&` 组合**，不引入逗号语法：

```
interface IWriter[T] { func write(self: *T, data: ref []u8); }
interface IReader[T] { func read(self: *T, buf: ref []u8): u64; }

// struct 实现多个 interface：
struct File implements IWriter[File] & IReader[File] {
    var fd: i32;
    func write(self: *File, data: ref []u8) { ... }
    func read(self: *File, buf: ref []u8): u64 { ... }
}

// 类型参数约束多个 interface：
func f[T extends IWriter[T] & IReader[T]](x: *T) { ... }   // T 必须同时实现两者
func g[T extends IWriter[T] | IReader[T]](x: *T) { ... }   // T 实现任一即可（§28 析取）
```

#### 31.6 interface 成员复制 `...A;`（非继承）

interface **没有继承**，但支持用 `...A;` 把另一个 interface 的成员**复制**到当前 interface：

```
interface IWriter[T] {
    func write(self: *T, data: ref []u8);
}
interface IReader[T] {
    func read(self: *T, buf: ref []u8): u64;
}

// B 复制 A 的所有成员：
interface IReadWriter[T] {
    ...IWriter[T];    // 复制 IWriter[T] 的所有属性/方法签名
    ...IReader[T];    // 复制 IReader[T] 的所有属性/方法签名
}
// IReadWriter[T] 等价于声明了 write + read 两个方法签名
```

**`...A;` 的语义**：

- **抽象语义**：与 §30 参数包的 `...` 相同——"展开"。但**实际语义不同**：§30 的 `...Args` 是参数包展开（展开类型/值序列），§31 的 `...A;` 是 interface 成员复制（展开 interface 的成员声明到当前 interface 体）。
- **是复制不是继承**：`IReadWriter` 不"是" `IWriter`，没有子类型关系。`IReadWriter` 只是把 `IWriter` 的成员签名复制到自己体内。`struct W implements IReadWriter[W]` 检查的是 `IReadWriter` 体内复制后的完整成员集（write + read），与 `IWriter` 无名义关联。
- **复制是文本级的**：sema 在处理 interface 声明时，把 `...A;` 替换为 A 的成员签名列表（深拷贝签名 AST），后续检查基于复制后的完整成员集。
- **可嵌套复制**：`interface C { ...B; }` 中 B 若含 `...A;`，C 会递归复制到 A 的成员（展开是递归的）。
- **环检测**：`interface A { ...B; } interface B { ...A; }` 非法（循环复制），sema 检测环并报错。

#### 31.7 interface 泛型参数不限于 self

interface 泛型参数 T 通常是 self 类型，但**不强制**——T 可以是任意类型参数，用于表达方法签名中非 self 的类型：

```
interface IComparable[T] {
    func compare(self: *T, other: *T): i32;   // T = self 类型
}

interface IContainer[T, E] {
    func get(self: *T, idx: u64): E;          // T = self 类型，E = 元素类型
    func push(self: *T, e: E): void;
}

struct Vec[E] implements IContainer[Vec[E], E] {
    func get(self: *Vec[E], idx: u64): E { ... }
    func push(self: *Vec[E], e: E): void { ... }
}

// 约束使用：
func sum[T, E extends IContainer[T, E]](c: *T): E { ... }   // T=self类型, E=元素类型
```

---

## 决策点汇总（用户确认记录）

| 决策点 | 选择 | 理由 |
|---|---|---|
| 参数化范围 | 函数 + 类型（struct/enum/union/别名，cunion 除外） + 泛型类型别名 | 覆盖经典容器场景，cunion 仅 FFI 不参与泛型（§29.6） |
| 约束机制 | 鸭子类型 + 可选 `extends` 结构约束（§27） | 默认无约束，可选结构前置过滤，复用 M2 extends 语义 |
| 实例化策略 | 全单态化 | 运行期零开销，与 CTFE 地基契合 |
| comptime 值参数 | 纳入 | 支持泛型容器（含数组长度） |
| 语法位置 | 方括号 `[T,U]` 前置 | 避免与 `<` `>` 比较符冲突 |
| 调用点推导 | 仅函数调用可推导，类型上下文必须显式 | 声明侧类型是约束非推断目标 |
| 递归实例化 | 允许 + 深度上限（128） | 支持 template metaprogramming，复用 CTFE max_depth |
| struct 字段用类型参数 | 允许 | 泛型容器核心场景 |
| 类型参数默认值 | 支持 | 表达力，从右向左省略 |
| struct 方法接收者 | 显式写完整泛型类型 | 与现有 struct 方法机制一致，不引入 Self |
| comptime 值参数语法 | `: comptime <type>` 标注 | 复用 comptime 关键字，显式区分泛型值参数 |
| 泛型 cfunc | 不支持 | C 无泛型，cfunc 保持非泛型 |
| 类型参数穿透 type def 右值（§24） | 允许 | T 作用域是整个声明体，type def 在体内定义自然可见 |
| 未实例化泛型作为值（§25） | 不允许（只是符号，实例化后才是实体） | 多态签名无法表达，无 fid/entry_pc |
| 单态化时机（§26） | 发生在编译期，实例是运行时实体 | 与 comptime func 正交：编译期生成 + 运行时执行 |
| `comptime func` + 泛型组合（§26） | M7 不纳入 | 编译期折叠与运行时执行语义冲突，需独立设计 |
| 约束机制（§27） | 可选 `extends` 结构约束 + 通配符 `?`/`<?>`，默认无约束 | 复用 M2 extends 递归解构，鸭子类型前提下加结构前置过滤 |
| 通配符语义（§27） | `?`=任意单类型，`<?>`=任意元数元组，递归向下逐节点匹配 | 局部通配符自然解析到结构内部任意深度 |
| 通配符合法位置（§27） | 仅 `extends` PATTERN 中合法，普通类型表达式不允许 | 具体类型必须有具体大小，`?` 是约束模式专用 |
| `extends` 捕获变量（§27） | M7 不纳入（只匹配不绑定） | 无 infer 能力，需要子类型时声明独立参数 |
| `extends` 重载分派（§27） | M7 不纳入（只过滤不分派） | 同名同元数泛型=重复定义，避免 overload resolution |
| comptime 值参数 `extends`（§27） | 不写（仅类型参数可 `extends`） | 值域约束超出本轮 |
| `extends` 析取/合取（§28） | `extends A | B`（OR）+ `extends A & B`（AND），`&` 优先级高于 `|`，支持括号 | 单泛型内部结构匹配组合，不是重载分派；短路求值 |
| 限定符匹配（§29.1） | `const` / `own` / `ref` / `fatal` 严格一致：PATTERN 带限定符则 T 必须同限定符，不带则 T 也不带；指针 rc（普通 / share / weak）同样参与；不做协变/逆变 | 限定符是类型构造，约束判断类型身份而非赋值兼容；协变会放行错误的限定符组合 |
| 所有权与 `extends`（§29.1） | 参与匹配：`own *?` / `ref *?` / `*?` 分别只匹配对应所有权的指针 | 所有权是类型修饰（独立 `TYPE_KIND`），与 const 同属类型身份 |
| 所有权通配 `?*?`（§27/§29.1，2026-10-22） | `?` 可出现在 `*` 的所有权位置：匹配 own/ref/fatal 任意一种；RC 不参与，必须显式 `share *?` / `weak *?` | "一切所有权指针"的写法；RC 是独立类型身份，不能被通配吞掉 |
| 调用点所有权不对称（§4.1，2026-10-22） | fatal 实参 → own 形参；own/fatal 实参 → ref 形参；推导出的 T 不含形参侧所有权 | 与值层所有权阶梯一致；约束匹配仍是严格类型身份 |
| struct `extends`（§29.2） | 只检查字段表（完全相等），不看属性/方法 | 复用 M2 struct_type_compatible 语义，名义属性/方法不参与结构匹配 |
| 类型别名与 `extends`（§29.2） | 天然透明（无需特殊规则） | 别名在引擎内部就是展开类型（M2 §5），sema 层无别名实体 |
| `?` 匹配 void（§29.3） | 匹配（无例外） | void 也是类型；void 的使用限制由现有类型系统拦截，非泛型约束职责 |
| comptime 值在 PATTERN（§29.4） | 只能用字面量值或 `?`，不能引用变量 | 与不捕获边界一致，避免歧义；通配用 `?`，精确用字面量 |
| 函数类型 `extends`（§29.5） | 严格匹配参数数量+参数类型+返回类型，不引入逆变/协变 | 函数类型名义（签名即身份），结构同构匹配，variadic 留待后续 |
| enum/union `extends`（§29.6） | 名义匹配 + 类型参数递归，变体表不参与 | 名义类型（名字即身份），变体结构相似但名字不同不通过 |
| 泛型 cunion（§29.6） | 不纳入泛型体系 | cunion 仅用于 FFI/optional/tag union，与 cfunc 同属 FFI 边界 |
| variadic 泛型 / 参数包（§30） | 纳入，**仅编译期可变**（非运行期可变） | 单态化定长，每次实例化展开为确定数量参数；无 C va_list 风格 |
| 参数包声明（§30.1） | `...Args` 前缀，列表最后，每声明最多一个；comptime 值参数包也纳入 | 与 C++/Go 一致，避免推导歧义 |
| 可变参数签名（§30.2） | 实例化后是分离参数（arg_0, arg_1, ...），体内不可访问这些名字 | compiler/VM 看到普通定长函数，调用点独立实参（C printf 形态） |
| 入口聚合（§30.3） | sema 在实例入口插入聚合代码，绑定 args 元组 | 用户体内访问 args 元组（<...Args>），用 args[i]/len(args)（clux 元组语法） |
| 类型展开（§30.4） | `<...Args>` 纳入，普通类型表达式 | 用于类型别名/字段类型/变量声明 |
| 值展开（§30.4） | M7 仅语法预留，不实现（后续单独设计） | `...args` 转发场景（fprintf(stdout,fmt,...args)）需展开位置语义等再设计 |
| 遍历（§30.3） | for-index（元组本身支持下标+长度，已支持） | 无需新增语法 |
| 类型别名与部分应用（§30.5） | `type T[...Args]=<...Args>` 纳入；部分应用允许 | 函数参数推导本来就支持部分传参+推导混合 |
| 空包（§30.6） | Args=[] → 空元组 <> | print("fmt") 合法，len(args)=0 |
| 参数包在 extends（§30.8） | 只 `extends <?>` 有意义，其他语法允许但退化为定长 | `...Args extends <i32,?,i32>` = 固定 3 参数，局部类型变化，无"可变"语义 |
| 运行期可变参数 | M7 不纳入 | 单态化要求实例化时实参组合完全确定，运行期不定长用 []T 或元组值显式传递 |
| `interface` 实体（§31） | 纳入，纯编译期，约束 struct 属性/方法 | §29.2 struct extends 只看字段，属性/方法约束需独立机制 |
| interface 声明（§31.1） | 顶层声明，只允许无初始化 var + 无函数体 func | 纯签名集合，不含实现 |
| struct 实现 interface（§31.2） | CRTP：`struct W implements IWriter[W]`（用 implements 非 extends 避免歧义），声明时检查属性/方法签名 | self 类型替换后严格匹配，无运行时实体 |
| interface 作类型约束（§31.3） | `T extends IWriter[T]`，sema 按 PATTERN 名分派 interface vs 结构模式 | interface 名查登记表，结构模式走递归解构 |
| interface 纯编译期（§31.4） | 不进 sema->types，**所有值类型位置（变量/参数/返回/字段/别名右值）都是语法错误**，唯一合法位置是 extends 约束 PATTERN | interface 只作约束不作类型，无 vtable/动态分派；运行时多态用 union/cunion 手动管理 |
| 多 interface 组合（§31.5） | 统一用 `&`（§28），不引入逗号语法 | struct 实现多个 + 类型参数约束多个都用 `&` |
| interface 成员复制（§31.6） | `...A;` 复制成员（非继承），可嵌套+环检测 | 抽象语义=展开（同 §30 `...`），实际语义=interface 成员复制；无子类型关系 |
| interface 继承 | M7 不纳入 | 用 `...A;` 成员复制 + `&` 组合替代，避免组合爆炸 |
| interface 泛型参数（§31.7） | 不限于 self 类型，可多参数（IContainer[T,E]） | T 通常是 self 但可表达非 self 类型（元素类型等） |
