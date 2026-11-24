# sema 整体设计（纯 AST 产物）

> 状态：定稿（2026-11-24）。本文是 dev 分支 sema 的权威设计。
> 前提约束来自旧版（master）的失败教训：sema 产出"混合 AST + sema 结构"，
> compiler 依赖 sema 内存对象（类型池借用指针、sema 分配的 fid、持久化作用域树、
> 符号表反向引用 AST），实践证明是灾难。本设计彻底切断该依赖。

## 1. 不变量

sema 的产物是**上下文无关的纯 AST**，且是**当前 clux 语言的子集**：

- **I1 语言子集**：sema 输出只使用现有 node kind（CLUX_NODE_*），不引入任何
  新节点类型。输出 AST 上每个节点的语义与一段合法 clux 源码一一对应
  （语义归一形：泛型已实例化、comptime 已折叠/剪枝、defer 已脱糖、
  名字已唯一化）。
- **I2 零外部依赖**：compiler 只 include `parser/`、`core/`、`diag/` 头。
  sema 的全部内存结构（作用域树、符号表、engine 对象）在 sema 结束时销毁，
  交接物只有 AST。
- **I3 可验证纯度**：sema 输出经 `*_node_to_json` 可完整序列化，JSON 中
  不出现指针地址；回归测试在 sema 结束后销毁全部 sema/engine 状态再跑
  compiler，现存活即为纯度证明。

名字即符号表，PROGRAM 声明表即上下文——compiler 是一个普通的
clux 编译器，不需要任何 sema 提示。

**整体构型趋近 C**：sema 输出的程序模型是——顶层为唯一的函数 /
类型 / 全局变量声明集，struct 仅剩字段，方法与属性是全局函数与
全局变量，类型全部显式，名字全部唯一，控制流只剩基础语句
（if/while/for/switch/return/break/continue）。嵌套声明、推断、
糖、编译期构造全部在 sema 消化完毕。这意味着 compiler 面对的
输入与 C 编译器同构：按名取声明、按显式类型布局、按基础语句
发码，不需要任何作用域/查找/推断智能。

## 2. 变换清单

sema 按序对模块做以下变换。每项变换的产物都在语言子集内。
下文的"替换 / 改写 / 摘除"均为**产物 AST 中的形态描述**——按 §4 的
三段式流程，sema 的产出是逆向编排重建的**全新 AST**，从不修改
原 AST 节点。

### 2.1 编译期计算 → 常量回填表达式

comptime var 求值、comptime func 调用、`#` 编译期内建求值的结果，
渲染回**字面量 / 构造表达式 AST** 落在引用点：

- comptime var 定义语句从语句流摘除；所有引用点替换为折叠值的字面量
  或构造子树。
- 求值内核直接复用 exec 层已验证的 CTFE 能力（engine 求值），
  engine 值 → AST 的转换器是本项的新增边界件。

### 2.2 局部类型/函数提升到全局

函数体内声明的类型与函数提升到 PROGRAM 声明表尾部，名字加 enclosing
编码保证模块内唯一（见 §3.3）。原位置的声明语句摘除；函数体内的引用点
改为提升后的名字。

### 2.3 泛型对象实例化（各模块各自实例化）

泛型**定义不进入输出**。每个实际使用的参数组合产出一个具名实例声明，
追加到本模块 PROGRAM 尾部；使用点全部替换为实例名：

```
var a: Test[i32, i32] = .{};   →   var a: Test$i32$i32 = .{};
```

- 实例体是定义 AST 的深拷贝 + 参数代换（类型参数 → 具体类型表达式）。
- 实例化发生在**使用它的模块**内；实例名在本模块 program 内唯一即可
  （不同模块各自实例化同名实例互不冲突，分属各自 program）。
- 使用导入模块导出的实例时直接引用其名字，不重复实例化
  （沿用 GET_MEMBER 通道语义）。
- 代换逻辑与 exec 层 `exec_generic_instantiate` 同构，只是产物
  从 engine 对象换成 AST。

### 2.4 defer 脱糖

STMT_DEFER 从树中收集，在作用域退出点（块尾 / return / break / continue）
按 LIFO 序内联为其体（或调用）。带捕获的 defer 按值语义定稿
（捕获一律 copy）脱糖为**立即捕获的闭包**：

```
defer |(h = handle)| { close(h); }
→  var __defer0 = |h| { close(h); }(handle);   /* 捕获即拷贝 */
```

退出点插入 `__defer0();`。

### 2.5 comptime if 剪枝

条件在 sema 内 CTFE 求值：真臂保留、假臂丢弃，节点替换为所选臂的块。
else-if 链递归处理。条件不可求值是硬错误。

### 2.6 comptime foreach 展开

编译期完全展开：容器是编译期已知的元素序列时，循环体按元素逐份拷贝，
迭代变量替换为对应元素子树。不可展开是硬错误。

### 2.7 foreach 脱糖（普通形态）

普通 foreach 脱糖为索引 for 循环，让 compiler 不必实现 foreach：

```
foreach (var item of items) { ... }
→  {
      var __i = 0;
      for (; __i < #len(items); __i = __i + 1) {
          var item = items[__i];
          ...
      }
  }
```

- 迭代对象限定数组/切片/元组/可索引序列（白皮书 §4.9），
  `#len` 与下标是现有语言能力，零新构造。
- `var` 省略时迭代变量按只读语义处理（只读性由使用点检查承担）。

### 2.8 推断显式化归一

类型推断的结果回填为显式标注，消除输出中的一切推断：

```
var a = 1;            →   var a: i32 = 1i32;
```

- 推断出的类型写入 TYPE 槽；无后缀字面量按推断类型补类型后缀
  （后缀是现有词法，见白皮书 §1；无上下文时整数默认 i32、浮点默认 f64）。
- 依赖类型检查完成；本项与常量回填共用"engine 类型/值 → AST 渲染器"。

### 2.9 匿名构造具名化

匿名构造 `.{ ... }` 合成为具名 struct 追加到 PROGRAM，构造点改写：

```
var x = .{ .a = 1, .b = 2 };
→   struct nonamed$1 { a: i32; b: i32; }
     var x: nonamed$1 = .nonamed$1{ .a = 1, .b = 2 };
```

- 合成 struct 的字段类型由元素类型推断（依赖 §2.8 的渲染器）。
- 同构的匿名构造是否共享同一合成名：实现期定稿（倾向共享，与实例
  去重语义对齐）。

### 2.10 struct 成员退化为全局（member_call 退化为函数调用）

struct 体内的属性（静态变量）与方法全部提升为全局，struct 仅剩字段；

```
struct AAA { func print(self: *AAA): void { ... } }
var a = .AAA{};
a.print();
→   struct AAA {}
     func AAA$print(self: *AAA): void { ... }
     var a: AAA = .AAA{};
     AAA$print(a.&);
```

- 方法 → 全局函数 `Type$name`；静态变量 → 全局变量 `Type$name`。
- `a.print()` 即 member_call 退化：receiver 按方法 self 声明形态适配
  ——`*AAA` 传 `a.&`（取地址是现有后缀语法），值形参传 `a`，
  const 借用形参按借用约定传。字段访问（`a.f`）不退化，
  仍是基础 member/index 运算。
- 前提：实例化与类型解析完成后，一切方法调用都静态可解析；
  接口形态的动态分派不在本变换展开（随接口实现落地另行定稿）。

### 2.11 不回填的信息

诊断类信息（类型检查细节、确定性赋值分析、约束检查记录、装饰器
元信息）不回填——它们是 sema 的消费品。装饰器中属于后端提示类的
（如 [[inline]]）随声明保留，其余在 sema 消费后摘除。

## 3. 名字方案（mangling）

mangled 名只存在于 AST 的 SYMBOL 文本中。目标是确定性、无歧义、可读，
并尽量保持可重新 parse（`$` 不与现有标识符字符集冲突，按 lexer 运行期
扩展处理；若第一版 lexer 不放行 `$`，验证模式降级为 to_json 往返）。

### 3.1 泛型实例名

```
Name$arg1$arg2...
```

- `Test[i32, i32]` → `Test$i32$i32`
- 参数段是类型的结构化渲染：内建/用户类型用名字；复合构造用受控
  编码（`*T` → `$pT`、`<T,U>` → `$tT$U`…，转义表实现期定稿），
  保证不同结构必产生不同实例名。
- 函数实例同理：`g$i32$str`。闭包捕获形态不入 mangling——实例体的
  类型正确性由实例体自身保证。

### 3.2 实例去重

同一参数组合在同一模块内只实例化一次（按 mangled 名查重），
与 exec 层结构判等去重语义对齐。

### 3.3 提升名与合成名

统一用 `$` 分隔（`_` 与用户标识符空间冲突，`$` 专属编译器合成，
与泛型实例名一致）：

- **局部提升**：函数 `f` 内的局部 `struct Foo` → `f$Foo`；嵌套递归
  拼接（`f$g$Foo`），enclosing 链取最近的函数边界。
- **struct 成员提升**：方法/静态变量 → `Type$name`（如 `AAA$print`）。
- **匿名构造合成**：`nonamed$N`（模块内计数器）。

所有合成名登记进符号表参与查重，与用户名冲突即编码加缀。

## 4. sema 内部架构（三段式：walk → 资源表 → 逆向编排）

```
parser 产出原始 AST（sema 只读，从不修改）
        │ ① walk：求值消费方向
        ▼
sema 资源表（engine 管理：type / function / value）
  + 作用域树 + 符号表（sema 私有，用完即毁）
        │ ② 逆向编排：渲染方向（唯一新建节点的通道）
        ▼
语义归一形新 AST（唯一交接物；未变换子树整段迁移）
        ▼
compiler（读 AST 出代码，零 sema/engine 依赖）
```

三段各自的角色：

- **① walk（AST → 资源表）**：只读遍历原 AST，不断"创建-消费"——
  解析名字、求值类型、实例化泛型、CTFE 折叠 comptime，产物登记为
  engine 管理的 function / type / value，连同提升名 / 实例名 /
  合成名一起进资源表。求值内核直接借用 engine（exec 层 CTFE 复用）。
  原始 AST 上的任何节点在这一段都不可变。
- **② 逆向编排（资源表 → 新 AST）**：从资源表出发重建全新 AST。
  **新建节点只有两条通道**——渲染器（engine 值/类型 → AST：常量
  回填、类型显式化、实例体、合成 struct、提升函数）与克隆器
  （原子树 deep clone：未变换的基础语句、未涉及推断的表达式）。
  原树完整保留只读，不做任何剪接迁移或原地改写。
- **作用域树与符号表是 walk 的工作内存**：查遮蔽、登记提升与
  实例化。sema 结束即销毁，不持久化。

设计动机：in-place 修改原 AST 会在槽位管理、双父挂接、被摘除节点
残留上反复出坑（AST 节点 children 手工维护，无自动防线）；重建路线
让"读原树 → 产新树"单向流动，所有权链干净：原 AST 归 parser，
资源表归 sema（随 sema 销毁），新 AST 是唯一交接物。原树只读换来
一条硬验证：sema 前后原 AST 的 to_json 完全一致。

## 5. 验证标准

1. **include 扫描**：compiler 源文件只允许 `parser/`、`core/`、`diag/` 头
   （脚本断言，进 CI）。
2. **纯度回归**：sema 输出后销毁全部 sema/engine 状态再跑 compiler——
   现存活写进测试（借 ASAN 抓 use-after-free）。
3. **序列化往返**：sema 输出 to_json 不含地址；中期目标：JSON 可重建
   等价 AST（第一版允许豁免，作为验收方向保留）。
4. **语言子集断言**：sema 输出 AST 中不存在 STMT_DEFER /
   STMT_COMPTIME_IF / STMT_COMPTIME_FOREACH / 泛型声明节点
   （generic params 为空或节点为具体形）；STMT_VAR 与 CONSTRUCT 的
   TYPE 槽非 NULL（无推断、无匿名构造）；call 的 callee 不再是
   member 访问形态（member_call 已全部退化）。逐变换写正测。
5. **原 AST 不可变**：sema 前后原 AST 的 to_json 完全一致
   （walk 只读 + 新建节点走渲染器/克隆器两条通道的回归证明）。

## 6. 分期落地

| Phase | 内容 | 验收 |
| --- | --- | --- |
| 1 | 骨架：符号表/作用域树 + 名字解析 + mangling 基础 | 解析错误诊断 |
| 2 | 提升（§2.2）+ 实例化名回填最小版（§2.3 只类型） | 子集断言 |
| 3 | 常量回填（§2.1）+ 推断显式化（§2.8，值/类型→AST 渲染器） | comptime 折叠正测 |
| 4 | defer 脱糖 + comptime if 剪枝 + 两类 foreach（§2.4–2.7） | 子集断言 |
| 5 | 泛型实例化完整版（§2.3）+ 匿名构造具名化（§2.9）+ struct 成员/method_call 退化（§2.10） | 与 exec 实例化行为对齐 |
| 6 | 纯度回归 + include 扫描 + 序列化往返（§5） | CI 常驻 |
