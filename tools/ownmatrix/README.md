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

## 闭包 / defer 捕获维度

`closure_matrix.py` 追加一个维度：**族 × 捕获写法**（7 族 × 7 种写法 = 49 格），
每格除断言字段值外还检查泄漏报告。

| 写法 | 内容 |
|---|---|
| `C1` | 纯 id 捕获 |
| `C2` | 括号捕获 + `move` |
| `C3` | 捕获后把源 `move` 走 |
| `C4` | 捕获后再 `clone` 源 |
| `C5` | 同时捕获两个含 own 的值 |
| `C6` | `defer` 捕获（复用同一套捕获解析） |
| `C7` | 块内捕获（闭包更早释放） |

当前结果：struct / nested / deep / tuple / array / union 六族 **42/42 干净**；
`optinner` 7 格被下面第 1 条缺口挡住（与闭包无关）。

> 附注：`closure_scope` 的生命周期**并非无人管理**——`vm_destroy` 会对每个函数
> 调 `scope_destroy(vm, &fn->closure_scope)`。此前的捕获泄漏根因是
> `value_own_copy_into` 没有回收 `ptr_clone_block` 的中间块（每构造一个含 own 的
> 字段泄漏 8 字节），**不含闭包的程序同样泄漏**；见 `minimal_probe.py` 的对照。

## 当前已知缺口

1. **`optinner` 全列**：`var a: ?I = .?I{ .I{ .p = new i32{7} } }` 报
   `cannot initialize optional ?I with fatal I`。sema 的匿名构造槽位把 option 的
   内层推断成了 `fatal I`，而 `?I` 要的是内层 `I`。
2. **`13_fatal_return` 的 `array` / `union` 两格**：运行期报
   `array: implicit cast only within layout-compatible array/tuple`。根因是
   fatal 变体的**元素类型不统一**——`fatal [2]I` 与 `fatal [2]fatal I` 互不相等
   （指针/切片由 `type_fatal_variant_of` 升为 fatal，而数组/union 的
   `*_intern_with_ownership` 保留了原元素类型）。
   `struct` / `tuple` 的对应格已通过（`value_relabel_into` 就地重贴标签）。

> 早先记录的 `13_fatal_return` 8 字节泄漏已随提交 `0732858` 修复（同第 1 条根因的
> 副作用）。
