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
原 AST 节点。原 AST 只在 sema 过程中只读保留，最终随 engine 一同
销毁（见 §4 所有权与生命周期）。

### 2.1 编译期计算 → 常量回填表达式

comptime var 求值、comptime func 调用、`#` 编译期内建求值的结果，
渲染回**字面量 / 构造表达式 AST** 落在引用点：

- comptime var 定义语句从语句流摘除；所有引用点替换为折叠值的字面量
  或构造子树。
- 求值走 sema 内嵌的 CTFE 内核（即 exec，见 §4 的关系定稿）：
  在 current 上下文继承 sema 现状做真实计算。
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
- **walk 期物化（P11）**：实例化与 emit 解耦——sema_walk 尾部
  （anons 收集后、检查 pass 前）按收集序对每个参数组合做 inst 态克隆
  物化（GENERIC_PARAMS 摘除 + 形参代换 + 自引用折叠），产出非泛型形态
  声明挂 `sema_instance_body_t.inst_stmt`。新造文本（实例名等）intern
  进 sema 持有的临时 `inst_program` 文本池。
- **检查 pass 覆盖实例体**：检查域 = 原树顶层语句 + 全部物化体，
  四类顶层循环（模块注册 / 非泛型函数体 / struct 方法体 / 模块 var
  不动点）统一遍历同一 tops 表。定义体错误在错误实参组合上暴露。
- **emit 只做普通消费**：emit 视物化体为普通 func/type，尾部纯普通
  深克隆进产物（不再关心实例化）；克隆期按指针命中文本池的 location
  文本重 intern 进产物 program（reown_from 通道），产物新造文本自持
  不借 sema 生命周期（§5-2 引擎全灭后仍可序列化）。defer_counter
  在物化循环与 emit 间连续（物化消耗记入 `inst_defer_counter`，
  emit 预置起点），防 `__deferN` 撞名。物化失败条目（防御态 NULL）
  检查与 emit 均跳过。
- 实例化发生在**使用它的模块**内；实例名在本模块 program 内唯一即可
  （不同模块各自实例化同名实例互不冲突，分属各自 program）。
- 使用导入模块导出的实例时直接引用其名字，不重复实例化
  （沿用 GET_MEMBER 通道语义）。
- 实例化与 CTFE 是**同一机制而非同构**（见 §4）：实例体的类型检查
  就是在代换后的 AST 上带实参 shadow 值再做一次 walk，只是最终
  产物从 engine 对象换成 AST。

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
        │ ① walk：求值消费方向（全程 shadow 值）
          ┌──────────── engine/scope/value 体系 ────────────┐
          │  资源表（type / function / value：engine 管理）  │
          │  作用域树 + 符号表：直接消费 engine/scope，不自建 │
          │  exec = sema 内嵌的 CTFE 求值内核（继承 current  │
          │  上下文临时做真实计算）                           │
          └──────────────────────────────────────────────────┘
        │ ② 逆向编排：渲染方向（唯一新建节点的通道）
        ▼
语义归一形 program_t 集合（唯一交接物；未变换子树整段克隆）
        ▼
compiler（读 program_t 出代码，零 sema/engine 依赖）
```

**engine/scope 与 sema/exec 的关系**（2026-11-25 定稿）：engine/scope
体系的本源消费者是 sema，不是 sema 复用 exec。sema 全程使用 shadow
变量在 engine/scope 上建树、查名、做类型检查；exec 不是独立的运行时层，
而是 sema 内嵌的 CTFE 求值内核——局部的编译期计算（常量折叠、comptime
条件求值）在 current 上下文**继承 sema 现状**临时做真实计算：

- CTFE 输入即 sema 现状：const 符号带真实值，var/参数是 shadow 值；
- CTFE 途中碰到 shadow 值（常量表达式引用了运行期量）即为 diag；
- 帧协议 global→module→closure→argument→body 正是"继承现状"的
  机制化：walk 建好的作用域树就是帧链的前几层，CTFE 进函数体时
  临时帧直接挂接。

三段各自的角色：

- **① walk（AST → 资源表）**：只读遍历原 AST，不断"创建-消费"——
  解析名字、求值类型、实例化泛型、CTFE 折叠 comptime，产物登记为
  engine 管理的 function / type / value，连同提升名 / 实例名 /
  合成名一起进资源表。错误逐层抛出到 stmt 级，经 context 的 diag
  收集器入列，从下一个 stmt 继续（不 fast-fail）。
  原始 AST 上的任何节点在这一段都不可变。
- **② 逆向编排（资源表 → 新 AST）**：从资源表出发重建全新 AST。
  **新建节点只有两条通道**——渲染器（engine 值/类型 → AST：常量
  回填、类型显式化、实例体、合成 struct、提升函数）与克隆器
  （原子树 deep clone：未变换的基础语句、未涉及推断的表达式）。
  emit 对 sema 状态**只读**：纯渲染、无副作用、不回写资源表。
  原树完整保留只读，不做任何剪接迁移或原地改写。
- **作用域树与符号表是 walk 的工作内存**：直接消费 engine/scope
  体系查遮蔽、登记提升与实例化（不自建平行符号表）。sema 结束
  即销毁，不持久化。

**所有权与生命周期**：原 AST 归 parser（挂在 engine 的 allocator
上），资源表与作用域树归 engine，两者随 engine 销毁一同消亡；
逆向编排的产物分配在独立的 out_allocator 上——它是新 AST 的唯一
分配来源，保证产物不夹带任何 engine 指针（I2/I3 由 allocator 分离
直接物化，而非代码约定）。sema 是两者之间唯一的单向渡口。
原树只读换来一条硬验证：sema 前后原 AST 的 to_json 完全一致。

设计动机：in-place 修改原 AST 会在槽位管理、双父挂接、被摘除节点
残留上反复出坑（AST 节点 children 手工维护，无自动防线）；重建路线
让"读原树 → 产新树"单向流动，所有权链干净：原 AST 归 parser，
资源表归 engine（随 engine 销毁），program_t 集合是唯一交接物。

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
| 7–10 | 语义合法性检查：名字解析 → 表达式类型推断 → 语句级检查 → DFA + 推断补全（§8） | 错误程序一次产出全部诊断 |

## 7. API 定稿（2026-11-25）

### 7.1 接口

```c
clux_sema_t *sema_create(clux_context_t *ctx, allocator_t *out_allocator);
int sema_walk(clux_sema_t *sema, node_t *program);
bool sema_has_error(const clux_sema_t *sema);
vec_t *sema_emit(clux_sema_t *sema);   /* 元素 program_t*，归调用方所有 */
void sema_destroy(clux_sema_t **sema);
```

- `sema_walk`：阶段一。AST → engine/module 体系 + 资源表；shadow
  值建树、逐 stmt 收 diag、按需内嵌 CTFE。可独立调用、独立验证。
- `sema_has_error`：委托 context 的 diag 收集器（ERROR 级判定）。
- `sema_emit`：阶段二。资源表 → program_t 集合。对 sema 状态只读、
  无副作用，因而幂等。返回按 import 拓扑序排列的向量（被依赖者在
  前，入口 module 在末尾）。

**错误即终止协议**：walk 结束发现有 ERROR 级 diag → 不进 emit，
直接渲染打印 diag、流程结束。错误程序唯一出口是诊断，不产半成品。

### 7.2 program_t（交接物）

```c
/* include/core/program.h —— 中性交接物，零 engine 依赖 */
typedef struct _program_t program_t;  /* opaque + getter */

program_t *create_program(allocator_t *allocator);
void delete_program(program_t **program);
```

- **粒度 per-module**：一个 module 一个 program_t；sema_emit 返回
  它们的有序集合。模块间依赖信息即拓扑序本身，导入名单表暂不预置，
  compiler 需要显式表时再加字段。
- **最小字段集**（实现中发现缺失的所有权依赖/元信息再补，不预置）：
  - `decls`：本 module 的 PROGRAM 顶层声明集（node_t*）
  - `module_name` / `source_path`
  - `strings`：常量字符串表（第一版做 intern 去重；后端数据段
    布局用途后续定）
- **放置位置**：core/。依赖 parser（node_t）与 core（string/vec/
  allocator），不依赖任何 engine 头——compiler 消费 program_t 与
  消费 clux_error_t 同一性质：纯数据、无引擎耦合，I2 的 include
  白名单不需开口子。

### 7.3 生命周期链

```
sema_create(ctx, out_allocator)
    → sema_walk(program)          /* 旧 AST 只读遍历 */
    → has_error ? 渲染打印 diag 并结束
                : sema_emit()     /* 产出 program_t 集合（out_allocator）*/
    → sema_destroy(&sema)
    → delete_clux_context(&ctx)   /* 旧 AST / 资源表 / 作用域树消亡 */
    → compiler 消费 program_t 集合（out_allocator 独立存活）
```

## 8. 语义合法性检查（Phase 7+）

> 状态：设计稿（2026-11-26）。本节是 §4 "walk 做类型检查"锚点的落地设计。
> 范围定稿：名字解析、类型检查全部项、DFA（确定性赋值）、泛型约束检查
> （§8.9，P12）。所有权/借用检查不进本期，随后续阶段另行定稿。

### 8.1 定位与原则

检查的输入是原 AST + 既有收集表（提升/实例/常量/匿名/方法），输出有二：

- **诊断**（diag，§2.11 定稿不回填，是 sema 的消费品）；
- **类型结果表**（`节点 → clux_type_t*`，§2.8 推断显式化补全的数据源，
  是唯一需要流到 emit 侧的检查产物）。

**核心原则：类型检查 = shadow 求值消费 engine 语义原语。** sema 不重写
类型规则——engine 的 `binary` / `invoke` / `index` / `field` / `assign` /
`safe_cast` 在 shadow 操作数下已按类型规则演算（shadow 传播：任一操作数
为 shadow 即产出结果类型的 shadow，错误原样传播），`invoke` 的 shadow
拦截天然实现"调用点只交验签名"，seal 门禁天然拒绝 void / interface
出现在值类型位置。sema 在本阶段新增的职责只有三块：

1. **遍历协议**：结构访问而非执行（循环体不迭代、调用不展开）；
2. **error → diag 转换**：engine 原语产出的 error 值转为 diag 入列，
   本 stmt 放弃、从下一个 stmt 继续（§4 不 fast-fail 协议）；
3. **engine 没有的静态检查项**：条件须 bool、return 匹配、构造字段
   完备性、DFA。

### 8.2 类型环境

- **内建类型**：engine 已按名注册进 global scope（`context_lookup`
  通道），零新增。
- **用户类型**：检查 pass 之前，模块级类型声明（struct/enum/union/
  bitfield/interface）按声明序从原 AST 构建引擎类型对象
  （`clux_context_create_struct_type` 等），对象归 context 具名类型表
  托管。自引用/互引用 struct 走两阶段构造（begin/install：字段壳先行
  布局，方法签名引用自身类型时壳已按名可见）。
- **名字解析通道**：函数体自由名字只解析到形参 / 显式捕获 / 模块级
  名字 / 全局内建（帧协议既有约定），遮蔽语义由 scope 树天然承载。
- **与 mangling 解耦**：检查在**原树原名**上做（作用域语义：遮蔽/
  嵌套）。mangled 名只是产物形态（§3），检查 pass 不感知——产物里的
  `f$Foo` 在检查期就是 `f` 作用域里的 `Foo`。

### 8.3 遍历协议（帧协议的静态版）

检查 pass 挂在 `sema_walk` 既有收集全部完成之后（提升名 / 实例名 /
常量表 / 剪枝决策已就绪，检查才能查到完整语义）。遍历序：

- **模块级**：逐声明构建类型对象与 func 签名（func 类型对象），函数
  体延后统一检查（前向引用天然成立，与 I1 的"顶层唯一声明集"同构）。
- **函数体**：沿 global→module→函数体作用域链 push 块作用域，形参
  登记 shadow 值，逐 stmt 遍历；嵌套块 / 闭包体递归进入，作用域随块
  push/pop。
- **检查对象是原树**，糖构造按其脱糖语义检查（§2.4–2.7 的变换语义
  即检查语义）：foreach 按迭代变量 + 体内使用检查；defer 体内名字
  解析到 defer 所在作用域（脱糖正是这个语义）；comptime 构造已被
  CTFE / 剪枝处理的部分不再检查（不进产物的代码不检查）。
- **调用点只交验签名**（既有约定）：不进被调函数体。

### 8.4 检查项清单

| 检查项 | 机制 | 分期 |
| --- | --- | --- |
| undefined name（变量/函数/类型） | scope 链 lookup 未命中 → diag | P7 |
| unknown type（类型表达式求值失败） | 类型名 lookup / 复合类型构建失败 → diag | P7 |
| 内建类型名遮蔽 | 模块/局部声明撞内建名 → diag | P7 |
| void / interface 出现在值类型位置 | engine seal 门禁（既有） | P7 |
| 表达式类型推断 | shadow 演算（engine 原语 shadow 传播） | P8 |
| 调用签名（实参个数/类型） | `invoke` 签名校验（既有，shadow 态） | P8 |
| var 初始化 / 赋值类型兼容 | `safe_cast` 结果判定 → diag | P9 |
| 构造字段完备性 + 字段类型匹配 | 按目标类型字段表比对 | P9 |
| if / while / 三元条件须 bool | 条件演算结果 kind 判定 | P9 |
| return 类型匹配 | 与签名返回类型 `type_equal` | P9 |
| use before assignment（DFA） | §8.6 状态演算 | P10 |

### 8.5 类型结果表（→ §2.8 渲染器）

- sema 内 per-node 表（ctx allocator，随 sema 销毁）：**第一版只记
  STMT_VAR / 构造字段 / foreach 迭代变量 → `clux_type_t*`**（借用，
  类型对象归 context）。其余表达式的类型只是演算中间态，不入表。
- emit 侧消费：克隆器的 STMT_VAR 后处理（`sema_clone_var_fixup`）
  在 TYPE 槽为 NULL 时查表回填渲染类型名——正是 Phase 6 遗留的
  `r / z / q / __defer0` 缺口的解法。
- 渲染用**mangled 名**（实例名 / 提升名 / 合成名，与 §3 一致）：
  检查期消费原名，产物期渲染产物名，两侧职责分离。
- §5-4 语言子集断言中 STMT_VAR TYPE 槽断言随之**收紧为无条件**
  （test_purity.cpp 注释已预告该收紧）。

### 8.6 DFA（确定性赋值）静态化

每个局部 var 一个赋值状态（未赋 / 已赋），随 8.3 遍历演算：

- **顺序**：语句序传递；赋值语句目标置已赋。
- **if**：两臂分别演算后按**并集**合流（两臂都赋才算赋）；无 else
  时假路径直通。
- **while / for / foreach**：保守策略——体内入口取循环前状态，体内
  赋值**不提升**到出口（循环可能零次执行），出口保持循环前状态。
- **短路 `&&` / `||`**：右侧按保守并集（右侧可能不求值）。
- **消费点**（读取局部变量）状态为未赋 → diag "use before assignment"。
- **defer / 闭包捕获**：捕获即拷贝，捕获点按消费点检查。
- TDZ 是同一语义的**运行期**同型物；静态版在检查 pass 内独立实现，
  机制分离（一个静态一个动态），语义对齐由测试对照保证。

### 8.7 泛型体检查（P11 已落地）

实例化与 emit 解耦（§2.3 P11）：sema_walk 尾部把每个使用过的参数组合
物化为非泛型形态声明（inst 态克隆 + 代换），检查 pass 把物化体当
**普通声明**检查——模块注册 / 函数体 / struct 方法体 / 模块 var 各循
环统一遍历「原树顶层 + 全部物化体」。泛型定义体本身不再单独建检查
通道：未使用的定义体不物化、不检查（`GenericDefWithoutUseIsNot
Instantiated`）；定义体错误天然只在物化出的坏实参组合上暴露
（`GenericBodyErrorOnlyInBadInstance`）。检查失败经 diag 汇聚到
sema_has_error，emit 返回 NULL。

### 8.8 分期表

| Phase | 内容 | 验收 |
| --- | --- | --- |
| 7 | 类型环境 + 名字解析：用户类型构建注册 + 函数体作用域树遍历 + undefined name / unknown type / 内建遮蔽 / seal 门禁 | 错误程序一次产出全部诊断（不 fast-fail） |
| 8 | 表达式类型推断：shadow 演算全表达式形态 + 类型结果表 + 调用签名交验 | 合法程序全过、非法调用点报 diag |
| 9 | 语句级检查：初始化/赋值兼容 + 构造完备性 + 条件 bool + return 匹配 | 正负测对照 |
| 10 | DFA 确定性赋值 + §2.8 渲染器消费类型表补全回填 + §5-4 断言收紧 | kRichSource 全部 STMT_VAR 有 TYPE；子集断言无条件 |
| 11 | 泛型实例体 walk 期物化 + 检查 pass 统一 tops 表覆盖（§8.7） | 坏实参组合报定义体错误、好组合零诊断、检查失败 emit 返回 NULL |
| 12 | 泛型约束检查（§8.9）：interface 类型真实构建 + 实例体逐参约束判定 | 不满足报第一个缺失/签名不符成员或派生失败；好组合零诊断 |
| 13 | 物化对齐默认值与参数包协议（§8.10）：§6.2 缺参默认补齐 + §6.4 尾部包聚合 | 缺参无默认报 missing、省略与显式折叠同名、包空/展开零诊断 |
| 14 | 装饰器编译期计算（§8.11）：exec_apply_decorator 公有化 + pass C 成员装饰器链式应用 + STMT_STRUCT 装饰三段式 | 方法装饰器值级包装 end-to-end、同定义双实例独立计算、标记/失败路径回归 |

### 8.9 泛型约束检查（P12 设计稿）

语义来源：白皮书 §6.5（`[W extends IWriter]`——extends 后是类型模式或接口
约束；接口约束按**结构**判定，报第一个缺失或签名不符的成员）。exec 侧已有
对照实现（src/exec/generic.c `check_generic_constraint`：绑定后求值 + 结构
判定/派生判定），sema 与其**错误消息逐字对齐**，测试对照保证语义一致。

**载荷来源**：物化期 GENERICS 槽置 NULL（clone.c 实例体克隆），约束槽不在
物化体内——约束检查不从物化体取，而按 `sema_instance_body_t` 走（原树
`stmt` + `param_names` + `args` 齐备）。

**前置补全——interface 类型真实构建**：check 的 STMT_INTERFACE 目前占位
登记（type value described=NULL，`check_type_find` 恒 NULL）。P12 升级为
真实构建：成员签名逐个求值（签名节点与 `check_func_signature` 同构）经
`create_clux_interface_type` 入库，按名登记 type value。约束表达式由此可被
`check_type_eval` 解析出 interface 类型。**泛型 interface（`IWriter[W]`）
暂缓**：exec 侧明确不支持（expr.c "generic interface is not supported
yet"），P12 同步限定为非泛型 interface 约束 + 类型模式约束；F-bounded 中
引用正在绑定参数的 `IWriter[W]` 形态随泛型 interface 批次开放。

**检查协议**（check pass 内，类型环境齐备后对每个实例体）：

1. 按形参序取原树约束槽（`generic_param_node_get_constraint`）；
2. 约束表达式做一次 inst 态克隆（`param_names`→`args` 代换，复用物化
   代换通道）后交 `check_type_eval`，求值须为类型；
3. 约束对象 = 该实参类型（类型实参对 args 节点直接求值；值实参暂无
   约束检查通道，随 comptime 值参数批次定）；
4. interface 约束 → 结构判定：`clux_interface_member_count`/`get_member`
   逐成员查实参类型 attr 表 + `clux_type_equal` 比对签名；非 interface
   约束 → `clux_type_extends` 派生判定；
5. 不满足 → diag（消息对齐 exec），经 sema_has_error 汇聚，emit 返回
   NULL；全收集不 fast-fail。

**约束失败即剔除**（2026-11-26 定稿）：约束不满足的实例体从检查域剔除，
一条约束错误足矣——物化体 body 照常检查只会对坏实参产出一串级联二次
错误，无增量信息。与 §8.7「坏实参组合单一诊断」的测试思路一致。

### 8.10 物化对齐默认值与参数包协议（P13 已落地）

P11 物化通道按 `param_count == arg_count` 严格校验，§6.2 泛型默认值省略
与 §6.4 参数包两协议在 sema 侧不可达。P13 把 `sema_instant_register`
的校验与登记扩展为三段协议（exec 侧 generic.c 已有对照实现，错误消息
逐字对齐）：

- **位置段校验**：无包时多给保留旧消息（`"generic '%s' expects %zu
  type arguments, got %zu"`）；缺参逐尾部查 DEFAULT 槽补齐（从右向左
  省略，§6.2），无默认报 `"missing generic argument '%s'"`
  （`GenericInstanceArityMismatchIsDiagnosed` 断言随消息对齐更新）。
- **拼名**：位置段缺槽用 DEFAULT 值源文本（非名字形态记 '?'），保证
  `f[i32]`（省略 `U = i32`）与 `f[i32, i32]` 折叠为同一实例名
  `f$i32$i32`；module_declare 二次失败即静默去重，实例体按使用点
  各登记一条。
- **参数包**（尾部 `...Args`，即 GENERIC_PARAMS 末尾 EXPR_REST）：
  包槽聚合为 owned TUPLE 挂 `sema_instance_body_t.pack_arg`——元素是
  剩余实参的**深拷贝叶子**（按 kind+location 重建 name-form 节点，
  delete_node 递归释放子节点，不可借原树）；物化期 `...Args` 代换为
  `...<e1,e2,...>`，exec 的 exec_pack_tuple_type 天然支持，check 侧
  签名降级零诊断。destroy 时 pack_arg 随 body 递归释放。
- **parser 同步修复**：泛型参数 TYPE/CONSTRAINT 槽解析改用 ternary
  层——顶层 assign 会把 `[N: usize = 4]` 的 `= 4` 吞成赋值表达式，
  DEFAULT 分支永不触发（与捕获项 `( name: type = default )` 同层同修）。

### 8.11 装饰器编译期计算（P14 已落地）

装饰器此前只有 parser 期语法与 sema 登记通道，exec 期零计算。P14 按
§9.1/§9.6/§9.9 把装饰器的编译期调用落进 exec，协议以
`exec_apply_decorator`（stmt.c，已公有化）为核心：

- **apply 协议**：装饰器表达式求值为 func → 以当前值为唯一实参
  invoke（值级包装）；求值为其他值（metadata/类型值等编译期标记）→
  原样返回当前值（§9.1 标记类透传）；表达式缺失/求值 error/invoke
  error → 产出 error。
- **STMT_STRUCT 装饰三段式**（exec_stmt_struct）：①装饰器表达式先于
  壳 define 求值，失败即拦截——名字不 define；②全标记类 → 具名求值
  后直接返回 bound；③有 func → 具名两阶段构造（先 define 壳使装饰器
  体内自引用可解析）→ 按声明序逐个 invoke → **绑定保持壳值，包装结果
  只消费 error**（§9.6 类型装饰器编译期计算但不改写类型本体；早期
  「struct 包装成 i32」属人为行为，随本语义收紧废除）。
- **pass C 成员装饰器**（exec_expr_struct_decl_impl）：STMT_VAR/
  STMT_FUNC 成员按声明序链式 `exec_apply_decorator`——方法装饰后由
  包装函数接管 method 表，`u.get()` 经 member-call 糖真实走捕获链
  （end-to-end 见 `StructMethodDecoratorWrapsMethodInPassCEndToEnd`）。
  标记类装饰器（类型值等）不改写成员初值，原样透传。
- **泛型按实例计算**：泛型实例体经 sema walk 期物化（形参名→实参
  代换）后作为独立实例语句在 exec 期逐条执行，装饰器表达式（含代换
  后的调用实参）随每条实例语句独立求值——`Conf[i32]` 与
  `Conf[usize]` 的 `[[addn(B)]]` 各算各的，不共享缓存
  （`GenericInstanceDecoratorComputesPerInstance`）。
- **注意**：装饰器函数签名经 exec 急切求值
  （exec_func_decl_impl → exec_operand_type），被装饰类型须先于装饰器
  定义存在，前向引用将报 undeclared。

**留后续的三个缺口**（P14 不含，随对应批次开放）：

1. STMT_TYPE 声明上的装饰器 exec 期静默丢弃（parser 保留节点）；
2. FIELD（枚举字段）装饰器全链路零消费；
3. 带装饰器的 comptime var 随 emit 常量摘除丢弃（emit 期不回放装饰器）。
