# clux 元信息（metadata）设计调研

> 状态：调研（2026-10-23）。未实现。

目标语法：

```clux
[[metadata("name", value)]]
struct User {
    [[metadata("unit", "px")]] x: i32;
    [[metadata("role", "handler")]]
    func run(self: ref *User): void { ... }
}

var m = get_metadata[User]("name");
var u = get_field_metadata[User]("x", "unit");
var r = get_method_metadata[User]("run", "role");
```

要求：

- 运行期可读取类型 / 字段 / 方法上的元信息；
- 元信息只能由 `[[metadata(...)]]` 声明；
- `value` 必须是编译期已知值，包含函数；
- 未来 C 后端中元信息就是全局变量。

---

## 1. 总体判断

**可以实现，而且最适合做成“编译期解析 + 运行期常量读取”的机制**，
不需要运行期哈希表：

- 装饰器在 sema 阶段求值并登记到类型/字段/方法的元信息表；
- `T` 和 `"name"` 如果是编译期已知的（类型实参 + 字符串字面量），
  sema 直接定位到具体条目；
- compiler 发一条“读取元信息常量”的指令 / C 后端直接引用全局变量；
- 运行期只读，没有注册、查找、释放。

这与 C 后端“元信息 = 全局变量”的目标天然一致。

---

## 2. 必须先定的核心问题：返回值类型

`get_metadata[T]("name")` 没有额外的类型参数，但元信息值可以是
`str` / 整数 / bool / 函数等不同类型。

两种方案：

### 方案 A：编译期解析，返回值使用**元信息条目自己的静态类型**（推荐）

```clux
[[metadata("level", 3)]]
struct User {}

var level: i32 = get_metadata[User]("level");   // i32
```

- sema 在解析 `get_metadata[User]("level")` 时已经找到条目；
- 直接返回条目值的类型（i32 / str / func(...) 等）；
- C 后端直接生成 `LOAD_GLOBAL __clux_meta_...`；
- 不存在类型擦除 / tagged union / 运行期反射；
- 缺失 key 在编译期报错；
- 如果 key 是动态字符串则无法用此方案。

优点：

- 类型安全，调用点不需要 `as` / 解包；
- 函数值元信息可以直接调用：
  ```clux
  var h = get_method_metadata[User]("run", "handler");
  h(user);
  ```
- C 后端最简单：全局变量有真实 C 类型。

缺点：

- `name` 必须是字符串字面量（至少编译期常量）；
- 缺失元信息是编译错误，不是运行期 nil；
- 同一类型上同名元信息不能有不同类型（应禁止重复 key）。

### 方案 B：运行期 `MetadataValue` 标签联合

```clux
union MetadataValue {
    s: str;
    i: i64;
    b: bool;
    f: opaque;
}
var m: ?MetadataValue = get_metadata[User]("level");
```

- 支持动态 key、缺失返回 nil；
- 缺点：调用点必须按 kind 解包；C 后端要生成联合体/标签；
- 与“元信息是全局变量”目标相比多一层运行期类型擦除。

**建议：先做方案 A。** 它最符合 C 后端全局变量的语义。

可选补充：

```clux
has_metadata[User]("name")
has_field_metadata[User]("x", "unit")
```

让“是否存在”也保持编译期。

---

## 3. 装饰器处理

### 3.1 内建 `metadata` 标记

`[[metadata("key", value)]]` 不是普通包装装饰器，而是**编译期标记**：

- desugar/装饰器展开阶段识别并摘除；
- 不能像普通装饰器那样生成 wrapper；
- 可以和普通装饰器共存：
  ```clux
  [[metadata("role", "handler")]]
  [[trace]]
  func run(...) { ... }
  ```
  `trace` 仍正常包装，`metadata` 只登记元信息。
- 同一实体多个 metadata 装饰器允许，key 重复报错。

### 3.2 可挂载位置

目标：

- 类型：`struct` / `union` / `cunion` / `enum` / `bitfield` / `type` 别名；
- 字段：struct/union/cunion/bitfield 的字段；
- 方法：struct 内的 `func` 成员。

当前 parser 的限制：

- 顶层装饰器只允许 `var` / `func`；
- struct 成员循环只在 `var` / `func` 前解析装饰器；
- 普通字段声明前没有装饰器槽位。

需要扩展：

- `ast_struct_field_t` 增加 `decorators`；
- `ast_union_def_t` / `ast_cunion_def_t` / `ast_bitfield_def_t` 字段同理；
- 类型定义节点增加 `decorators`；
- 顶层 struct/enum/union 分支接收装饰器，而不是报错。

---

## 4. 元信息值模型

编译期值可以复用 `sema_ct_const_t`：

- 整数 / float / bool / str；
- 函数引用（`func_id`，含无捕获函数）；
- 数组常量已有编码能力；
- 带捕获的闭包也有编码，但 C 后端全局常量比较麻烦。

建议第一版允许：

- `i8..i64/u8..u64/usize`
- `f32/f64`
- `bool`
- `str`
- 无捕获函数值
- 数组（可选）

限制：

- 不支持带捕获闭包作为 C 全局元信息（环境不是全局常量）；
- 不支持运行期计算的值；
- 值的类型必须完整、可赋给全局常量。

如果未来 C 后端要求更严格，可以在 sema 报“metadata value must be
a global constant”。

---

## 5. 存储模型

### 5.1 编译期

在类型/字段/方法的元数据表中保存：

```c
typedef struct clux_meta_entry {
    strslice_t     key;
    sema_ct_const_t value;
} clux_meta_entry_t;
```

键空间：

- 类型：`type_t* + key`
- 字段：`type_t* + field_name + key`
- 方法：`type_t* + method_name + key`

建议挂在 `sema_type_t` 上：

```c
meta_table_t *type_meta;
meta_table_t *field_meta;   // (field_name, key) -> value
meta_table_t *method_meta;  // (method_name, key) -> value
```

这样跨模块、泛型实例都能按具体类型对象查。

### 5.2 运行期 / C 后端

每个条目直接对应一个全局常量：

```c
/* 类型元信息 */
static const int32_t __clux_meta_T_hash_level = 3;
/* 字段元信息 */
static const char *const __clux_meta_T_hash_x_unit = "px";
/* 方法元信息：函数指针 */
static clux_fn_t __clux_meta_T_hash_run_handler = { .fn = ..., .env = NULL };
```

`get_metadata[T]("level")` 编译为：

- VM 阶段：`LOAD_METADATA <meta_id>`（或直接 `PUSH` 常量）；
- C 后端：直接引用 `__clux_meta_...`；
- 没有运行期元信息表。

如果条目值不是全局可表示（例如带捕获闭包），编译期拒绝。

---

## 6. API 解析规则

### 6.1 必须静态

```clux
get_metadata[User]("level")
```

- `User` 必须是具体类型（泛型函数内是当前实例化后的 T）；
- `"level"` 必须是编译期字符串常量（建议只接受字符串字面量）；
- 缺失条目 → 编译错误（方案 A）。

### 6.2 字段/方法查找

```clux
get_field_metadata[User]("x", "unit")
get_method_metadata[User]("run", "role")
```

- 字段/方法名也必须是编译期字符串；
- 字段不存在 / 没有该 key → 编译错误；
- 继承/嵌入字段：
  - 建议沿 embeds 链查找；
  - 如果多个基类同名歧义 → 报错。
- 泛型类型：
  - `Box[i32]` 与 `Box[str]` 是不同 owner；
  - 元信息在实例化时求值并登记。

---

## 7. 泛型与装饰器

### 7.1 `struct Box[T]` 上的 metadata

```clux
[[metadata("size", sizeof(T))]]
struct Box[T] { ... }
```

- 值可能依赖 T，不能在定义点只求一次；
- 每个具体实例 `Box[i32]` 在 sema_generic 中求值一次；
- `get_metadata[Box[i32]]("size")` 得到 4。

### 7.2 泛型方法上的 metadata

```clux
struct Box[T] {
    [[metadata("kind", T.name)]]
    func get(self: ref *Box[T]): T { ... }
}
```

- 每个实例的方法元信息独立；
- 泛型方法自身的实例化再按具体类型参数展开；
- 需要把 metadata 从原方法节点带到实例方法节点。

### 7.3 普通函数/变量

用户当前只要求类型/字段/方法；顶层函数/变量 metadata 可以作为后续扩展。

---

## 8. 现有架构的接入点

### 8.1 Parser

- `parse_decorator_prefixes` 已能解析 `[[expr]]`；
- 扩展类型/字段节点保存 decorators；
- 顶层 `struct` / `enum` / `union` / `cunion` / `bitfield` / `type` 分支把 decorator 前缀传给节点。

### 8.2 Desugar

- 在普通装饰器包装前识别 `metadata(...)`；
- 类型/字段的 metadata 不包装；
- 方法上的 metadata 与普通装饰器共存时，仅摘除 metadata，剩余装饰器继续走 wrapper；
- 泛型类型/方法的 metadata 不能定义点丢弃，需要保留到 sema_generic 实例化。

### 8.3 Sema

- 类型完成/密封后求值 metadata 表达式；
- CTFE 求值 value；
- 登记到 `sema_type_t` 的元信息表；
- 泛型实例在 `sema_generic` 中完成相应登记；
- 实现 API 的内建查找（`get_metadata` 等），返回具体类型 shadow；
- 字符串 key 必须是字面量 / 编译期常量。

### 8.4 Compiler

- 为每条元信息分配稳定 id / 全局符号名；
- `get_metadata[...]` 编译成常量读取；
- C 后端直接引用全局变量；
- 如果值类型含函数，需保存函数对象/函数指针。

---

## 9. 主要难点

1. **返回值类型**：如果不用方案 A，需要设计 `MetadataValue` 标签联合。
2. **装饰器与 wrapper 的共存**：metadata 必须从普通装饰器链中剥离，不能生成 wrapper。
3. **泛型实例的 metadata**：值可能依赖 T，必须在每个实例求值，不能缓存定义点值。
4. **C 后端常量表示**：函数/数组/闭包作为全局常量的 ABI。
5. **字段装饰器解析**：当前字段没有 decorators 槽位。
6. **继承字段查找**：跨 embeds 的归属与歧义。
7. **元信息 key 重复/缺失**：需要明确诊断规则。

---

## 10. 建议的第一版范围

推荐先做：

- 方案 A（编译期定位、返回条目静态类型）；
- `struct` 类型 metadata；
- struct 字段 metadata；
- struct 方法 metadata；
- 值类型：整数 / bool / str / 无捕获函数；
- key 与字段/方法名必须是字符串字面量；
- 缺失/重复编译错误；
- 泛型实例按具体 T 求值；
- C 后端全局常量模型先设计好符号命名，不在当前 VM 阶段做 C 代码生成。

暂不做：

- 运行期动态 key；
- `MetadataValue` 标签联合；
- 带捕获闭包作为全局元信息；
- 运行时修改元信息。

---

## 11. 类型装饰器（补充调研）

类型装饰器当前尚未实现，其原理是**一个带泛型参数的 comptime 函数**：

```clux
comptime func LogName[T](): void {
    // 编译期执行，T 是被装饰的类型
}

[[LogName]]
struct User { ... }

[[LogName]]
type Alias = User;
```

语义建议：

1. 装饰器是编译期钩子，不生成运行期 wrapper，也不生成运行期代码；
2. `T` 绑定为**被装饰的具体类型**；
3. 在类型完成/密封之后调用；
4. 返回类型必须是 `void`；
5. 可以带普通参数：
   ```clux
   [[LogName("prefix")]]
   struct User {}
   ```
   即调用 `LogName[User]("prefix")`；
6. 多个类型装饰器按源码顺序执行；
7. 泛型类型按**每个具体实例**执行：
   ```clux
   [[LogName]]
   struct Box[T] { ... }
   ```
   `Box[i32]` 和 `Box[str]` 各执行一次 `LogName[Box[i32]]()` /
   `LogName[Box[str]]()`，不是在定义点执行一次；
8. 装饰器函数必须是 `comptime func`，且至少有一个未显式传入的类型参数；
9. 重复的类型装饰器应用需要按类型 + 装饰器做去重/记忆化，避免实例化
   过程中重复执行。

### 11.1 与类型别名

clux 的 `type Alias = Target` 当前是**透明别名**，不产生新的运行期类型身份。
因此：

```clux
[[LogName]]
type Alias = User;
```

应理解为对 `User` 执行 `LogName[User]()`，而不是对一个新的 `Alias`
类型执行。由此推论：

- 类型别名上的装饰器只能作用于底层类型；
- 多个别名装饰同一底层类型时，副作用会按声明顺序重复执行；
- 无法为不同别名保存不同的元信息（因为运行期只有 `User`）；
- 如果需要“别名自己的元信息”，需要先把 `type` 改成 nominal alias
  （新类型身份），这是另一个语言特性。

第一版建议明确文档化“透明别名”语义，而不是静默按别名名做独立存储。

### 11.2 与 metadata 的关系

类型装饰器和 `[[metadata(...)]]` 是两层机制：

- `[[metadata("k", v)]]` 声明不可变元信息；
- `[[LogName]]` 是任意的编译期逻辑（日志、校验、生成其他编译期状态）。

如果希望类型装饰器也能写入元信息，需要额外的编译期 API，例如：

```clux
set_metadata[T]("key", value);
```

并且只能在 comptime 上下文调用。当前用户给出的 API 只有 get，因此第一版
建议保持两者独立，不开放 `set_metadata`。

### 11.3 与 metadata 的执行顺序

如果同一个类型上同时有：

```clux
[[metadata("role", "handler")]]
[[LogName]]
struct User {}
```

建议：

1. 先完成类型密封；
2. 按源码顺序执行装饰器；
3. `metadata` 条目先登记，随后 `LogName` 才能通过
   `get_metadata[User]("role")` 读到；
4. 或者反过来要求用户显式排序。需要明确固定规则，避免歧义。

### 11.4 C 后端

类型装饰器只在编译期执行，不产生 C 代码；它产生的 metadata 最终仍然
是全局常量。如果允许装饰器写 metadata，C 后端只需要保证写入发生在
全局常量生成之前。

### 11.5 类型装饰器的风险

- 装饰器函数在编译期执行，错误应该定位到类型定义位置；
- 泛型实例可能产生大量重复调用，需要 memoize；
- 装饰器可以触发其他类型实例化，需防止递归循环；
- 装饰器不能修改已密封类型的字段布局，否则会破坏已生成的代码；
- 装饰器的副作用必须是确定性的，否则 C 后端/增量编译结果不稳定。

---

## 12. 待确认

1. 是否接受方案 A（返回条目自己的静态类型、key 必须编译期常量、缺失编译错误）？
2. 是否需要 `has_metadata` / `has_field_metadata`？
3. metadata 是否允许挂在 enum/union/bitfield/type alias 上，还是先只做 struct？
4. 函数元信息是否允许闭包，还是仅无捕获函数？
5. 继承/嵌入字段的 metadata 查找规则？
6. 类型装饰器的 `T` 是否就是被装饰的具体类型，并且泛型类型按每个实例执行？
7. 透明类型别名上的装饰器按底层类型执行是否可接受？
8. 是否需要 `set_metadata[T]`，让类型装饰器可以写入元信息？
9. 类型装饰器是否允许带普通参数（`[[LogName("x")]]`）？
