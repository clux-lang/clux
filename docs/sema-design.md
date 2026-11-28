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
  emit 预置起点），防 `$defer$N` 撞名。物化失败条目（防御态 NULL）
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
→  var $defer$0 = |h| { close(h); }(handle);   /* 捕获即拷贝 */
```

退出点插入 `$defer$0();`。

### 2.5 comptime if 剪枝

条件在 sema 内 CTFE 求值：真臂保留、假臂丢弃，节点替换为所选臂的块。
else-if 链递归处理。条件不可求值是硬错误。

### 2.6 comptime foreach 展开

编译期完全展开：容器是编译期已知的元素序列时，循环体按元素逐份拷贝，
迭代变量替换为对应元素子树。不可展开是硬错误。

### 2.7 foreach 脱糖（普通形态）

普通 foreach 脱糖为索引 for 循环，让 compiler 不必实现 foreach；iterable
提升为隐藏 var `$iter`（类型从 check 侧 iterable 节点锚点显式渲染），
`#len` 与下标引用 `$iter`，全循环只求值一次：

```
foreach (var item of items) { ... }
→  {
      var $iter: [3]i32 = items;
      var $i = 0;
      for (; $i < #len($iter); $i = $i + 1) {
          var item = $iter[$i];
          ...
      }
  }
```

- 迭代对象限定数组/切片/元组/可索引序列（白皮书 §4.9），
  `#len` 与下标是现有语言能力，零新构造。
- `var` 与裸名是两种语义（白皮书 §4.9）：`var` 每轮重新定义迭代变量；
  裸名前提是名字已在外层定义，循环体首句脱糖为赋值
  `item = $iter[$i];`（check 侧解析既有绑定并走 safe_cast 判定，
  exec 侧 assign 既有绑定），每轮给既有绑定赋值。

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

### 2.12 闭包退化（P3——嵌套递归退化 + 跨层捕获）

具名函数退化为一等函数值结构：`func f(...)` → `struct $closure$f`
（`$callee` 字段打头 + 逐捕获字段）+ `func $callee$f($env: *$closure$f, ...)`
纯函数（$env 首参自指）。调用点 `f(x)` 改写 `f.$callee(f.&, x)`；体内
捕获引用改写 `$env.<name>`；自递归 `$env.$callee($env, x)`。合成名走
§1.3 三分法：`$closure$M`/`$callee$M` 用 `$` 中缀连接，协议键
`$callee`/`$env` 用 `$` 前缀与用户捕获名/形参名隔离（捕获名 `callee`、
形参/捕获名 `env` 均放行并存）。

- **模块级条目**（含零捕获）：原位换 `var f: $closure$f = $closure$f`
  构造，声明与 callee 集中双轮合成（先全部 struct 后全部 callee，
  收集序）。
- **块级零捕获条目**（P2 纳入）：零捕获即无前向依赖，定义点**原位
  构造** `var N: $closure$M = .$closure$M{$callee = ...}`，不走 TDZ
  占位/块尾赋值，定义后立即可调用。
- **块级含捕获条目**：定义点换 TDZ 占位 `var N: $closure$M =
  undefined`，定义块块尾追加构造赋值。
  **var 捕获恒为块尾快照**——定义点不绑定现值；mid-block 调用读
  TDZ var 报错是**有意语义**（与 §4.17 析构登记的时序约定一致）。
- **退化资格**：具名 func（cfunc/generic/export/comptime/装饰器/
  匿名不退化）；块级另需 Scan-A/B/C 敏感名字域检查通过。
- **bail 收紧**（P2）：块级条目退化失败（定义位置不支持/二遍定型
  失败/Scan-A/B/C）不再静默跌回旧提升通道（提升为静态函数丢失
  闭包值语义——语义漂移），改报显式编译错误，emit 短路不产出
  Program。shape 不达标（cfunc/export/comptime/装饰器/泛型等）与
  struct 方法体内条目（§2.10 后续批次）保持旧通道；nested 场景
  由 P3 批次 1 纳入递归退化（见下）。
- **P2 已知局限**：函数值传递仅限非条目名位置；捕获定型依赖名字
  匹配（遮蔽场景显式报错）；captured disposable 值不登记析构
  unwind。
- **P3-1 合成名统一自增 id**（`sema->synth_seq` 全局单调）：函数名只在
  定义处作用域唯一，提升到全局须消歧——所有提升/退化合成名
  （hoist 通道 `Owner$Name$N` 与闭包通道 `$closure$Chain$Name$N`、
  `$callee$Chain$Name$N`）一律拼尾段 id（§1.3 `$` 中缀、与 `$defer$N`
  同构），walk 期提升项与 emit 期模块级退化条目共用同一序号流。
- **P3-1 边界**：id 只保证合成名互异；引用改写通道（闭包退化引用
  收集、提升 rename 表）仍按名字文本工作，同函数内同 owner 同名
  遮蔽仍显式报错（`ambiguous local declaration`）——AST 包含判定
  落地（P3 批次 1）后放开。
- **P3 批次 1——嵌套资格 AST 包含判定**（替代旧名字前缀检测）：
  直接容器 = 条目表中体子树包含候选 stmt 的最深条目（walk 先序
  收集，祖先先于后代，最后命中即最深）；模块级条目（owner NULL）
  天然可作容器。嵌套候选照常入表，克隆语境（skip/块尾构造/调用
  改写）按 owner（恒为顶层函数 stmt）+ 原树块/句指针匹配，容器体
  克隆时天然落位，clone.c/sema.c 零改动。
- **P3 批次 1——跨层捕获**：捕获来源 S = 直接容器（否则 owner 兜底
  ——未退化顶层函数体克隆同样携带 closure_owner，机制一致）。可见
  名集三来源：S 形参（plain，TYPE 槽为克隆源，构造点 `.a = a`）、
  S 捕获（via_env，构造点渲染 `$env.<name>`，借用容器捕获锚点）、
  S 体根至定义点的 STMT_BLOCK 链上全部直接子级 STMT_VAR（§8.5 锚
  点 + 可存储校验；**含后向 var**——构造点在块尾，取值时已定型）。
  深层路径 var 遮蔽浅层与形参。
- **P3 批次 1——隐式发现与捕获定型**：发现扫描把候选体内自由名
  收敛为捕获（`discovered` 标志区分显式/发现式）；发现式锚定仅限
  源可见集，显式裸捕获保留定义块全量 var 兜底（旧后向语义）。
  签名默认值不进 callee 合成（既有行为，不构成悬空）。
- **P3 批次 1——bail 语义**（一律显式报错，emit 短路）：
  defer/非候选嵌套边界内引用外层名（Scan-B）；经中间闭包的跨层
  捕获链；按名引用容器闭包（Scan-C，容器自身 bail 后不入表，其
  嵌套条目级联报"defined inside a nested function"——两条 diag
  均指向真实问题）。Scan-B 放行域 = 包含条目的祖先候选子树
  （STMT_FUNC 及其声明本体 EXPR_FUNC_DECL——后者为前者的唯一
  直接子级，不放行会在递归中再落 boundary 整树检查误伤嵌套条目
  体内引用）。
- **P3 批次 2——同名遮蔽放开**（三通道协同，check 预扫描为根）：
  同函数内兄弟块同名提升声明（含退化条目）合法化。
  ① check 层块级预扫描替代整树平铺——`check_local_scan` 对
  STMT_BLOCK 截断（嵌套块声明不再平铺提升进函数帧 scope），walk
  进入每块时对直接子级自扫登记进块 scope，scope 链就近命中实现
  遮蔽；函数体直下声明仍由 `check_func_decl` 帧入口登记进帧
  scope（形参/返回类型前向引用依赖），walk 进入函数体块时跳过
  二次扫描（`frame_body_block` 标记——装饰器求值随扫描走，重复
  扫描即重复诊断）。这是**第三条按名通道**的发现起点：check 名字
  解析此前也按首个同名命中，遮蔽放开必须三层（check/提升
  rename/闭包 Scan）同步。
  ② per-site 解析表（`sema->site_resolves`）——提升 rename 表按
  名字文本匹配，同名提升项首条命中即错改；改按名字叶指针消歧：
  DFS 块层栈（全块收集直接子级提升声明，退化条目也入层）把每个
  名字叶解析到最近同名提升项。clone 消费点 site 命中优先、文本
  匹配兜底（无歧义时等价）；调用点 desugar 亦按它定位退化条目。
  构建时机约束：site 表在 sema_emit 内惰性构建，晚于闭包 collect
  （跳过判定依赖条目表）——块级 Scan-B 执行时不可查 site 表，
  故闭包放行采用独立的结构化判定（③）而非逐叶查询。
  **模块级退化条目不跳过 site walk**（owner==NULL：全局不存在
  闭包、零捕获，其 callee 体克隆挂 renames 消费，体内 hoist 引用
  必须走 per-site 消歧）；块级（嵌套）条目子树仍整树跳过（体内
  自由名已收敛为捕获，cap 改写先于 renames 消费）。
  ③ 闭包 Scan 同名放行——块直接子级含非条目的同名提升声明时，
  块内全部该名字引用词法解析到该声明（块级预扫描语义：块域
  前向可见），Scan-B 整块放行、Scan-C 对 owner 体根至提升项路径
  穿过同名遮蔽块的条目放行。纯 AST 位置判定，不进函数/defer
  边界语义。
- **P3 批次 2——覆盖与残留**：四场景测试（兄弟块同名闭包独立
  退化/兄弟块同名 struct 按 scope 消歧/条目体内深层同名遮蔽递归
  退化/同名闭包与 struct 跨层消歧）。已知不支持边：同层同名不同
  种类（同一块内既有 struct S 又有 func S）。Scan-A 残留缺口：
  条目体内深块再声明同名 + 引用的改写正确性未覆盖（场景③特意
  不写内层调用点避开）。emit 对 synth_seq 不幂等（重复 sema_emit
  对模块级条目二次取号致 id 漂移）——测试基建 emit 恰调一次。

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
| 模块级 var 初值编译期可知（C 规范） | §8.15 形态白名单（字面量/合法名字/限定二元/`.&` 地址常量/聚合/构造） | §8.15 |

### 8.5 类型结果表（→ §2.8 渲染器）

- sema 内 per-node 表（ctx allocator，随 sema 销毁）：**第一版只记
  STMT_VAR / 构造字段 / foreach 迭代变量 → `clux_type_t*`**（借用，
  类型对象归 context）。其余表达式的类型只是演算中间态，不入表。
- emit 侧消费：克隆器的 STMT_VAR 后处理（`sema_clone_var_fixup`）
  在 TYPE 槽为 NULL 时查表回填渲染类型名——正是 Phase 6 遗留的
  `r / z / q / $defer$0` 缺口的解法。
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
| 15 | check 层装饰器检查通道（§8.12）：表达式求值诊断 + func 形态签名交验 + 挂点全覆盖（模块 register/install_methods/check_stmt） | 未定义名/签名不符恰报一条且消息与 exec 逐字一致、标记/TDZ 零诊断、包装后类型可被后续消费验证 |
| 16 | 类型位置装饰器语义收尾（§8.13）：STMT_TYPE 装饰器 exec 真实计算 + FIELD 装饰器全链路消费 + check 端 eval-only 同构 | 标记透传不改写类型本体、求值/invoke error 在 define 前拦截名字不落地、check 端未定义名报而标记零诊断 |
| 17 | 泛型 interface 实例化与 F-bounded 约束（§8.14）：exec 定义产 type 模板 + 下标实例化通道 + check 端模板登记/SUBSCRIPT 求值/F-bounded 绑定环境内真实求值 | 定义产 TYPE_TEMPLATE、实例成员签名按实参代换、同组合去重恒等、F-bounded 好组合零诊断坏组合单一约束诊断 |

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
`check_type_eval` 解析出 interface 类型。~~**泛型 interface（`IWriter[W]`）
暂缓**~~：P17 已开放（见 §8.14）——exec 侧定义产出 type 模板 + 下标
实例化通道，check 侧模板登记 + `check_type_eval` 的 SUBSCRIPT 实例求值
通道；F-bounded 中引用正在绑定参数的 `IWriter[W]` 形态在实例绑定环境内
真实求值。

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

**缺口收敛**（原三条，P16 后仅余一条）：

1. ~~STMT_TYPE 声明上的装饰器 exec 期静默丢弃~~（P16 关闭，见 §8.13）；
2. ~~FIELD（struct 字段）装饰器全链路零消费~~（P16 关闭，见 §8.13）；
3. 带装饰器的 comptime var 随 emit 常量摘除丢弃（emit 期不回放装饰器）
   ——真正应用 func 装饰器需 CTFE 支持函数 invoke，随 CTFE 批次开放。

### 8.12 check 层装饰器检查通道（P15 已落地）

P14 之前装饰器表达式在 check pass 零检查零诊断——未定义名、类型错误
全部静默通过。P15 把装饰器纳入检查，核心是 `check_decorator`
（check.c，协议与 `exec_apply_decorator` 对齐）：

- **apply 协议**：
  - 表达式缺失 → `"decorator requires an expression"`（与 exec 逐字
    一致——parser 容错产出的残缺节点走此通道）；
  - 求值真 error → `check_error_diag` 后返回 NULL（链条降级
    eval-only，不级联）；
  - 求值为 func 且 subject 可演算 → 以 subject 的 **shadow 值**为
    唯一实参走 `check_call_value`（个数/实参类型交验，消息与运行期
    invoke 一致），通过返回包装后类型；
  - 其余（标记类/模板/不可演算/subject 未知）→ 原样返回 subject
    （§9.1 标记透传、§9.6 类型装饰器不改写类型本体、模板随实例化
    通道逐实例检查）。
- **链式包装**：subject 沿装饰器链传递，`bound`/`subject` 双变量——
  失败回退 **last-good 类型登记**（与 exec「失败不 define」不同：
  check 的登记是检查环境本身，回退防级联假阳性）。
- **挂点分工**（hoisted 成员语句不在 tops 里，方法装饰器检查唯一
  归属 install_methods，避免与 hoist 副本双报）：
  - `check_module_register`：模块级 var（有初值且非 undefined 才
    链式包装）与 func（签名链式包装，define 前、与 exec 同序）；类型
    族语句（struct/enum/union/bitfield/interface）顶层装饰器在 define
    前 **eval-only** 求值（对齐 exec P14 三段式的拦截语义，自引用
    报 undefined）+ struct 的 attr 成员装饰器（`check_struct_attr_
    decorators`：TDZ/无初值跳过，与 exec 同构）；
  - `check_struct_install_methods`：方法签名链式包装（exec pass C
    包装值接管 method 表的静态镜像）；
  - `check_stmt`（局部）：var 装饰后类型进 bind；嵌套 func 对预扫描
    登记的 shadow 签名就地 `clux_value_retype`（error 类型占位排除，
    防签名失败后误报 argument type 错）；局部类型族语句顶层 eval-only。
- **环境一致性**：模块级 register 循环源码序 = exec 模块语句源码序
  （装饰器引用后续名两端同报 undefined）；局部检查放 check_stmt（预
    扫描名字全就位 = exec 块提升环境），而非 check_local_scan（scan
    序会漏后续提升名产生假阳性）。
- **跳过项**：STMT_TYPE 装饰器不再跳过（P16 起纳入 eval-only，见
  §8.13）；模板定义分支（实例体的克隆副本通过模块 register 逐实例
  检查——`GenericInstanceBodyDecoratorErrorPerInstance` 每实例各报
  一次）；TDZ（`= undefined`）/无初值 var（exec 不适用装饰器）。
- **已知边界**：comptime var 装饰器照常检查（exec emit 丢弃是 §8.11
  缺口 3，验而不应用）；本地 struct 顶层装饰器引用壳名属假阴性（壳
  在预扫描登记，签名不可演算时 eval-only 降级）；attr 成员仅做装饰
  器表达式检查（attr 安装通道不存在，值级语义无检查域）。

### 8.13 类型位置装饰器协议（P16 已落地）

P14/P15 之后仅剩类型位置装饰器无语义：STMT_TYPE 装饰器 exec 静默
丢弃、FIELD（struct 字段）装饰器全链路零消费。P16 把两处补齐，
并固化**正交规则**：值位置装饰器（var/func/method）check 端做签名
交验；类型位置装饰器（STMT_TYPE 别名 + struct FIELD 字段 + 五类
类型族顶层）check 端一律 **eval-only**，func 类的 invoke 交验留 exec
运行期报告。

**eval-only 的技术依据**：exec 端 func 装饰器对类型位置以 **type
value**（值类型为 type_type，TYPE kind）为唯一实参 invoke——check
若传字段/别名类型作 subject，两端实参形态不一致（假阳/假阴）；且
type value 属 TYPE kind，会被 `check_call_value` 的 opaque gate
（不可演算实参不硬报）跳过，check 层强行模拟 invoke 得不到任何交验
结果。P15 对 STMT_STRUCT 顶层 eval-only 是同一先例。

- **exec STMT_TYPE**（exec_stmt_type 非泛型分支）：seal 门禁通过后、
  define 前对装饰器链逐个 `exec_apply_decorator`（subject = 别名的
  type value）——func 类编译期计算，**包装结果只消费 error**（绑定
  保持原 type value，§9.6 不改写类型本体）；标记类透传；失败在
  define 之前拦截（名字不 define，与 struct 三段式一致）。
- **exec FIELD**（exec struct pass B，字段类型求值成功后、入表前）：
  以 `clux_context_create_type_value(ctx, 字段类型, NULL, loc)` 为
  subject 链式 apply——type value 不具名（name=NULL 不进词法作用域）；
  结果同样只消费 error，字段类型保持声明本体；失败置 fail，struct
  不 define。仅 struct 字段允许装饰器（parser 对 tag union/cunion
  字段 allow_decorators=false）。
- **check 端三处 eval-only**：`check_module_register` STMT_TYPE
  非泛型分支（define 前）、`check_stmt` 局部 STMT_TYPE（与模块级
  同构）、`check_build_struct` FIELD 循环（`check_type_eval` 后）——
  均只做装饰器表达式求值诊断（undefined name 等），func 形态不交验。
- **测试锚**：exec 端 `TypeAliasMarkerDecoratorDefinesAlias` /
  `TypeAliasDecoratorErrorBlocksDefine` /
  `TypeAliasFuncDecoratorResultIsDiscarded`（执行器懒安装证 invoke
  真实发生）/ `TypeAliasFuncDecoratorArityErrorBlocksDefine` /
  `StructFieldDecoratorErrorBlocksDefine` /
  `StructFieldDecoratorKeepsFieldType`；check 端
  `TypeAliasDecoratorUndefinedNameIsReported` /
  `TypeAliasDecoratorMarkerHasNoDiag` /
  `StructFieldDecoratorUndefinedNameIsReported` /
  `StructFieldDecoratorMarkerHasNoDiag`。
- **测试通道注意**：元类型 `type` 不在 builtin 命名注册表（AST 无法
  表达"接收 type value 的形参类型"），测试中构造 func 装饰器须走引擎
  直建（create_func_type + func_set_ast + declare_value），与
  `StmtUnionDecoratorWrapsTypeValueEndToEnd` 同一通道。

### 8.14 泛型 interface 实例化与 F-bounded 约束（P17 已落地）

语义来源：白皮书 §6.1/§6.6/§6.10。P12 时泛型 interface 在 exec 与
check 两侧均为明确暂缓项（expr.c 拒绝 + 约束限定非泛型），P17 将其
对齐 func/struct 的既有"定义产模板 + 下标实例化"协议，并放开 F-bounded
约束（`[T: IWriter[T]]`——约束引用正在绑定的形参）。

**exec 侧（定义/实例同构协议，与 struct 先例一致）**：

1. **定义产模板**：`exec_expr_interface_decl` 命中非空 GENERICS 槽时
   产出 `CLUX_TYPE_TYPE_TEMPLATE` 模板值（`clux_context_create_type_template`，
   data 存声明 AST，§6.1），具名时照常 define；
2. **实例化通道**：generic.c 的 GENERICS 槽取值（`template_params_node`）
   与重执行分派（`exec_expr_interface_decl_instance`，is_instance=true）
   各加 INTERFACE_DECL case——在绑定 scope 内重执行建实例 interface
   类型，不具名不 define，实例值归实例化块出口 clone；同组合经引擎
   结构去重恒等（§6.10）。

**sema walk 侧**：`g_sema_decl_meta` 的 STMT_INTERFACE 行补 generics_of
（解包 INTERFACE_DECL 取 GENERICS 槽）——泛型 interface 定义自动进入
模板物化通道：使用点 `EXPR_SUBSCRIPT` base 命中泛型名即拼实例名登记
实例体，物化走 clone env（GENERICS 槽 kind 级摘除 + 成员签名形参经
代换通道替换为实参树），实例体与非泛型定义同构。

**check 侧**：

1. **模板登记**：`check_module_register`/`check_local_scan` 的
   STMT_INTERFACE 分支对非空 GENERICS 走 `check_define_template`
   （载荷传 STMT_INTERFACE 包装节点，与 struct/union 一致），非泛型
   保持原样；
2. **SUBSCRIPT 实例求值**：`check_type_eval` 新增 EXPR_SUBSCRIPT
   case——base 名经 `clux_scope_get` 查 TYPE_TEMPLATE 值取载荷 AST，
   STMT_INTERFACE 解包后：参数包/comptime 值参/实参数不齐静默 NULL；
   否则 push scope 逐参把实参求值结果 define 为类型值，同一环境
   `check_build_interface_decl` 构建实例 interface 类型，pop 返回；
   `check_build_interface` 拆为 decl 级构建器 + 解包转发（物化实例体
   GENERICS 已摘、形参已代换，与非泛型定义同构构建）；
3. **F-bounded 真实求值**：`check_generic_constraints` 检测约束表达式
   引用形参名（`check_constraint_refs_param`）时，push 临时绑定 scope
   把全部形参按实例实参 define 为类型值，在同一环境求值约束
   （SUBSCRIPT 通道解析 `IWriter[T]` 实例），pop 后照常走
   `check_constraint_satisfied` 结构判定——不再对 F-bounded 形态
   continue 静默放行。

**测试锚**：exec 端 `GenericInterfaceInstantiationBuildsInstanceType`
（模板 kind/lookup/AST 载荷、i32/str 实例成员签名代换、二次去重
恒等）；check 端 `FBoundConstraintSatisfiedHasNoDiag` /
`FBoundConstraintMissingMemberReportedOnce`（好组合零诊断；坏组合
单一约束诊断且失败实例体剔除）。

### 8.15 模块级 var 初始化器编译期可知性（C 常量初始化器规则，已落地）

跟随 C 静态存储期初始化规则：**模块级普通 var 的初值必须是编译期可知
表达式**，运行期求值形态一律拒绝（check.c `check_modvar_init_is_const`）。
消息：`"initializer of module-level var '%s' is not a compile-time
constant"`（对齐 C "initializer element is not constant" 风格）。

- **形态白名单（递归）**：
  - 全部字面量 kind（undefined/string/character/numeric/boolean）；
  - **合法名字**：命 中 const_defs 表（comptime 常量）或 scope 值
    kind == FUNC（函数名**地址常量**——C `int (*f)(void) = g;` 的
    对应物）；普通 var 占位（ERROR kind）与类型名（TYPE kind）不放行；
  - **二元运算**：运算符限定算术/位/比较表 + LEFT 为 NULL 的一元
    prefix 形态——member `"."`、下标 `"[]"`、赋值 `"="` 等同为
    EXPR_BINARY 形态，**按符号拒收**（防只看 kind 的白名单漏过）；
  - **地址常量 `".&"`**：后缀取址是 C 地址常量（`&b` 不读 b 的值、
    只取符号地址），操作数放宽为**任意模块级已声明名字**（普通 var
    占位亦含——前向引用以指针模式回归）；非纯名字操作数（字面量/
    成员取址等）拒收（C 中字面量非对象）；
  - 元组聚合 / 构造（TYPE 槽与具名字段名槽跳过，只递归字段值）。
- **comptime var 独占通道**：初值合法性由 walk 期 `sema_ctfe_eval`
  校验（"not a comptime value" 等），check 循环跳过 comptime var
  防双报。
- **语义收窄与指针模式**：原 #248「模块级 var 前向引用顺序无关」
  特性随严格 C 规则收窄为**值引用不可、指针模式可**——`var a = b;
  var b = 1;`（值形态）仍违规；`var a = b.&; var b = 1;`（地址
  常量）合法（C `int *a = &b; int b = 1;` 对应物）。循环依赖的值
  形态在前置拦截下不可能出现，原不动点解析循环移除。
- **两遍定型**：check 循环分两遍——第一遍合法性判定 + **值型**定型
  （含 `".&"` 的地址型暂跳，`check_modvar_contains_addr`）；第二遍
  **地址型**定型（此时被引用 var 已由第一遍回填，`".&"` 影子演算
  产 \*T，`check_modvar_addr_is_ready` 前置判定操作数就绪）。指针
  模式的**自引用**（`var a = a.&`）与**互引用**（`var a = b.&;
  var b = a.&`）类型无穷回归不可解（C 靠显式类型标注兜底）——未
  就绪者跳过定型（0 diag、占位留 error，现状固化待白皮书定夺）。
- **check 期 comptime 名字解析**：comptime 常量真值在 const_defs 表
  （walk 期 CTFE 结果 `sema_const_t`），scope 里 P7 预扫描登记的是
  error 占位 shadow——`check_lookup_value` 命中占位时查 const_defs
  按 kind 物化对应类型 shadow 参与演算（bool 内建 / INT·FLOAT 按
  type_name 查类型），普通 var 占位不在表不受影响。
**测试锚**：`ModuleVarConstInitializerFormsAreAccepted`（白名单全覆盖
+ 函数名地址常量）/ `ModuleVarRuntimeInitializerFormsAreRejected`
（call/ternary/member/subscript/builtin 各一条）/ `ModuleVar
ComptimeReferenceTypeIsInferred`（comptime 引用定型回填）/ 三个
`*ReferenceIsRejected`（普通 var 引用/互引用/自引用——值形态）/
`ModuleVarAddrOfForwardReferenceIsAccepted`（指针模式前向引用定型
\*i32）/ `ModuleVarAddrOfFormsAreAccepted`（顺序/函数/comptime/元组
内嵌取址）/ `ModuleVarAddrOfCyclesStayUntyped`（自引/互引用指针
模式不定型现状固化）。
