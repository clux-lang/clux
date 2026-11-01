# 魔法方法设计（M8）

状态：设计稿。`docs/` 是规范树，本文件属于实现设计；落地后语义并入
`docs/whitepaper/03-declarations.md`（声明规则）与
`docs/whitepaper/05-expressions.md`（运算符派发）。

## 1. 目标

用户 `struct` 在语言原生语法位置（运算符、下标、`len`）表现如内建类型。
实现上**全部降级为普通实例方法调用**：

- sema 一处改写，C 后端 / CTFE / 泛型实例化 / 调试信息零特判；
- 魔法方法就是普通方法，因此显式调用 `p.__add__(x)` 合法，且遵循普通方法
  规则（接收者可自动解引用）。

非目标（P0）：自定义字面量、调用运算符 `x(args)`、隐式转换、对称派发
（`2 + v`）、`union` 上的魔法方法（`union` 目前没有方法）。

## 2. 方法模型（本设计的前提）

**方法与"静态方法"没有区别**：struct 里的 `func` 就是类型命名空间里的一个
普通函数，`self` 只是首参的约定名字，不是关键字，也没有声明级的"实例 /
静态"区分。

实例调用 `a.b(args)` 是纯语法糖：

```text
a.b(args)  ⇒  typeof(a)::b(a.&, args)      // a 是结构体值
a.b(args)  ⇒  typeof(a)::b(a,    args)      // a 已经是指针（传 a 本身）
```

因此：

- **不存在"接收者必须是指针"的声明规则**。`func b(self: T)` 是合法的普通
  函数（可以 `T::b(x)` 静态调用、按值传参）；但 `a.b()` 展开后传的是
  `a.&`（`*T`），把 `*T` 绑到值形参 `T` 会被**普通实参隐式转换**规则拒绝
  ——错误发生在调用点，不需要也不应该在声明处特判。
- `T::b(a, ...)` 静态调用不插入接收者，按普通函数调用的实参规则走。
- 魔法方法是同一套模型的延伸：`a + b` ⇒ `typeof(a)::__add__(a.&, b)`，
  `len(a)` ⇒ `typeof(a)::__len__(a.&)`，`a[i]` ⇒
  `(typeof(a)::__index__(a.&, i)).*`。派发 = 在类型命名空间里找一个函数，
  没有新的调用机制。

## 3. 命名与声明

- 方法名形如 `__add__`（双下划线前后缀）。
- parser：`__` 前缀在**所有位置**默认拒绝（现状），仅对 **struct 成员方法
  名**放行；变量 / 参数 / 类型名 / 全局函数名仍一律拒绝。
- sema：方法名必须在**已知魔法表**（§3.1）内，否则报
  `unknown magic method '__foo__'`——避免写出永不生效的方法。
- 签名不符（参数个数、`__eq__` 返回非 `bool` 等）在 sema 报错，不在后端。
- **魔法方法允许泛型**：clux 没有重载，泛型 + `comptime if` 就是重载的表达
  方式。派发点生成的是普通方法调用，会像任何泛型方法调用一样按实参推断
  并实例化：`a + 5` ⇒ `a.__add__(5.&)` 推断出 `U = i32`。泛型声明不做
  具体签名校验（形参还是类型参数）；调用点的普通调用规则负责实例化后的
  参数/返回检查。`__deref__` 的返回类型可解引用规则在泛型声明上同样跳过
  （无法静态判定）。
- 魔法查找**只看自身成员**（P0）：嵌入继承暂不参与魔法表；普通方法调用
  的继承行为保持不变。

## 3.1 魔法表（P0）

| 语法 | 方法 | 签名要求 | 备注 |
|---|---|---|---|
| `a + b` | `__add__` | `(self: *const T, rhs: *const R) -> V` | 结果类型 = 方法返回类型 |
| `a - b` | `__sub__` | 同上 | |
| `a * b` | `__mul__` | 同上 | |
| `a / b` | `__div__` | 同上 | |
| `a % b` | `__mod__` | 同上 | |
| `a & b` | `__and__` | 同上 | |
| `a \| b` | `__or__` | 同上 | |
| `a ^ b` | `__xor__` | 同上 | |
| `a << b` | `__shl__` | 同上 | |
| `a >> b` | `__shr__` | 同上 | |
| `-a` | `__neg__` | `(self: *const T) -> V` | |
| `!a` | `__not__` | `(self: *const T) -> bool` | 逻辑取反语义，强制 `bool` |
| `~a` | `__bnot__` | `(self: *const T) -> V` | |
| `a == b` | `__eq__` | `(self: *const T, rhs: *const R) -> bool` | 定义即覆盖逐字段比较；`a != b` 自动取反（无 `__ne__`） |
| `a < b` | `__lt__` | `(self: *const T, rhs: *const R) -> bool` | 各自独立，不互相推导 |
| `a <= b` | `__le__` | 同上 | |
| `a > b` | `__gt__` | 同上 | |
| `a >= b` | `__ge__` | 同上 | |
| `a[i]` | `__index__` | `(self: *const T, i: I) -> R`；`R` 必须**可解引用**（`*U` / `*const U` / 带 `__deref__` 的结构体） | 可写版本用 `self: *T` 且返回 `*U`；`a[i]` ⇒ `(a.__index__(i)).*` |
| `len(a)` | `__len__` | `(self: *const T) -> usize` | 与 `__index__` 组合即可被 `foreach` 遍历 |

魔法方法的 `self` 推荐写成 `*const T`（只读）或 `*T`（需要写穿时，如可写
`__index__`）。**这不是声明规则**：方法模型决定调用点传的是 `a.&`，值形参
`self: T` 会在调用点因 `*T → T` 不兼容而自然报错（§2）。
只读语义由返回类型/形参的 `const` 表达（比较 / `len` / 只读下标用 `*const T`；
`__index__` 要写穿时用 `self: *T` + 返回 `*U`，或 `self: *const T` + `*const U`）。

**`__index__` 的索引形参类型**：`foreach` 展开出的下标是 `usize`，所以想让
自定义类型可 `foreach` 就得写 `i: usize`；此时字面量下标要写 `b[1usize]`
（clux 的整数字面量不会隐式转 `usize`，`var x: usize = 1;` 本身也是错误）。
若更看重 `b[1]` 的字面量写法，可以写成 `i: i32`，但那样的类型不可 `foreach`。
这是**用户的选择**，编译器不加特判。

## 4. 派发规则

对语法位置 P 上的类型 `T`：

1. **内建类型优先**：标量 / 数组 / 切片 / `str` / 元组 / enum / 指针
   永远走既有语义，不查魔法表（`i32 + i32` 行为不变）。
2. **接收者必须是结构体**：`strip_quals(T)` 的 kind 为 `STRUCT` 才尝试
   魔法方法。**不解引用**（`*T` 不是 `T`）、**不解 optional**（`?T` 不是
   `T`）、**不取元素**（`[N]T` / `[]T` 不是 `T`）。
3. 命中 → 按方法模型展开为类型命名空间里的调用（与 `a.b()` 同一套糖）：
   `a[i]` → `(typeof(a)::__index__(a.&, i)).*`，`a + b` →
   `typeof(a)::__add__(a.&, b)`，`len(a)` → `typeof(a)::__len__(a.&)`；
   接收者表达式只求值一次。
4. 未命中 → 保持现有诊断（`cannot apply '+' to 'P'` / `cannot index value
   of type 'P'` / `len: argument is not a sequence`），文案附一句
   `(P defines no '__add__')` 之类的提示。
5. **方法体内没有特殊限制**：魔法方法体里的表达式按普通规则派发，不做
   "禁止二次派发"的检查。因为 `self` 是指针，`self == other` 是**指针比较**
   （指针不参与魔法派发）；要比较值必须显式写 `self.* == other.*`，此时
   运算符再次派发到 `__eq__` 是用户的显式选择（递归由用户负责，运行期
   无限递归 / CTFE 深度超限各按既有路径处理）。这条也适用于借泛型把
   比较托管给别的类型的写法，不限制表达。
6. 显式调用（`p.__add__(x)`、`p.__index__(0)`）按普通方法调用处理，
   **允许接收者自动解引用**——这是"函数调用"，与运算符派发是两套规则。
7. **只派发前导操作数（lhs）**：`a + b` / `a == b` / `a < b` 只看 `a` 的类型。
   不做对称派发、不做反向派发（没有 `__radd__` 之类）；`b` 的类型只用于
   匹配 `rhs` 形参。`b` 自己定义了同名魔法方法也不影响 `a` 是否派发。

### 4.1 下标：`__index__` 返回可解引用对象

`a[i]` 的语义就是 `(a.__index__(i)).*`：`__index__` 返回一个**指向元素**的
可解引用对象，实际的读/写由 `.*` 完成。

```clux
struct Buffer {
    items: []u8;

    // 只读视图：返回 *const u8 → a[i] 不可写
    func __index__(self: *const Buffer, i: usize): *const u8 {
        return self.items[i].&;      // 元素取址（clux 没有指针算术）
    }
    func __len__(self: *const Buffer): usize { return len(self.items); }
}

struct MutableBuffer {
    items: []u8;

    // 可变视图：返回 *u8 → a[i] = v 写穿
    func __index__(self: *Buffer, i: usize): *u8 {
        return self.items[i].&;
    }
    func __len__(self: *const MutableBuffer): usize { return len(self.items); }
}
```

- **读**：`a[i]` → `*(a.__index__(i))`；
- **写**：`a[i] = v` / `a[i] += v` → 同一个表达式，写穿到指针目标；
  返回 `*const U` 则由现有 const 规则拒绝（**只读 = 返回 const 指针**，
  不需要 `__set_index__`，也不再需要"可变下标"的第二种方法）；
- **左值链**：`a[i].field = v`、`a[i].m()` 都成立（`.*` 结果是左值）；
- **代理**：`__index__` 也可以返回带 `__deref__` 的结构体，`.*` 会继续
  穿透（与 §5.4 同一规则）；
- **只求值一次**：`a` 与 `i` 在改写后的表达式里各出现一次；
- **越界/未命中**：语言不做隐式检查，**由 `__index__` 自己决定是否检查**；
  选择检查就直接 `panic(...)`（与内建下标越界同路径），不想 panic 的调用方
  另用显式方法（如 `get(i) -> ?V`）——那与 `__index__` 无关，不改变 `[]` 语义。

### 4.2 比较：`__eq__` / `__lt__` / `__le__` / `__gt__` / `__ge__`

**签名**

```text
__eq__(self: *const T, rhs: *const T) -> bool   # 必须 bool
__lt__ / __le__ / __gt__ / __ge__           # 同上，必须 bool
```

- `rhs` 恰好一个参数，**按指针传**（零拷贝）：`a op b` ⇒
  `typeof(a)::__op__(a.&, b.&)`；`b` 不是左值时编译器物化临时副本再取址
  （C 后端 `&(T){v}` 复合字面量 / CTFE 的受跟踪副本）。`rhs` 指向的类型可以是任意
  `R`（例如 `func __eq__(self: *const P, rhs: *const str): bool`）。
- `self` 必须是 `*T` 或 `*const T`（魔法方法一律指针接收者，见 §3）；
  比较类方法用 `*const T`（不修改被比较对象）。

**派发**

- 只看**前导操作数**：`a == b` 检查 `a` 的类型；`a` 是 `T` 且 `T` 有
  `__eq__` → 改写为 `a.__eq__(b)`（`b` 按普通实参转换匹配 `R`）。
- `a` 没有 `__eq__` → 保持既有语义：`T == T` 逐字段比较，`T == U` 报错。
- `!=` 由 `__eq__` 取反；**没有 `__ne__`**。
- `<` `<=` `>` `>=` 各自独立，**不互相推导**（没有 `__cmp__`，也不从
  `__lt__` 推出 `__le__`）：少写一个就是"该语法不支持"。
- `.__eq__(...)` 显式调用是普通方法调用（可自动解引用）。

**聚合内部的相等必须调用 `__eq__`（不是 memcmp）**

`__eq__` 是**类型的相等定义**，凡该类型参与判等都要用它：

| 写法 | 行为 |
|---|---|
| `a == b`，`a: T` 有 `__eq__` | `a.__eq__(b)` |
| `s.a == s.b`，`s.a: T` 有 `__eq__` | `s.a.__eq__(s.b)` |
| `o1 == o2`，`?T` 且 `T` 有 `__eq__` | tag 相同后，载荷用 `T.__eq__` |
| `arr1 == arr2`，`[N]T` 且 `T` 有 `__eq__` | 逐元素用 `T.__eq__` |
| 结构体 `S` 的字段 `f: T`（`T` 有 `__eq__`） | `S` 的逐字段比较里，`f` 用 `T.__eq__` |
| tag union 载荷类型有 `__eq__` | 按 tag 取载荷后用 `T.__eq__` |
| `p == q`，`p: *T` | 指针比较（内建，不解引用） |

### 4.2.1 编译期如何决定「逐字段 / 整块 memcmp / 调方法」

每个类型在代码生成前归入一类，规则如下（C 后端 `clux_eq_*` 助手与 CTFE
必须给出**完全相同**的结果）：

| 类型 | 判等方式 |
|---|---|
| struct 且自身定义了 `__eq__` | **调用 `__eq__`**（整块传地址，不逐字段） |
| struct 无 `__eq__` | 逐字段，每个字段按本表递归 |
| `[N]T` | 逐元素，按 `T` 递归 |
| `?T` | 先比 tag，再按 `T` 递归载荷 |
| tag union | 先比 tag，再按当前成员的载荷类型递归 |
| `str` | 内容比较 |
| 标量 / 指针 / func 值 / cunion / slice / 位域 struct | 整块 `memcmp`（无 padding 风险或按位定义） |

要点：

- **struct 一律不整块 memcmp**：C 结构体的 padding 内容不确定，memcmp 会把
  未初始化字节算进相等性——这正是"逐字段"存在的理由；"整块 memcmp"只留给
  上面最后一行的类型类别。
- 判断"是否调 `__eq__`"发生在**类型层**（编译期），链式生效：外层 struct
  无 `__eq__`，但它有字段 `?T`、`[N]T`、以及 `T` 有 `__eq__`——外层逐字段、
  内层仍会调到 `__eq__`。
- **实现方式：sema 把 `==` 展开成对生成函数的调用**。聚合判等不是一个
  独立的"类型层钩子"，而是糖在类型结构上的递归展开：站点 `a == b`
  改写成 `__clux_eq_<typeid>(a.&, b.&)`，函数体按类型写成
  `a.f1 == b.f1 && ...` / nil 分支 / for 循环 / `is` 分支——这些 `==`
  在 walk 时**再次**走同一条规则，于是叶子上的 `__eq__` 就是普通方法调用：
  - 有实参（`x.&`）→ 泛型 `__eq__` 可以正常推断/实例化，聚合内部同样生效；
  - CTFE 与 C 后端都只看到普通函数调用与内建比较，没有第二套判等实现；
  - `__clux_eq_<id>` 是内部生成函数（static，按类型共享，惰性生成）。
- 允许在生成代码里写 `?T == ?T` / `[N]T == [N]T`（用户代码仍按原规则
  拒绝）：生成体需要它们来表达字段判等。

**递归与自身**

- 方法体内没有"禁止二次派发"的限制（§4 第 5 条）：`self == other` 是指针
  比较（`self` 是 `*const T`，指针不参与魔法派发）；要按值比较必须写
  `self.* == other.*`，那次 `==` 会正常派发到 `__eq__`——是用户的显式选择，
  递归后果由用户承担（与 C++ 运算符重载一致；CTFE 里表现为深度超限诊断）；
- 体内比较字段/聚合仍按 §4.2.1 的判定，遇到有 `__eq__` 的类型会调用它；
- 指针接收者不拷贝对象；值接收者不是魔法方法的合法形态。

**const**

`const T` 上只能用 `self: *const T`；`self: *T` 按现有 const 规则拒绝
（比较方法本来就应当是 `*const T`）。

**求值**

`a == b` → `a.__eq__(b.&)`：`a`、`b` 各求值一次，顺序不变；`b` 不是左值时
编译器自动物化（C 侧 `&(T){v}` 复合字面量，对象存活到所在块结束）。**仅
派发处**：用户代码的 `x.&` 仍要求可寻址（sema 拒绝 `(5).&`）。

## 5. `__deref__`：`.` 与 `.*` 的回退

### 5.1 适用范围

`__deref__` 重写 **`.` 成员访问**（读 / 写 / 调用三种形态）与 **`.*`
解引用运算符**：

| 语法 | 参与回退 |
|---|---|
| `a.x`（读字段 / 取方法） | ✅ |
| `a.x = v`（写字段） | ✅ |
| `a.m(args)`（调用方法） | ✅ |
| `a.*`（解引用） | ✅ 语义上就是 `__deref__` 的字面含义（见 §5.4） |
| `a.&`（取址） | ❌ 取址不是解引用 |
| `a[i]` | ❌ `__index__` 是独立运算符 |
| `len(a)` | ❌ `__len__` 是独立运算符 |
| `a + b` 等运算符 | ❌ 独立运算符 |
| `foreach (x of a)` | ❌ 走 `len` + 下标 |

推论：**智能指针类型要支持 `a[i]` / `len(a)` / 运算符，必须自己定义对应的
魔法方法**；`__deref__` 不会代理它们。`a.x` 的直接成员永远优先，因此
"没有 `->` 运算符"的缺口由显式 `a.__deref__().x` 关闭。

### 5.2 展开算法

```text
resolve_member(expr, name, mode ∈ {read, call, write}):
  visited = {}          # type_t* 集合（环检测）
  depth = 0
  t = type(expr)
  loop:
    core = strip_quals(t)
    if is_ptr(core):
        core = strip_quals(pointee(core))     # 既有指针成员访问语义
    if kind(core) != STRUCT:
        return 不展开（走现有诊断）
    if member_exists(core, name):
        return 命中（mode == write 且 depth > 0 时，要求当前 expr 类型为指针）
    if !has_member(core, "__deref__"):
        return 不展开（诊断附已尝试的链）
    if depth >= limit or core ∈ visited:
        return 诊断（环 / 超深）
    visited += core
    depth++
    expr = CALL(MEMBER(expr, "__deref__"))    # 普通方法调用，接收者只求值一次
    t = return_type(__deref__)
```

- `member_exists` 命中"字段或方法"任一即算命中：字段遮蔽同名方法时不会
  回退（沿用现有诊断 `cannot call 'm': it is a struct member variable`）。
- 每一步展开都是普通方法调用，因此 `p.__deref__()`（`p: *U`）也合法
  （显式调用允许自动解引用）。
- 链的末态是指针时，`.x` 由语言既有的指针成员访问自动解引用。

### 5.3 `__deref__` 的静态要求

```text
__deref__(self: T / *T / *const T) -> R
R 必须满足： type_is_ptr(R)
        或  kind(strip_quals(R)) == STRUCT 且 has_member(R, "__deref__")
```

- 不满足 → `'__deref__' must return a pointer or a type with '__deref__'
  (got 'X')`；
- 除 `self` 外不接受参数；`__deref__` 可以是泛型（与其它魔法方法一致），
  但返回类型必须可解引用的检查在泛型声明上跳过（实例化后的调用点仍有
  普通类型检查）；
- `const` 传播按普通方法规则（`self: *T` 给 `const T` 会被现有 const 规则拒绝，
  建议写 `self: *const T`）。

### 5.4 `.*`（解引用运算符）

`.*` 与 `__deref__` 是同一件事的两种写法，规则如下：

```text
resolve_deref(expr):
  t = type(expr)
  if is_ptr(strip_quals(t)):
      return 内建解引用（现有语义，不变）
  if kind(strip_quals(t)) == STRUCT 且 has_member(t, "__deref__"):
      # 改写表达式并**重新解析**：a.*  ⇒  (a.__deref__()).*
      expr = CALL(MEMBER(expr, "__deref__"))
      回到 resolve_deref(expr)          # 递归，受 §5.5 的环/深度约束
  return 诊断：cannot dereference value of type 'T'
                （若 T 无 '__deref__'，提示 "provide '__deref__' to make
                 '.*' work"）
```

- 语义：`a.*` 沿 `__deref__` 链一直走到**指针**，再对该指针解引用——即
  "取到最终被指向的值"。链为 `A -> B -> *C` 时 `a.*` 的类型是 `C`；
  想只取一步就写 `a.__deref__()`（得到 `B`）。
- 与 §5.2 的关系：两者共用同一条"结构体且有 `__deref__` 就展开"的循环，
  区别只是终止条件（`.` 等"成员命中"，`.*` 等"类型变成指针"）。
- 写入：`a.* = v` 展开后是 `(*(a.__deref__())) = v`，写穿到指针目标；
  `__deref__` 返回 `*const U` 时由现有 const 规则拒绝。
- 链上没有 `__deref__` 且不是指针 → 保持现有诊断，附
  `'T' defines no '__deref__'` 提示。

### 5.5 写入（`a.x = v`）

- 未发生展开：保持现有"可寻址左值"检查；
- 发生展开（`depth > 0`）：命中成员处表达式类型**必须是指针**，否则
  `cannot assign through '__deref__': 'x' is reached through a value, not a
  pointer`（写临时副本没有意义）。

### 5.6 环与深度（可配置）

- 环检测：`visited` 类型集合，命中即报
  `'__deref__' cycle: A -> B -> A`；
- 深度上限：**不写死在算法里**。默认常量
  `SEMA_MAGIC_DEREF_MAX_DEPTH_DEFAULT`（8），存放于
  `sema_t.magic_deref_max_depth`，按以下优先级覆盖：
  1. CLI `--magic-deref-depth=N`（`build` / `run`；0..31；驱动层入口
     `driver_set_magic_deref_depth`）；
  2. 环境变量 `CLUX_MAGIC_DEREF_DEPTH`；
  3. 默认常量。
- 超限诊断：`'__deref__' expansion exceeded the configured depth (N)`；
  配置为 `0` 表示禁用 `__deref__` 展开（`a.x` 与 `a.*` 均不再回退）。

### 5.7 诊断一览

| 场景 | 诊断 |
|---|---|
| 成员不存在且链已尽 | `'x' is not a member of 'T' (tried '__deref__' chain: T -> U)` |
| `.*` 无法解引用 | `cannot dereference value of type 'T' ('T' defines no '__deref__')` |
| 返回类型非法 | `'__deref__' must return a pointer or a type with '__deref__' (got 'X')` |
| 成环 | `'__deref__' cycle: A -> B -> A` |
| 超深 | `'__deref__' expansion exceeded the configured depth (N)` |
| 写穿失败 | `cannot assign through '__deref__': ...` |
| 多余参数 | `'__deref__' takes no arguments besides self` |

## 6. 与现有语法糖的关系

| 现有糖 | 与魔法方法的关系 |
|---|---|
| `x.?` / `x.!` | 不变（result 协议 `ok/value/error/panic` 保持现状） |
| `a[i]` | `(a.__index__(i)).*`；只有 `__index__` 即可，`__len__` 不是必需 |
| `foreach (x of a)` | 展开为 `len(a)` + `a[i]`：数组/切片/str 走内建；用户类型需 `__len__` + `__index__` 两个都定义 |
| `len(a)` | 内建序列优先；用户结构体查 `__len__` |
| `data(a)` | 不变（数组/切片/str） |
| 结构体 `==` | 未定义 `__eq__` 时保持逐字段比较 |

## 7. 实现落点

- **parser**：`parse_func_like` 增加 magic 允许位（默认 false），
  struct 成员方法位置传 true。
- **sema**：新增 `src/sema/magic.c` + `include/sema/magic.h`：
  - 魔法表与名字/签名校验（含"接收者必须是指针"）；
  - `sema_magic_try(sema, node, op)`：运算符/下标/`len` 的派发改写；
  - `sema_magic_deref_resolve(sema, slot, recv, name, mode)`：`.` 回退展开；
  - hook：`ex_member`（读）、`ex_call` 方法分支（调用）、`stmt_assign`
    （写）、`AST_BINARY`/`AST_UNARY`/`AST_INDEX`（运算符与下标）、`len`
    内建分支。
- **sema 判等展开**（`src/sema/magic_eq.c`）：`a == b`（lhs 是 struct/union，
  struct 无 `__eq__`）⇒ `__clux_eq_<typeid>(a.&, b.&)`：
  - 按类型惰性生成普通函数（缓存 `type_t* → fid`），函数体逐字段/逐元素/
    按 tag 写普通 `==`，walk 时递归展开（叶子 `__eq__` 是普通调用点）；
  - optional 字段先拷贝到局部（`const ?U` → `?U`）再 nil 分支 + `.!`；
  - 数组字段用 `for` + `(a.*)[i]`（只读下标支持 `const [N]T`）；
  - union 字段拷贝到局部后用 `is` + 成员读（`const U` 的 `is` 不允许，
    拷贝一次规避）；
  - 生成函数名 `__clux_eq_<typeid>`（static、按类型共享、惰性生成）。
- **cbackend/CTFE**：不再有判等特判——生成的判等函数是普通函数，两边都按
  普通调用处理（C 后端的 `clux_eq_*` 助手只剩位域 struct / cunion / tuple /
  slice 等内建叶子的比较）。
- **CTFE ensure-walked**：生成函数可能在 walk 队列处理它之前就被 CTFE 解释
  （comptime 折叠发生在生成它的那次 walk 里）——`ctfe_call_ast_fn*` 解释前
  确保目标函数已 walk（`sf->is_walked` 防二次 walk）。
- **配置**：`sema_t.magic_deref_max_depth` + CLI/env（§5.6）。

## 8. 测试计划

- 声明：未知魔法名、参数个数不符、`__eq__` 返回非 bool、非 struct 成员位置
  使用 `__` 名字、`union` 上声明魔法方法（P0 诊断）。
- 派发：内建优先（`i32 + i32` 不受影响）、接收者必须 `T`（`*T`/`?T`/`[N]T`
  不派发）、显式调用自动解引用、魔法方法体内表达式不设限制
  （`self == other` 是指针比较；`self.* == other.*` 会再次派发——递归由用户负责，
  CTFE 版本表现为深度超限诊断）。
- `__deref__`：`a -> b -> *c` 链、直接成员优先、字段遮蔽方法、`const`
  接收者、写穿成功/失败、环、超深、返回类型非法、单次求值、CTFE 版本。
- `.*`：`A -> *U`（得 `U`）、`A -> B -> *C`（得 `C`）、`a.*.*` 与
  `a.*` 等价、`a.* = v` 写穿、`*const U` 拒绝写、无 `__deref__` 时诊断、
  `.*` 与 `.` 混用（`a.*.field`、`a.__deref__().*`）、CTFE 版本。
- 下标：`__index__` 返回 `*U` 的读/写/复合赋值、返回 `*const U` 的只读
  拒绝、`a[i].field = v` 左值链、返回带 `__deref__` 的代理、`a` 与 `i`
  单次求值、`__index__` 返回非可解引用类型（如 `i32`）的诊断、
  `__len__` + `__index__` 的 `foreach`、`__index__` 内 `panic` 的越界路径、
  CTFE 版本。
- 比较：`__eq__` 覆盖逐字段、`!=` 取反、`rhs` 为不同类型（如 `str`）、
  `__lt__` 等四个各自独立且不互相推导、`const` 接收者、
  `__eq__` 返回非 bool 的诊断、CTFE 版本。
- 聚合判等（重点）：`?T`、`[N]T`、嵌套字段、union 载荷在 `T` 有 `__eq__`
  时**必须**调用 `__eq__`（运行期与 CTFE 各一组，二者结果一致）；
  无 `__eq__` 时保持逐字段/memcmp 现状；struct 不整块 memcmp（padding）。
- 接收者：魔法方法 `self: T` 被拒绝、`*const T` / `*T` 通过、`const` 调用规则。
- 与糖组合：`foreach` 走 `__len__` + `__index__`；`.?` 协议不受影响。

## 9. 已定稿的决策

| 项 | 结论 |
|---|---|
| 命名 | `__add__` 风格（parser 已预留 `__` 前缀文案） |
| 前导操作数 | 只派发 lhs；**不支持对称/反向派发**（无 `__radd__`） |
| 右操作数 | 二元魔法方法的 rhs **按指针传**（`*const R`，零拷贝；右值物化临时）；`__index__` 的索引值仍按值传 |
| 方法体内 | 不设"禁止二次派发"检查：`self == other` 是指针比较，要按值比较写 `self.* == other.*`，递归由用户负责 |
| 接收者 | 魔法方法必须指针接收者（`*T` / `*const T`），值接收者报错 |
| `__eq__` | 定义即覆盖；聚合内部（`?T`/`[N]T`/嵌套字段/union 载荷）同样调用 `__eq__`，编译期按类型决定"调方法 / 逐字段 / memcmp"（§4.2.1） |
| 比较序 | `__lt__` / `__le__` / `__gt__` / `__ge__` 进 P0，各自独立 |
| 下标 | `__index__` 返回可解引用对象；无 `__set_index__`；越界由用户 `panic`（§4.1） |
| `__deref__` | 只服务 `.` 与 `.*`；深度默认 8，可配（§5.6） |
| 配置 | CLI `--magic-deref-depth=N` + 环境变量 `CLUX_MAGIC_DEREF_DEPTH` |
| `union` | P0 不允许魔法方法（`union` 目前没有方法） |
| 嵌入继承 | P0 魔法查找只看自身成员 |
| `__dispose__` | `using a = e;` 作用于作用域退出（宽松约定，**不是析构**）；签名 `(self: *T) -> void`，与 `defer` 共用 LIFO 退出序列（§4.3） |
| `__assign__` | `a = b` 派发；**初始化不派发**；`op=` 先算再派发（§4.4） |

## 4.3 `using` 与 `__dispose__`

```text
using a = e;            ⇒   var a = e;
                            defer |pa = a.&| { pa.__dispose__(); }
```

- **宽松约定**：不是析构，不参与所有权/生命周期推导；编译器只保证在
  离开当前作用域时调用一次 `a.__dispose__()`（块尾 / `return` /
  `break` / `continue` 四条路径）。
- 展开与 `defer` 完全同构，因此退出顺序、提前返回的执行点、LIFO 复用既有
  实现：`using` 在作用域退出序列里登记一项，与 `defer` 按源码顺序交错取反。
- 实现细节：登记项是**变量定义节点**（marker），退出点直接插入
  `a.__dispose__()` 调用（变量在退出点仍在作用域内，不需要闭包捕获——
  C 后端的 defer 值捕获受限，这条路径绕开它）。
- 签名：`func __dispose__(self: *T): void`；返回非 void 报错。类型没有该
  方法时 `using` 报
  `type T has no '__dispose__' method: using requires a struct declaring ...`。

## 4.4 `__assign__`（赋值）

```text
a = b;            ⇒   a.__assign__(b.&)                 // 返回 void
a += b;           ⇒   a = a + b  ⇒  __add__ 派发后再 __assign__ 派发
```

- 派发条件：**赋值语句**的目标类型是结构体且定义了 `__assign__`；目标形态
  支持变量 / 字段 / 下标 / 解引用。
- **与初始化严格区分**：`var a = b;`、参数绑定、字段构造、返回值都走
  `value_assign`（复制 + 隐式转换），**不派发** `__assign__`。
- 签名：`(self: *T, rhs: *const R) -> void`（右操作数按指针传；self 必须是
  可写指针，`const` 目标在调用点按普通 const 规则拒绝）。
- 实现：`shadow_assign` 在正常赋值前做类型探测（IDENT/MEMBER/DEREF/INDEX），
  命中就把 `target.__assign__(&value)` 放进语句 prelude 并置 `sugar_noop`
  ——与 `.?'` 糖同一机制；复合赋值把 `op=` 改写成 `= (lhs op rhs)` 后重走。
- `a = b.?`（try 糖）：展开成 `if (b.ok()) { a = b.value(); } else { ... }`，
  then 分支的赋值同样派发 `__assign__`（`sema_try_make_if` 生成的赋值语句
  被替换成 `a.__assign__(&b.value())`）。
- 不派发的边角：`_ = expr`（丢弃）、`target = nil` 保持原语义。


## 10. 实现顺序（P0 进度）

1. ✅ parser 放行 struct 成员位置的 `__` 名字；
2. ✅ `sema/magic.c`：魔法表 + 名字/签名校验 + 前导操作数派发；
3. ✅ `__deref__`：`.` 回退（读/写/调用，`sema_magic_deref_rewrite`）与 `.*`
   展开（`sema_magic_deref_star_rewrite`）+ 环/深度（`CLUX_MAGIC_DEREF_DEPTH`）；
4. ✅ `__index__` / `__len__`（`a[i]` ⇒ `(a.__index__(i)).*`，读/写/复合赋值；
   `foreach` 可用）；
5. ✅ 运算符：算术/位运算/一元/比较（`== != < <= > >=`，`!=` 取反）；
6. ✅ 判等的类型级生效：sema 把 `a == b` 展开成 `__clux_eq_<T>(a.&, b.&)`
   （生成函数逐字段/for/`is` 展开，叶子 `__eq__` 是普通调用点——泛型
   `__eq__` 在聚合内部同样生效）；C 后端/CTFE 零特判；
7. ✅ 测试（`tests/magic_methods_test.cpp`，32 例：声明/派发/下标/比较/
   `__deref__`/`.*`/聚合判等（含泛型 `__eq__`/optional/数组）/comptime）。

## 4.5 `__init__`（构造）

```text
.T{arg1, arg2, ...}   ⇒   var t: T = <zero>;   // 全 0 字段
                          t.__init__(arg1, arg2, ...);
                          // 值 = t
```

- **有 `__init__` 的 struct 不再支持普通构造**：具名字段形式
  `.T{ .x = 1 }` 报错（`named-field construction is disabled`）；按序字段
  赋值也取消——`.T{a, b}` 的参数是 `__init__` 的实参，不是字段。
- 未赋值的字段保持**全 0**（`zero_init` 的零初始化临时对象），不是未定义。
- 签名：`func __init__(self: *T, ...任意参数...): void`——形参个数/类型由
  用户决定（魔法表 `any_arity` 跳过个数校验），返回必须 void。
- 实现：惰性生成
  `__clux_init_<id>(p1: A1, ..., pN: AN) -> T { var t: T = <zero>; t.__init__(p1..pN); return t; }`，
  构造点改写成对它的一次普通调用（参数求值顺序保持；临时对象在生成函数内部，
  C 侧不需要语句表达式）。
- **隐式单参构造**：`__init__` 恰好一个参数时，凡是"需要 T 的位置"都
  等价于 `.T{x}`：
  - `var a: T = x;`
  - 函数实参（`f(x)`，f 的参数类型是 T）
  - 返回值（`return x;`，返回类型 T）
  - 字段初始化（`.S{ .f = x }`，字段类型 T）
  条件：源类型不是 T，且可隐式赋给 `__init__` 的唯一参数类型。
  **赋值不算构造**：`a = x`（a 已是 T）仍要求 T 或 `__assign__`。