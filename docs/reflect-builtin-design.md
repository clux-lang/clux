# clux 反射 builtin 注入设计（v0 草案，2026-10）

> **依据**：[reflect-design.md](reflect-design.md) 只描述了概念（泛型 `TypeInfo[T]`、
> "builtin 关键字"），并不是针对当前类型系统的设计。本文按**现在**已有的类型结构
> 与编译期机制（`type` 值、`comptime func`、`comptime foreach`、CTFE、sema 折叠）
> 给出可实施的设计：**编译器注入什么、以什么形状注入、在哪里求值**。
>
> 本文不含 `std/Reflect` 库的最终 API 形态（那需要 §7 的前提先补齐）。

---

## 0. 结论（三个决策）

| 决策 | 选择 | 理由 |
|---|---|---|
| **注入面** | `T.<key>`——把反射元数据作为 **type value 的成员**注入 | 零关键字增长；type 值已是一等公民；与 `T::x`（struct 静态成员）天然分离；新增 key 只是 sema 表里加一行 |
| **求值** | **sema 折叠**为常量（`str` / `usize` / `type` / `bool`），CTFE 共用同一实现 | 与 `sizeof`/`typeof` 同族：运行期零指令、零元数据存储 |
| **值域** | `str` / `usize` / `type` / `bool`；计数+索引取代数组 | 与 `sizeof` 返回 `usize` 一致；`[N]str` 需要 CTFE 构造数组常量（见 §6） |
| **kind / ownership** | 返回**编译期字符串**（`"struct"` / `"own"` …），不引入内建枚举 | 字符串天然自描述（打印/比较/诊断都能直接用），且不必为反射新增类型 |
| **type 的传递** | **走泛型**（M7），不做 `comptime func(T: type)` | 用户决策：type 应由泛型传递而不是参数 |

`T.<key>` 只对 **type value** 生效（`T` 是类型名或 `typeof(x)` 之类的类型值表达式），
因此不影响实例字段访问 `p.x` 与结构体静态成员 `T::x`。

---

## 1. 当前类型系统能提供的元数据（事实表）

以下全部来自 `include/vm/type.h` 与各 `type_*.c` 的访问器，**不需要新增运行时数据**：

| kind | 元数据 | 来源 |
|---|---|---|
| 所有类型 | `name`（规范名）、`size`、`align`、`kind` | `type_t.name/size/align/kind` |
| struct | `field_count`、每字段 `name / offset / type / ownership` | `struct_type_t.fields[]`（`struct_field_t`） |
| struct（组合） | `embed_count`、每个 `embed_type(i)` | `struct_type_t.embeds[]`、`embed_count` |
| struct（成员） | `method_count/method_name(i)`、`attribute_count/attribute_name(i)/attribute_type(i)` | `struct_type_t.methods`（vec，登记序）、`attributes`（strmap，**无序**）、`member_scope` |
| tuple | `elem_count`、每元素 `type / offset / ownership` | `tuple_type_t.elems[]` |
| enum | `variant_count`、每变体 `name / value`、`underlying` | `enum_type_t.variants[]`、`underlying` |
| union（tag） | `member_count`、每 member `name / tag / payload_type / ownership` | `union_type_t.members[]` |
| cunion | `member_count`、每 member `name / type / ownership` | `cunion_type_t.members[]` |
| func | `param_count`、每参数 `type / ownership`、`return_type / return_ownership`、`is_variadic` | `func_sig_t` |
| cfunc | 同 func，另 `is_c_abi = true` | `cfunc_type_t.sig` |
| array | `elem_type`、`len`、`elem_ownership` | `array_type_t` |
| slice | `elem_type`、`elem_ownership` | `slice_type_t` |
| ptr | `target_type`、`rc`（0 普通 / 1 share / 2 weak） | `ptr_type_t.base_type/rc` |
| option | `inner_type`、`inner_ownership` | `option_type_t` |
| const / volatile | `unqualified`（剥限定后的类型） | `const_type_t.sub` / `volatile_type_t.sub` |

**注意**：所有权（own/ref/fatal）是**槽位属性**，存在 `struct_field_t.ownership`、
`tuple_elem_t.ownership`、`func_sig_t.param_ownerships[]` 等处；`type_t` 本身不带
所有权。反射要暴露它，只能取这些**槽位**上的值，不能问"这个类型是不是 own"。

---

## 2. 注入机制选型

### 方案 A：新增关键字（reflect-design.md 的原始设想）

`typename(T)` / `field_count(T)` / `field_name(T, i)` …每个都是像 `sizeof` 一样的
关键字：parser 建专用 AST 节点，sema/CTFE 求值。

- ✅ 与 `sizeof`/`typeof` 完全同构，语义最直白。
- ❌ 关键字 +20：`typename`、`field_count`、`field_name`、`field_type`、
  `field_offset`、`methods`、`attributes`、`variant_count`… 全部变成保留字，
  用户代码里的同名标识符立刻失效；词法/语法/诊断/文档全要同步。
- ❌ 不可扩展：以后加一个元数据（例如 `field_ownership`）就要再动语言关键字。

### 方案 B：宏函数（`make(T, N, …)` 的做法）

parser 识别固定名字，第一个实参按**类型表达式**解析（`make` 已有先例：

```
make(T, N, init...)   // T 是元素类型表达式
```

）。

- ✅ 语法像函数，宏函数表集中管理。
- ❌ 仍需 +20 个保留名（`make` 是 `comptime` 关键字族的一员，用户不能重名）；
  与方案 A 的关键字污染问题相同，只是换了个位置。

### 方案 C：type value 的成员注入 —— **推荐**

把 key 作为**每个 type value 的成员**：`T.name`、`T.field_count`、`T.field_type(i)`。

- ✅ **零关键字增长**：`name`/`size`/`field_count` 等仍是普通标识符；只有"接收者
  是 type value"时才按反射 key 解释。
- ✅ 与既有面天然分离：
  * `p.x` —— 实例字段（接收者是值）；
  * `T::x` —— struct 静态成员（`::` 显式）；
  * `T.x` —— **反射**（接收者是 type value）。
  三者语法上互不冲突，也不需要改任何现有语义。
- ✅ 可发现性好：`Point.fild_count` 的报错可以列出该类型可用的全部 key。
- ✅ 扩展成本 = sema 的 key 表加一行；内建类型（`i32.name`）、匿名类型
  （`typeof(x).size`）、用户类型（`Point.field_type(0)`）统一处理。
- ⚠️ 需要 sema 在 `ex_member` 里对 `TYPE_KIND_TYPE` 接收者做特殊解析
  （现在报 `cannot access field of value of type type`）。

**选 C**。原始文档的 `field_count(T)` 形状可以在库层补：等 type 值透传补齐（§7）后，
`std/Reflect` 里写 `comptime func count(T: type): usize { return T.field_count; }`
即可得到文档里的 API。

---

## 3. key 表（v0）

**通用（所有 kind）**

| key | 返回 | 语义 |
|---|---|---|
| `name` | `str` | 规范类型名（`i32` / `[2]i32` / `Point` / `func(i32): i32`） |
| `size` | `usize` | 与 `sizeof(T)` 一致 |
| `align` | `usize` | 与 `alignof(T)` 一致 |
| `kind` | `TypeKind` | 内建 enum（见 §3.9） |

**struct**

| key | 返回 | 语义 |
|---|---|---|
| `field_count` | `usize` | 字段数（**含**组合插入的嵌入字段，与 `fields[]` 一致） |
| `field_name(i)` | `str` | 第 i 个字段名（声明序） |
| `field_type(i)` | `type` | 第 i 个字段类型（type value） |
| `field_offset(i)` | `usize` | 第 i 个字段偏移 |
| `embed_count` | `usize` | `...Base;` 嵌入数 |
| `embed_type(i)` | `type` | 第 i 个嵌入类型 |
| `method_count` | `usize` | 方法数（`methods` vec，登记序） |
| `method_name(i)` | `str` | 第 i 个方法名 |
| `method_type(name)` | `type` | 按名取方法签名类型（`T::f` 的 func 类型） |
| `attribute_count` | `usize` | 静态属性数（`attributes` strmap，**按名排序**输出） |
| `attribute_name(i)` | `str` | 第 i 个属性名（排序后） |
| `attribute_type(i)` | `type` | 按排序下标取属性类型 |

**enum**

| key | 返回 | 语义 |
|---|---|---|
| `variant_count` | `usize` | 变体数 |
| `variant_name(i)` | `str` | 变体名 |
| `variant_value(i)` | `i64` | 变体整数值 |
| `underlying` | `type` | 底层整型 |

**union（tag union）**

| key | 返回 | 语义 |
|---|---|---|
| `member_count` | `usize` | member 数 |
| `member_name(i)` | `str` | member/tag 名 |
| `member_type(i)` | `type` | payload 类型 |
| `member_tag(i)` | `usize` | tag 编号 |

**cunion**：`member_count` / `member_name(i)` / `member_type(i)`（无 tag）。

**func / cfunc**

| key | 返回 | 语义 |
|---|---|---|
| `param_count` | `usize` | 形参数 |
| `param_type(i)` | `type` | 第 i 个形参类型 |
| `return_type` | `type` | 返回类型（void 也返回 `void` 的 type value） |
| `is_variadic` | `bool` | 变参 |
| `is_c_abi` | `bool` | `cfunc(...)` 为 true，`func(...)` 为 false |

### 3.10 为什么**不**暴露槽位所有权（用户修正）

初版设计打算暴露 `field_ownership(i)` 等 key，理由写的是"反射生成的销毁序列需要
知道字段是不是 own"。这个理由**不成立**（用户指正）：

1. **所有权是类型的一部分**。`struct_field_t.ownership` 参与结构体类型的 intern
   去重：`struct N { next: own *Node }` 与 `struct V { next: ref *Node }` 是**两个
   不同的类型**。同一个类型不可能既有 own 槽位又有 ref 槽位——所以"这个字段是不是
   own"**完全由类型决定**，不是值的运行期状态。
2. **释放序列由编译器从类型生成**（`gen_destroy_impl` / `type_has_own_slots`），
   调用点无需任何元数据：`FREE`/销毁语句在编译期就定死了。
3. 因此反射再暴露一遍槽位所有权是**冗余信息**：任何需要它的编译器路径（cfunc→func
   包装的 `move(p)`、切片逐元素 FREE、结构体销毁）都直接读类型，而不是读反射。

结论：**v0 不暴露 `*_ownership` key**。若将来某个用户级库确实需要它（例如生成
"逐字段拷贝" 的泛型代码时判断要不要 `move`），那是**泛型 + 类型级查询**的范畴，
等 M7 再议；届时它也只是 `T` 的又一个 key，加回来是一行表项。

**array**：`elem_type``elem_type` / `elem_ownership` / **`length`**（注意不是 `len`——`len` 是关键字，`T.len` 过不了 parser）
**slice**：`elem_type`
**ptr**：`target_type` / `rc`（`0` 普通、`1` share、`2` weak）
**tuple**：`elem_count` / `elem_type(i)` / `elem_offset(i)`
**option**：`inner_type`
**const / volatile**：`unqualified`（剥限定后的类型）

### 3.9 kind / ownership 返回**编译期字符串**（不引入内建枚举）

**决策**：不新增 `TypeKind` / `Ownership` 类型；两者直接返回 `str`，值就是
`type_kind_t` / `ownership_t` 的小写名。字符串是自描述的：打印、比较、拼进诊断/
代码生成都能直接用，也不需要为反射引入新的语言类型。

`T.kind` 的取值（与 `type_kind_t` 一一对应）：

```
"void" "bool" "int" "float" "str" "type" "func" "error" "interrupt"
"const" "volatile" "option" "struct" "array" "tuple" "enum" "union"
"cunion" "ptr" "slice" "opaque" "module" "cfunc"
```

判断种类用普通字符串比较即可：

```c
if (T.kind == "struct") { … }          // 编译期折叠成常量比较
```

> 为什么不用数值：把 kind/ownership 暴露成数字会逼用户记 0..22 的映射，
> 也会把内部枚举顺序变成 ABI；字符串没有这两个问题。若将来确实要 switch，
> 字符串 case 也是现成可用的。

---

## 4. 求值规则

1. **位置**：`ex_member` 处理 `object.field` 时，若 `object` 的静态类型是
   `TYPE_KIND_TYPE`（type value）且 `field` 命中 key 表 → 走反射折叠；
   否则维持现有诊断。`T.field(i)` 形式（key 带参数）由 `ex_call` 的 member-call
   分支识别（callee 是 `AST_MEMBER`、接收者是 type value、member 名是带参 key）。
2. **实参**：索引（`usize`）与名字（`str`）实参必须是**编译期常量**——用 CTFE
   求值（`ctfe_eval`）；非常量 → 报错 `reflection index must be a compile-time constant`。
3. **折叠产物**（sema 就地替换，compiler 零感知）：
   * `str`（含 `kind` / `*_ownership`）→ `AST_STR_LIT`
   * `usize` → `AST_INT_LIT`（type = `usize`）
   * `type` → `AST_TYPE_REF`（指向已登记类型；`sema_type_register`）
   * `bool` / `i64` → 对应字面量
   * `type` 结果可直接**链式**继续反射：`Point.field_type(1).name`、
     `Point.field_type(0).size`——`field_type(i)` 折叠成类型引用后，外层
     `.<key>` 又是同一个折叠路径。
   * 注意：type 值**不能存进变量**（"a type is not a runtime value"）——绑定一个
     反射出来的类型用 **`type` 定义**，这也是 clux 里"给类型起名"的唯一方式：

     ```c
     type F0 = Point.field_type(0);     // 绑定第 0 个字段的类型（编译期）
     print(F0.name);                    // "x"
     print(F0.size);                    // 4
     ```

     三种消费形态等价且可混用：`sizeof(T.field_type(i))`、`type Alias = T.field_type(i);`、
     链式 `T.field_type(i).name`。
   * **别名透明**：`type T = i32;` 之后 `T.name` 是 `"i32"`、`T.kind` 是 `"int"`
     （反射看到的是别名指向的底层类型，不是别名本身的名字）——与 `sizeof(T)`、
     `typeof` 的语义一致。
4. **CTFE 同源**：把 key 表与取值逻辑放在 `src/sema/reflect_meta.c` 的纯函数里
   （输入 `const type_t *` + key + 实参，输出"常量描述"），sema 折叠与 CTFE 求值
   都调它——避免两套实现漂移（`type_rhs_uses_var` / `type_has_ct_op` 各自手写下钻
   曾经就漂移过）。
5. **运行期**：不产生指令。`T.size` 在运行期可见的位置与 `sizeof(T)` 完全一致
   （折叠成字面量，随便用）。
6. **诊断**：
   * 未知 key：`'Kind' has no reflection member 'xxx'; available: name, size, …`
   * key 不适用于该 kind（例如对 `i32` 取 `field_count`）：
     `'i32' (Int) has no 'field_count' (only struct has fields)`
   * 越界：`field index 3 out of range (Point has 2 fields)`
   * 按名查询未命中：`no field named 'z' in 'Point'`

---

## 5. 边界与规则

| 情形 | 规则 |
|---|---|
| `const T` / `volatile T` | `kind` 如实返回 `"const"` / `"volatile"`（自描述，否则这两个 kind 永远不可见）；`name/size/align` 按限定形态显示/计算（`size` 与底层相同）；**kind-specific key 自动穿透限定**（`const Point` 上取 `field_count` 等价于 `Point.field_count`），另给 `unqualified` 取剥限定后的类型 |
| `?T` | 有独立 kind（Option）；`inner_type` / `inner_ownership`；`name` 为 `?i32` |
| `func(...)` vs `cfunc(...)` | 两个 kind（Func / Cfunc）；`is_c_abi` 区分 |
| struct 组合（`...Base;`） | `fields[]` 已含嵌入字段（组合期插入），`field_count` 与之一致；`embed_*` 额外给出嵌入类型表 |
| struct 属性/方法 | 属性是无序 strmap → **按键名排序**输出（确定性）；方法用 `methods` vec 的登记序。继承（嵌入）成员不复制进 `attributes`，`T::f` 查找才沿 `embeds` 递归——反射**不展开继承**（v0，避免歧义）；后续可加 `inherited_*` |
| 匿名/匿名构造类型 | `name` 可能是 intern 生成的规范名（如 struct 字段表拼接名），照实返回 |
| `type` 值的成员被用户结构体静态成员同名占用 | 不冲突：`T::x` 走静态成员，`T.x` 走反射。若将来要访问"类型自身的静态属性"用 `T::x`，语义不变 |
| 反射 key 与实例字段同名 | 不冲突（接收者 kind 不同） |

---

## 6. 与 `comptime foreach` / 数组的关系

`reflect-design.md` 里 `methods(T)` / `attributes(T)` 返回 `[N]str`。当前：

- `[N]str` 作为**编译期常量数组**需要 CTFE 构造数组值 + `sema_type_register`，
  目前 CTFE 能构造的类型集合里没有"字符串数组常量"这条路（`make(...)` 是运行期
  分配）。**v0 用 `*_count` + `*_name(i)` 取代数组**，遍历用普通 `for`：
  ```c
  var i: usize = 0usize;
  while (i < Point.field_count) { print(Point.field_name(i)); i = i + 1usize; }
  ```
- 以后若补上"CTFE 数组常量"，可再加 `field_names: [N]str`，并用已实现的
  `comptime foreach` 展开——两者不冲突，`*_count/*_name(i)` 仍是底层原语。

---

## 7. type 的传递：走泛型，不走 comptime 参数（用户决策）

**决策**：`comptime func` **不**接受 `type` 参数；类型只能由**泛型**传递
（M7）。因此不存在"补 type 值透传"这一阶段，反射库层等泛型落地后再写。

作为对照，当前实现里 type 参数确实不成立（实测）：

```c
comptime func width_of(T: type): usize { return sizeof(T); }
width_of(i32)    // → 8（错！i32 是 4）——type 参数在 CTFE 调用里丢掉了类型身份
comptime func same(T: type): type { return T; }
                 // → error: cannot return type from function returning type
var t: type = i32;   // → error: a type is not a runtime value
```

这不是要修的"缺口"，而是方向选择：类型信息走泛型（M7 的 monomorphization，
每个实参类型各特化一份），反射 API 就是泛型库：

```c
// M7 泛型落地后的形状（与 reflect-design.md 一致，只是不再假装用 comptime 参数）
struct TypeInfo[T] {
    func field_count(): usize { return T.field_count; }   // T.<key> 在特化后的
    …                                                     // 函数体里是具体类型
}
```

**在泛型落地之前**，`T.<key>` 本身就是可用的反射接口——调用点直接写
`Point.field_count` / `p.x` 所在作用域里的类型名即可，不经过任何库层。
P0/P1 因此**不依赖**泛型；只有 P3（`std/Reflect` 库）需要等 M7。

---

## 8. 实施阶段

| 阶段 | 内容 | 依赖 |
|---|---|---|
| **P0** | sema 的 `T.<key>` 识别（通用 key：`name`/`size`/`align`/`kind`）+ 折叠为常量；诊断脚手架 | 无 |
| **P1** | struct（字段/嵌入/方法/属性）、enum、union、cunion、tuple、array、slice、ptr、option、func/cfunc 的 key 全套；`*_ownership` 返回 `str` | 无 |
| **P2** | 链式类型值（`Point.field_type(1).name`）、`type Alias = T.field_type(i)`、`sizeof(T.field_type(i))` 的端到端验证 | P0/P1 |
| **P3** | `std/Reflect` 库（`TypeInfo[T]` / `StructInfo[T]` / `FieldInfo[T, idx]`，形状见 reflect-design.md） | **M7 泛型** |

P0/P1/P2 不依赖任何未实现的语言特性，可以立刻做；P3 等泛型。

---

## 9. 已定决策（2026-10，用户确认）

| # | 决策 | 结果 |
|---|---|---|
| 1 | 注入面 | **C：type value 成员注入 `T.<key>`** |
| 2 | 元数据集合形状 | **计数 + 索引**（`field_count` / `field_name(i)`），不用 `[N]str` 数组 |
| 3 | 属性枚举顺序 | **按名排序**（确定性） |
| 4 | 所有权元数据 | **不暴露**（用户修正）：所有权是类型的一部分（参与 intern），释放序列由编译器从类型生成，反射再暴露一遍是冗余信息；详见 §3.10 |
| 5 | kind / ownership 的表示 | **编译期字符串**（自描述），不引入内建枚举 |
| 6 | 命名风格 | **snake_case**（`field_count`，与 reflect-design.md 一致） |
| 7 | type 的传递 | **走泛型（M7）**；`comptime func` 不接受 `type` 参数 |
