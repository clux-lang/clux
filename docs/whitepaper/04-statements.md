# 4. 语句

## 4.1 变量声明

```clux
var x: i32 = 1;
var y = 2;
var z = #none(i32);
```

规则见[声明](03-declarations.md)。局部变量属于当前块作用域。

## 4.2 赋值

```clux
x = 3;
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 2;
```

目标必须是可写左值。复合赋值展开为“读取、运算、写回”。

**魔法赋值**：目标类型是结构体且定义了 `__assign__` 时，简单赋值
`a = b;` 展开为 `a.__assign__(b.&)`（返回 `void`，两个操作数都按指针传）；
复合赋值 `a += b;` 先展开为 `a = a + b`（`__add__` 派发）再按上一条派发
`__assign__`。成员 / 下标 / 解引用目标同样适用。

**`a = b.?`（try 糖）**展开成 `if (b.ok()) { a = b.value(); } else { ... }`，
then 分支的赋值同样派发 `__assign__`。

**初始化不派发**：`var a = b;`、参数绑定、字段构造、返回值都是
`value_assign`（复制/隐式转换），不会调用 `__assign__`。

```clux
struct V {
    x: i32;
    func __assign__(self: *V, o: *const V): void { self.x = o.x; }
}
var a: V = .V{ .x = 1 };
var b: V = .V{ .x = 2 };   // 初始化：不派发
a = b;                      // 赋值：a.__assign__(b.&)
```

## 4.3 块

```clux
{
    var x = 1;
    x += 1;
}
```

块引入独立作用域；块内声明的变量在退出块时结束生命周期。

## 4.4 if

```clux
if (cond) {
    ...
} else if (other) {
    ...
} else {
    ...
}
```

条件必须用圆括号包围。

## 4.5 switch

```clux
switch (value) {
    (1, 2) -> {
        ...
    }
    (3) -> {
        ...
    }
    default -> {
        ...
    }
}
```

- 条件必须用圆括号。
- 分支写作 `(pattern, ...) -> { ... }`。
- `default` 最多一个。
- 分支不 fallthrough。

## 4.6 while

```clux
while (cond) {
    ...
}
```

## 4.7 do-while

```clux
do {
    ...
} while (cond);
```

循环体至少执行一次。

## 4.8 for

```clux
for (var i = 0; i < 10; i += 1) {
    ...
}
```

三段式 `for` 的初始化、条件、更新都以 `;` 分隔；初始化可以省略。

## 4.9 foreach

```clux
foreach (var item of items) {
    ...
}
```

- 迭代对象必须是数组、切片、元组或可索引序列，求值一次。
- `var` 与裸名是两种语义：`var` 形态每轮循环重新定义迭代变量（独立
  轮作用域）；裸名形态的前提是名字已在外层定义，每轮给既有绑定赋值。
- 迭代变量可以带显式类型标注。

```clux
foreach (var item: Item of items) { ... }
```

```clux
var sum: i32 = 0;
var item: i32 = 0;
foreach (item of <1, 2, 3>) { sum = sum + item; }  /* 每轮赋值既有 item */
```

## 4.10 break / continue

```clux
break;
continue;
```

只作用于当前最近一层循环。

## 4.11 return

```clux
return;
return value;
```

返回表达式必须能转换为函数的声明返回类型。

## 4.12 defer

```clux
defer {
    cleanup();
}
```

`defer` 块在离开当前作用域时执行，按后进先出顺序。

带捕获的 defer（捕获语法与函数捕获一致，见 §3.3——defer 本质上是函数）：

```clux
defer |h| {              % 裸名捕获：类型从定义点作用域现值推断
    close(h);
}
defer |(h = handle)| {   % 逐捕获括号：可带类型标注与默认值
    close(h);
}
```

两种形态可逐项混用：裸名 `|h|` 仅允许标识符；带类型标注或默认值的捕获
必须各自用括号包裹 `|(name: type = expr)|`——括号隔离捕获表达式内可能
出现的 `|`，避免与捕获列表闭合符冲突。空捕获列表写作 `||`。

## 4.13 表达式语句

```clux
foo();
x + 1;
```

表达式作为语句时以 `;` 结束。没有副作用的值表达式仍必须满足类型检查。

## 4.14 空语句

```clux
;
```

空语句不产生操作，只占一个语句位置。

## 4.15 comptime 语句

```clux
comptime if (LIMIT > 0) {
    ...
}

var seq: [3]i32 = .[3]i32{1, 2, 3};
comptime foreach (var x of seq) {
    ...
}
```

- `comptime if` 在编译期选择分支；未选分支不参与运行期代码。
- `comptime foreach` 在编译期展开；序列长度和元素必须是编译期已知值。
  序列必须是**已有的编译期值**（局部/全局变量、参数等）：clux 没有数组字面量，
  所以不能直接写 `comptime foreach (var x of [1, 2, 3])`，先绑定到变量再迭代。

## 4.16 编译期断言

```clux
static_assert(#sizeof(i64) == 8 as u64, "expected 64-bit i64");
```

- `static_assert` 是标准语句，可出现在任何语句位置。
- 条件在编译期求值（CTFE），必须是 `bool`：
  - `true` → 无操作，不生成运行期代码；
  - `false` → 编译失败，诊断带上 `msg` 的字符串字面量文本。
- 条件不是编译期常量、或不是 `bool` → 编译失败。
- 位于 `comptime if` 未选中分支里的 `static_assert` 不参与语义检查（随剪枝
  消失），可用于泛型/平台条件断言，例如禁止 `T == i32`：

```clux
func f[T](x: T): void {
    comptime if (T == i32) {
        static_assert(false, "f: T must not be i32");
    }
}
```

## 4.17 自动析构（RAII）

需要析构的绑定在离开作用域时**自动析构**，无需显式清理语句。析构语义
由类型定义，插入由编译器完成：插入点是 **sema**——自动析构是 AST 糖，
编译器把析构调用合成进归一化 AST，执行器/引擎自然执行，不引入运行期
隐式控制流。

### 4.17.1 "需要析构"谓词

类型 `T` **需要析构**（disposable），当且仅当：

- `T` 是 struct 且自身实现 `__dispose__`；**或**
- `T` 传递性地包含需要析构的成分（struct 的字段、`?T` 的载荷、
  tag union 的 variant、`[N]T` 的元素、元组的成员）。

不拥有资源的类型**不需要析构**：基础类型、指针、切片（`[]T`）、
函数值。谓词按**实例化后的类型**计算，泛型类型逐实例精确判定。

**cunion 被排除**：无 tag，运行期不可知活跃成员，cunion 不可析构，
且**禁止声明需要析构的字段**（编译错误）。

### 4.17.2 真实析构

类型的真实析构 = （若实现 `__dispose__`）先执行钩子 + 逆**声明序**递归
析构所有需要析构的字段。字段递归**不要求**外层 struct 显式实现
`__dispose__`——否则给 struct 加一个 disposable 字段会静默漏析构。

真实析构由编译器合成（合成名形如 `$dispose$T`，`$` 名字空间见 §1.3）。
**谓词驱动插入**：所有字段都不需要析构且自身无钩子时，不合成析构、
不插入调用——no-op 归零不是优化，是基线。

各复合类型的析构展开：

| 类型 | 析构展开 |
|---|---|
| struct | 钩子（若有）+ 逆声明序逐字段 |
| `?T` | `none` 无操作；`some` 析构载荷 |
| tag union | 按运行期活跃 tag 析构对应 variant |
| `[N]T` | 逆序逐元素（`N` 为编译期常量，展开为固定循环） |
| 元组 | 逆序逐成员 |
| cunion | 禁止（见 §4.17.1） |
| 指针 / 切片 / 基础类型 / 函数值 | 无操作（不拥有资源） |

### 4.17.3 插入时机与顺序

- **触发点**：块尾正常退出、`return`（表达式**先求值**再析构局部）、
  `break` / `continue`、`comptime if` 选中分支作用域退出。
- **顺序**：与 `defer` 共用同一条 LIFO 序——绑定按声明序"登记"，
  后登记的先析构；`defer` 块与自动析构按登记顺序交错执行。
- **手动析构**：`#dispose(x)`（§3.15.5）实参按**地址**接收——左值自动
  取址，指针直接作用于目标。因此它**析构的是目标值，不是绑定本身**：
  对堆对象 `#dispose(boxed)`（`boxed: *T`）释放目标后，`boxed` 这个
  指针值不需要析构，作用域出口无操作；对栈对象 `#dispose(v)` 提前析构
  后，`v` 的自动析构义务**不变**，作用域出口照常执行。语言**不做**
  重复释放的静态检查：重复安全性由类型自身状态保证——典型做法是钩子
  释放后把资源句柄置空（如 `?*T` 置 `none`），真实析构对空值天然无操作；
  需要"提前释放且出口不再触发"的对象应放在堆上经指针管理。
- **全局/模块级 `var` 禁止需要析构的类型**：程序存活期没有对应的作用域
  退出点；需要析构的值只能存活于函数内。

### 4.17.4 拷贝、移动与所有权通道

自动析构不引入所有权分析；拷贝/移动完全经由 §3.15 的既有钩子：

- 同类型初始化（`var b: T = a;`、实参、返回值、字段）走 `__init_copy__`
  （固定签名 `(self: *T, src: *T) -> void`，源按地址传入）：实现它就是
  拷贝语义；构造里置空源对象就是移动。没有它时同类型初始化按
  "不可 copy"拒绝——double-free 被静态关闭。
- `return` 与实参传递都经 `__init_copy__` 派发，**NRVO 无需特判**：
  返回值直接构造进调用方，源不经受拷贝，自然也只有一个所有权方。
- `__assign__` 只管赋值语句，不参与析构（旧值怎么释放由实现者负责，
  通常是实现里的 `#dispose(self)` 或字段逐一覆盖）。

### 4.17.5 临时值：提升（lift）

需要析构的临时值**无需显式绑定具名 `var`**——sema 在归一化 AST 里自动
把临时捕获提升为隐藏绑定（`$tmp$N`，`$` 名字空间见 §1.3），在其生命
终点插入真实析构。提升完全发生在 AST 层，执行器/引擎零感知。

**临时根判定**：调用或构造表达式（`make_thing()`、`.Res{v = 1}`）且
类型结果命中 disposable 谓词（§4.17.1）。**捕获协议**（语句槽边界）：
临时根提升为 `var $tmp$N: T = <表达式>;` 插到语句前，语句后逆序
（LIFO，与 §4.17.3 同序）插入 `$dispose$T($tmp$N.&)`：

```clux
func work(): void {
    make_thing();                  // var $tmp$0: T = make_thing();
                                   // ... $dispose$T($tmp$0.&);
    var n = make_thing().count;    // 临时提升，n 从 $tmp 拷贝后即时析构
    var t = make_thing();          // 直通：t 直接 bind，作用域出口析构
}
```

- **槽直通**：`var` / `return` 的值槽**顶层单一**临时根不提升——`var`
  直接绑定（避免 `__init_copy__` 二次拷贝），`return` 所有权随值转移；
  `foreach` 的 iterable 由脱糖（§2.7）直连 `__iter`。嵌套子树照常捕获。
- **`return` 物化**：捕获非空时 return 表达式引用 `$tmp`，直接
  [析构][return] 会 use-after-dispose——先物化 `var $ret$N: T = <return
  表达式>;`（经 `__init_copy__` 置空 `$tmp` 源），析构 `$tmp` 后
  `return $ret$N`。`$ret` 不析构（所有权流出）。`T` 取 return 值根的
  类型锚点（CALL / CONSTRUCT / 元组根）；锚点缺失的残余形态沿用裸
  形态并跳过析构（保守泄漏，保语义正确）。
- **惰性上下文不捕获**：`&&` / `||` / `?:` 的子表达式条件求值，提升即
  提前求值、破坏短路——整个惰性父节点解除捕获，其内临时值按引擎既有
  匿名值通道存活（v1 已知限制，不做自动析构）。
- **函数定义子树不捕获**：形参/捕获默认值在**调用点**求值，不在定义处。

**循环降维（零 flag 方案）**：cond/update（for 另含 init）含临时根的
`while` / `do-while` / `for` 统一降为 `for (;;)` + 段内 gate 块——临时在
gate 块内每轮声明、每轮析构（块每执行 push 新作用域），临时生命周期
封闭在轮次内，cond 求值次数与原语义一致：

- `while (C) B` → `for (;;) { gate(C); B' }`
- `do B while (C)` → `for (;;) { B'; gate(C); }`——原语义 `continue` 会
  再求值一次 cond，体内直接层 `continue` 语句前同构前插一份独立 gate
  （cond 假→析构→break 正确终止，真→落到 `continue` 回块顶正确继续）；
  `break` 无需前插（跳出循环本就不再求值 cond）。
- `for (I; C; U) B`：I 的临时物化到循环**外**（var 形态直通绑定）；
  C 有根时 gate 入 body 首（COND 槽空 = 恒真，§4.8）；U 有根时 gate
  块入 UPDATE 槽（`continue` 先走 update 再走 cond，对齐原语义）。

gate 块结构：`{ [vars]; if (C') {} else { [析构] break; } [析构]; }`。
早期构想的 `$first` flag 方案（隐藏运行期变量标记首轮）被否决：flag
把"首轮差异"编码为**运行期状态**，每次条件求值都要读写它；零 flag
方案把同样的差异编码为 **gate 块的作用域结构**——降维纯 AST 层，无
运行期代价，也无可忘置的标志位。

**foreach 元素逐轮析构**：var 形态且元素类型需要析构时，循环体每轮
末尾追加元素析构（元素是 copy，析构的是拷贝而非容器成分）；体内
直接层（不穿嵌套循环体）的 `continue` / `break` 语句前同构前插——
break/continue signal 在循环边界被消费、跳过块尾语句，纯体尾追加
对该两形态轮次漏析构。裸名形态（每轮给外层既有绑定赋值）元素
所有权不归循环，不做逐轮析构。

**已知限制（v1）**：深 `return` 穿过循环时不触发 iterable 的出口析构。

### 4.17.6 FFI 边界

C ABI 边界（`cfunc` / `extern` 的形参与返回类型）**拒绝需要析构的类型**：
C 侧无法执行 clux 的析构协议，出现即编译错误。跨边界只能传不拥有
资源的类型（基础类型、指针、切片视图）。

```clux
struct Guard { ... func __dispose__(self: *Guard): void { ... } }
extern func register_c(g: Guard): void;     // 编译错误：Guard 需要析构
extern func register_c(p: *Guard): void;    // 合法：指针不拥有资源
```
