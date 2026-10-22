# 运行期可变参数与元组表示（设计草案）

> 状态：**设计草案，未实现**。M7 当前的可变参数只有两类：
> 1. 泛型参数包 `...Args`：编译期单态化，实例函数是定长签名；
> 2. `extern cfunc` 的 C `...`：走 libffi，不进入 clux 函数调用约定。
>
> 本文设计的是第三种：**clux 语言函数的运行期可变参数**，并明确它在
> VM 里用**元组值**表示。目标是先支持**静态类型已知**的参数包，不引入
> 动态类型/`any`。

---

## 1. 结论摘要

推荐采用 **Option B：单个 tuple 形参 + 静态 tuple 类型**。

- 变参实例的签名从“N 个分离形参”改为
  “固定形参 + 1 个 tuple 形参”；
- tuple 的元素类型在实例化时完全确定，例如
  `print[i32, i32]` 的 `args` 类型是 `<i32, i32>`；
- 调用点在字节码里构造 tuple，函数体直接绑定这个 tuple 形参；
- 复用现有 `tuple_type_t` / `VTABLE_TUPLE`，不新增 value kind；
- 不改变 C ABI 的 `extern cfunc ...` 路径。

这样做的收益：

- 可变参数在运行期有一个**一等值**表示，而不是“只在 sema 里聚合、
  compiler/VM 零感知”的假象；
- 天然支持转发 `f(...args)`、`len(args)`、`args[i]`、按 tuple 传递；
- 元素类型、布局、所有权、clone/dispose 全部复用现有 tuple 机制；
- `.cxb/.cxs` 不需要运行期动态类型描述符，因为 tuple 类型在编译期已登记。

---

## 2. 现状与问题

当前实现（`docs/m7-design.md` §30）：

```clux
func print[...Args](fmt: str, args: ...Args): void {
    var n = len(args);
    var first = args[0];
}
```

实例化后 sema 生成：

```clux
func print__i32_i32(fmt: str, arg_0: i32, arg_1: i32): void {
    var args: <i32, i32> = .<i32, i32>{ arg_0, arg_1 }; // sema 注入聚合
}
```

问题：

1. `args` 不是真正的形参，只是 sema 在入口插入的局部变量；
2. 无法在运行期把 `args` 作为一个值转发（`...args` 值展开未支持）；
3. ABI 随实例的包长度变化（`print__i32_i32` 两个参数、
   `print__i32_i32_i32` 三个参数），函数值/回调/容器里的签名形态不统一；
4. sema 注入聚合代码需要合成 AST，增加“编译器零感知”的维护负担。

---

## 3. 方案比较

### Option A：保持分离形参 + 入口聚合（现状）

- 优点：零运行时开销；不用改调用约定。
- 缺点：`args` 不是真正参数；值展开/转发困难；ABI 随 arity 变化。
- 结论：保留为**兼容路径**，但不再作为新设计目标。

### Option B：单个 tuple 形参（推荐）

实例签名：

```clux
func print__i32_i32(fmt: str, args: <i32, i32>): void
```

调用点：

```clux
print[i32, i32]("%d %d", 1, 2)
// 编译为：PUSH "%d %d"; MAKE_TUPLE <i32,i32> 2; CALL print__i32_i32
```

函数体里的 `args` 就是第二个形参，类型 `<i32,i32>`。

- 优点：静态类型完整；复用 tuple 布局/所有权；转发简单；ABI 统一。
- 缺点：多一次 tuple 构造/拆包；需要新 opcode 与签名标志。
- 结论：**采用**。

### Option C：运行期动态 tuple 描述符（异构、`any`）

```clux
func log(...args) {          // 每个实参类型可以不同
    for (i = 0; i < len(args); i += 1)
        println(args[i]);    // args[i] 类型未知
}
```

需要在 VM 运行期按实参类型动态 intern tuple 类型，并引入 `any`/类型擦除、
运行期类型检查、动态 clone/dispose。它解决的是**动态语言式**变参，不是
M7 的静态泛型包。

- 结论：**不在本设计范围**；等 `any`/运行时类型反射设计确定后再做。
- 本文的 tuple 表示层会为它留出扩展点，但不引入动态元素类型。

---

## 4. 类型模型

### 4.1 签名

`func_sig_t` 增加一个变参风格字段（不要复用 C ABI 的 `is_variadic`）：

```c
typedef enum {
    FUNC_VARIADIC_NONE = 0,
    FUNC_VARIADIC_C_ABI,     /* extern cfunc ...：libffi */
    FUNC_VARIADIC_TUPLE,     /* clux 运行期 tuple */
} func_variadic_style_t;

typedef struct {
    const type_t **params;      /* 固定形参（不含 tuple） */
    size_t        param_count;  /* 固定形参个数 */
    const type_t *return_type;
    bool          is_variadic;  /* 兼容字段：C ABI / 内建 variadic */
    func_variadic_style_t variadic_style;
    const type_t *variadic_tuple_type; /* TUPLE 风格：最后一个逻辑形参 */
} func_sig_t;
```

规则：

- `FUNC_VARIADIC_NONE` / `C_ABI`：维持现状；
- `FUNC_VARIADIC_TUPLE`：
  - 逻辑形参数 = `param_count + 1`（最后一个是 tuple）；
  - 调用必须恰好传 `param_count + 1` 个实参；
  - 第 `param_count` 个实参类型必须与 `variadic_tuple_type` 相同/兼容；
  - `func_type_param_count()` 是否包含 tuple 需要在 API 层明确：
    **包含**，避免调用点两套计数；另提供
    `func_type_fixed_param_count()` 供 FFI/ABI 使用。

### 4.2 实例化

泛型参数包实例化时：

```text
fixed_params = 原固定形参
variadic_tuple_type = type_tuple_intern(<...Args>)
```

`args: ...Args` 在 sema 的形参表里不再展开成独立形参；它变成
**一个 AST_VAR_DEF，类型 = tuple 类型**。实例函数体里的 `args` 就是
普通 tuple 形参，`sema_build_func_scope` 正常绑定。

空包：`< >` 是合法 tuple 类型；实例仍有一个空 tuple 形参。

### 4.3 函数值/回调

- `func_type` 的 tuple 签名是普通 `type_t`，可以进函数指针、闭包捕获、
  cfunc 回调（非 C variadic）；
- C 回调仍不允许 `C_ABI` variadic（现有规则）；
- tuple 风格函数可以作为 C 回调（如果元素都是 C ABI 兼容类型），
  因为它在 ABI 上是“固定参数 + 一个 tuple 值”。

---

## 5. 调用约定

### 5.1 调用点

`sema` 在实例化后把调用点实参改写为：

```text
[固定实参..., tuple 实参]
```

tuple 实参的构造：

- 编译器发 `MAKE_TUPLE <type_id> <n>`：
  - 从栈顶取 `n` 个值；
  - 按 `tuple_type_t` 的 offsets 写入新 tuple 值；
  - 元素所有权按 tuple 元素类型处理；
- 或复用现有 `CONSTRUCT`/`PUSH_TUPLE` 序列，但推荐专用 opcode：
  - 语义单一；
  - disasm 可读（`MAKE_TUPLE <i32,i32> 2`）；
  - 便于 `.cxb` 版本化。

### 5.2 VM 入口

`func_vcall` / `MEMBER_CALL`：

- 从 `func_t.sig` 读 `variadic_style`；
- `FUNC_VARIADIC_TUPLE`：
  - 校验实参数 = fixed+1；
  - 校验最后一个实参类型（或隐式转换）到 `variadic_tuple_type`；
  - 绑定 tuple 形参到 frame slot；
  - 不执行旧的 `arg_N` 聚合。

### 5.3 函数体

- `args` 就是 tuple 形参的 shadow；
- `len(args)` 编译期常量（tuple 元素数）；
- `args[i]` 走普通 tuple 下标；
- `for (x of args)` 走 tuple 迭代（已有）；
- 不再有 sema 注入的 `var args = ...` AST。

---

## 6. 转发与展开

### 6.1 `f(...args)` 值展开

目标：把 tuple 直接作为被调函数的 tuple 实参。

```clux
func forward[...Args](args: ...Args): void {
    print(...args);      // print 的实例 tuple 类型必须相同
}
```

规则：

- `...args` 只在“被调函数是 `FUNC_VARIADIC_TUPLE` 且 tuple 类型匹配”时合法；
- 不重新构造 tuple，直接传递同一个 tuple 值；
- **所有权**：默认按 tuple 值移动（consume）；如果调用方仍需使用
  `args`，需要显式借用/克隆（语法在 §7 定义）。

### 6.2 展开到固定参数

```clux
print("%d %d", ...args)
```

如果 `args` 是 `<i32,i32>`，且 `print` 实例固定参数为 `fmt` +
tuple `<i32,i32>`，则直接传 tuple；不展开成两个独立实参。

### 6.3 与 C variadic 的边界

- `extern cfunc printf(fmt, ...)` 仍是 `C_ABI`；
- 不能把 clux tuple 直接传给 C `...`；
- 若要支持 `printf("%d", ...args)`，需要单独的 FFI 桥接（逐元素
  libffi 打包），属于后续工作。

---

## 7. 所有权与生命周期

tuple 值本身就是所有者：

- 元素按 tuple 元素类型的 ownership 修饰拥有；
- tuple 被移动/传入函数后，元素所有权随 tuple 转移；
- tuple 销毁时按元素类型 dispose；
- `args[i]` 返回借用视图，生命周期不超过 tuple；
- 从 tuple 中 `move(args[i])` 仅在元素类型允许 move 时合法。

转发 `f(...args)` 的默认语义：

- **consume**：tuple 作为整体移动给被调函数；
- 若需要保留，写 `f(...clone(args))`？——本设计不新增 `clone` 表达式；
  建议先只支持 consume，并在诊断中说明；
- 若被调函数形参是 `ref` tuple，则 `f(...args)` 传借用（不移动）。

这部分需要与 ownership 模型最终确认；当前草案先固定“整体值传递”，
不拆元素、不逐元素 move。

---

## 8. 字节码与容器

### 8.1 新 opcode

```text
MAKE_TUPLE <u32 type_id> <u32 count>
```

- 栈：`[v0, v1, ..., v(count-1)]` → `[tuple]`；
- `type_id` 必须在 `vm->types_by_id` 已定义（compiler hoist 已保证）；
- `.cxs` 渲染为 `MAKE_TUPLE <type_id> <count>`，并注释类型名。

### 8.2 容器

- tuple 类型已经由 `sema_types_global` 登记，`.cxb` 的类型表会包含它；
- 不需要额外的运行期描述符段；
- 装载器按类型表恢复 tuple 布局；
- `BCODE_GRAPH_VERSION` 需要递增，因为签名语义/opcode 新增；
- 旧 `.cxb`（无 tuple 变参 ABI）不兼容；**不做跨版本兼容**，重新 emit。

### 8.3 disasm / 文本往返

`bc emit`/`bc disasm` 需支持：

```text
MAKE_TUPLE 65 2
```

并通过类型 id 注释还原 `<i32,i32>`。

---

## 9. 诊断

- 调用 tuple 变参函数但最后一个实参不是 tuple：
  `expected tuple <i32,i32> for variadic parameter 'args'`
- tuple 元素数/类型不匹配：
  `variadic tuple mismatch: expected <i32,i32>, got <i32>`
- `...args` 展开到非 tuple 变参函数：
  `'...' value expansion requires a tuple-variadic callee`
- C variadic 与 tuple 混用：
  `cannot pass a clux tuple to a C variadic function`
- 空 tuple：允许，`len(args) == 0`。

---

## 10. 实施阶段

### Phase 1：类型/签名（无行为变化）

1. `func_sig_t` 增加 `variadic_style` / `variadic_tuple_type`；
2. `func_type_*` 构造器/访问器；
3. 现有 `extern cfunc` 标为 `C_ABI`，内建 `len` 等标为 `C_ABI`/特殊；
4. 测试签名相等/打印/反射。

### Phase 2：sema 实例化

1. 泛型包实例化生成 tuple 类型，签名最后形参 = tuple；
2. `args` 作为普通形参绑定，删除入口聚合；
3. 调用点参数计数改为 fixed+1；
4. 体 walk/类型检查使用 tuple 形参。

### Phase 3：compiler/VM

1. `MAKE_TUPLE` opcode + asm；
2. 调用点构造 tuple；
3. `func_vcall`/`MEMBER_CALL` 识别 tuple 风格；
4. 容器版本递增。

### Phase 4：展开与转发

1. `...args` 值展开；
2. 所有权规则；
3. `.cxb` 往返测试。

### Phase 5：FFI（可选，独立）

1. tuple→C 逐元素 libffi 打包；
2. 明确禁止 own/ref 复杂所有权跨 C ABI。

---

## 11. 验收用例

```clux
func sum[...Args](args: ...Args): i32 {
    var s = 0;
    for (i = 0; i < len(args); i += 1) s += args[i] as i32;
    return s;
}

func forward[...Args](args: ...Args): i32 { return sum(args); }

func main(): i32 {
    return sum[i32, i32](1, 2) + forward[i32](3) - 6;
}
```

另需覆盖：

- 空包 `sum[]()`；
- 单元素、多元素；
- tuple 元素含 struct/own；
- `args[0]` 越界诊断；
- `len(args)` 常量折叠；
- `.cxb` emit → run；
- 函数值：`var f: func(<i32,i32>) -> i32 = sum__i32_i32;`（tuple 作为普通形参）；
- C variadic 回归：`printf` 不受影响。

---

## 12. 明确不做

- 运行期异构 tuple（Option C）；
- `any` / 动态类型；
- C `va_list` 语义；
- 变参函数重载；
- 把 tuple 直接传给 C `...`（需独立 FFI 设计）。

---

## 13. 待确认问题

1. `...args` 展开默认 move 还是 borrow？
2. 是否允许 tuple 变参函数作为 C 回调（ABI 上是固定参数+tuple）？
3. 空 tuple 是否作为普通 tuple 值参与相等/布局（建议是）？
4. `func_type_param_count` 是否包含 tuple（建议包含，另给 fixed 计数）？
5. 是否在 M8 引入 `any` 后再评估 Option C？

以上问题确认后，Phase 1–3 可以独立落地，不阻塞当前 M7 的编译期参数包。
