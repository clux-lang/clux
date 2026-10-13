# repro/own-infer —— 推断即可：`var a = .T{ .data = new i32{123} }`

锁的是一条**跨提交的交互**，两半必须同时成立：

- **fatal 自动推断**（`17839d9` / `ca00b91`）：`var` 推断时把 fatal 剥离成可用类型
  ——`fatal T`（复合）→ 基础类型 `T`；`fatal *T` → `own *T`；`fatal []T` → `own []T`。
- **构造产 fatal T**（所有权扩散）：构造表达式的产物类型是 fatal T，编译器在构造点
  发 `LOAD_TYPE <fatal 变体 id>`。

于是下面这种写法**不需要**显式标注 `a` 是 `own Test` 还是 `Test`：

```rust
struct Test { data: own *i32; }
var a = .Test{ .data = new i32{123} };    // 推断为 Test
func mk(n: i32): fatal Test { ... }
var b = mk(7);                             // 推断为 Test（fatal 被剥离）
var p = new i32{9};                        // 推断为 own *i32
```

## 合流的正确形态（bytecode 实证）

```
LOAD_TYPE <fatal 变体>     ← 构造点
... CONSTRUCT N            ← 运行期产出 fatal 值
LOAD_TYPE <基础类型 T>     ← DEFINE 类型槽位（推断写回的显式类型）
DEFINE "a"                 ← op_define → value_implicit_cast：
                              fatal T → T 的**接管转移**（堆块归变量，不泄漏）
```

缺任何一侧都会退化：只有推断 → 接管无从发生；只有"构造产 fatal"而没有推断写回 →
运行期值带 fatal 类型、变量声明为 T，转移判定落空 → 变量析构不释放堆块（泄漏）或与
临时值双释放。

## 用例表

| 文件 | 覆盖 | 期望 |
|---|---|---|
| `probe1_construct.cx` | 用户给的确切形态：构造推断，不写任何标注 | exit 0 |
| `probe2_fatal_return.cx` | `fatal Test` 返回值推断为 Test | exit 0 |
| `probe3_fatal_ptr.cx` | `new` 产 fatal 指针 → 推断为 `own *i32` | exit 0 |
| `probe4_nested_agg.cx` | 嵌套 struct / 含 own 的元组 / 含 own 元素的数组 | exit 0 |
| `probe5_scoped.cx` | 内层作用域的推断变量：退出时正确析构 | exit 0 |
| `probe6_explicit.cx` | 显式标注（`Test` / `own *i32`）与推断两条路都通 | exit 0 |

回归：`tests/driver_own_infer_test.cpp`（6 例，与上表一一对应）。

设计说明：`docs/m3-design.md` §7.3「推断与扩散的合流」。

> 注：本目录内 `Test` 是**结构体类型名**，与语言关键字无关。
