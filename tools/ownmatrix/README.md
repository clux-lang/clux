# 所有权矩阵探针（类型族 × 操作）

本目录是"含 own 指针的复合类型"的**全量覆盖**巡检工具：把类型族与操作组合成
矩阵，逐格生成一个最小 clux 程序、逐格实跑，按 exit code / 崩溃码归类。

存在理由：这一族代码的失效形态要么是**段错误**要么是**内存泄漏**，点状抽查
（"每种类型挑一个用例"）会漏掉整整一类组合。2026-10-13 用它一次跑出 19 格
崩溃（全部集中在赋值与函数边界），修完六处根因后降到 0 格。

## 用法

```sh
# 1) 生成矩阵（默认写到 <repo>/build/tmp/matrix）
python tools/ownmatrix/gen_matrix.py

# 2) 批量实跑并汇总（输出矩阵表 + 坏格清单 + REJECT 明细）
python tools/ownmatrix/run_matrix.py
```

两个脚本都只用标准库，路径按仓库根写死（`D:\projects\clux\...` 形式在脚本内的
`DIR` / `CLUX` 常量里，换机器时改这两处）。

## 矩阵维度

**类型族（7）**

| 族 | 类型 | 覆盖点 |
|---|---|---|
| `struct` | `struct T { p: own *i32; n: i32; }` | 直接 own 字段 |
| `nested` | `T { inner: I; n }`，`I { p: own *i32 }` | 一层内嵌 |
| `deep` | `T { m: M; n }`，`M { i: I; n }` | 两层内嵌 + 中间层标量 |
| `tuple` | `.<own *i32, i32>` | 元组元素 |
| `array` | `[2]I` | 数组元素（多个 own 块） |
| `union` | `union T { p: own *i32; n: i32 }` | tag union 的激活 member |
| `optinner` | `?I` | option 内层含 own（**当前已知未支持**，见下） |

**操作（17）**

`01` 显式标注构造 · `02` 推断构造 · `03` move 到局部 · `04` clone 到局部 ·
`05` move 后丢弃 · `06` 直接丢弃构造 · `07` 内层作用域退出 · `08` 赋值 move ·
`09` 赋值 clone · `10` 作数组元素 move · `11` 作数组元素 clone ·
`12` fatal 形参 · `13` fatal 返回 · `14` 具名实参 move 传入 ·
`15` fatal 形参原样 return（转发） · `16` 二次 move · `17` move 后 clone

每格断言**字段值**（`a.p.* == 7 && a.n == 3` 之类），所以"跑通"不只是没崩，
值也必须对。

## 结果分类

| 类别 | 含义 |
|---|---|
| `ok` | exit 0 |
| `REJECT` | exit 1 —— **需人工判定**：合理的语义拒绝（如普通 `T` 形参应改 `fatal T`）还是真缺口 |
| `ACCESS_VIOLATION` / `HEAP_CORRUPTION` / … | 崩溃码，**真 bug** |
| `TIMEOUT` | 挂起（可能是析构死循环） |

`run_matrix.py` 会把非 `ok` / 非 `REJECT` 的格子单独列出来（那才是必须修的），
并把所有 `REJECT` 的首条诊断附上供判定。

## 当前已知缺口（矩阵里表现为 REJECT，已确认是缺口而非合理拒绝）

1. **`optinner` 全列**：`var a: ?I = .?I{ .I{ .p = new i32{7} } }` 报
   `cannot initialize optional ?I with fatal I`。sema 的匿名构造槽位把 option 的
   内层推断成了 `fatal I`，而 `?I` 要的是内层 `I`。
2. **`13_fatal_return` 的 8 字节泄漏**：`var o: T = mkone();`（`mkone` 返回
   `fatal T`）程序 exit 0，但收尾报一个 `clux.vm.value_data` 泄漏。

两项都记在提交 `6b72e74` 的说明里，尚未修复。
