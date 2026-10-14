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

当前结果：**七族 49/49 干净**（`optinner` 随下面两条缺口修复后也全通）。

> 附注：`closure_scope` 的生命周期**并非无人管理**——`vm_destroy` 会对每个函数
> 调 `scope_destroy(vm, &fn->closure_scope)`。此前的捕获泄漏根因是
> `value_own_copy_into` 没有回收 `ptr_clone_block` 的中间块（每构造一个含 own 的
> 字段泄漏 8 字节），**不含闭包的程序同样泄漏**；见 `minimal_probe.py` 的对照。

## 当前已知缺口

**（无）** —— 截至本次，矩阵的 119 + 49 格全部 `ok`。

### 已修复（留档，避免回退）

1. **`optinner` 全列**（原：`cannot initialize optional ?I with fatal I`）
   两层根因：sema 的 option 字段类型比对没把 fatal 折回 base；而折回时"按同布局
   重新 intern"对**具名 struct 不可靠**（标了 `unique`、不参与去重，每次新建实例）。
   修法：`type_fatal_variant_of` 创建变体时登记 `fatal → base`（`vm->fatal_bases`），
   折回查登记表拿规范身份。见 `docs/m3-design.md` §7.4。
2. **`13_fatal_return` 的 `array` / `union` 两格**（原：运行期
   `array: implicit cast only within layout-compatible array/tuple`）
   真正的原因**不是**元素类型不统一，而是返回值路径上**第 6 步**（safe_cast 到声明
   返回类型）抢先做了一次 `base → fatal` 的 cast —— 那是**非法方向**（各族
   `implicit_cast` 只认 `fatal → base` 的接管）。修法：声明返回类型 fatal 且带资源时
   第 6 步让开，交给第 7 步 `value_move` + `value_relabel_into`。
   struct / 指针族此前靠布局兜底偶然通过。见 `docs/m3-design.md` §7.5。
3. **`13_fatal_return` 的 8 字节泄漏**：随提交 `0732858` 修复
   （`value_own_copy_into` 未回收 `ptr_clone_block` 的中间块）。

### 尚未拦截（sema 侧，非缺陷但规则不齐）

捕获 **`own *T` 裸变量**已被拦（`cannot capture own/fatal pointer … use move()`），
但**含 own 字段的复合类型按值捕获（纯 id）未被拦**——目前走深拷贝语义（安全、无
泄漏），与 §7.2"复合类型赋值禁止 copy"不一致。是否要求显式 `clone()`/`move()` 待定。

### ref 聚合的作用域检查（另一个维度，见 `docs/m3-design.md` §5/§5.1）

含 ref 字段的聚合类型（struct/tuple/array/union）在三处强制/校验作用域标注：

| 位置 | 规则 | 状态 |
|---|---|---|
| 变量声明 | 含 ref 字段**必须标注**（`'<a>` / `'<*>`） | ✅ `check_var_annot` |
| 参数位 | 同上（顶层 `ref *T` 除外） | ✅ 参数检查 |
| 返回位 | 同上 | ✅ `sema_check_ret_ref_annot` |
| **赋值** | 标注 `'<A>` = "借的东西至少活到 A" → 要求 `借源作用域 ⊇ A` | ✅ `borrow_set` provenance（§5.1） |

**方向极易写反**：`'<*>` 是**最强**承诺。赋值的五条方向性用例
（`tests/driver_m3_test.cpp` 的 `Driver.StepBAggregate*`）：

| 用例 | 期望 |
|---|---|
| `g: '<*> H = .H{.r = p}`（承诺活到全局，实际只借到 p） | 拒绝 |
| `g: '<p> H = .H{.r = inner.&}`（只借到更内层块局部） | 拒绝 |
| `g: '<p> H = move(h)`，h 只借到块局部（继承借源） | 拒绝 |
| `g: '<p> H = .H{.r = p}`（同锚点） | 通过 |
| `g: '<p> H = .H{.r = gx.&}`（承诺比借源短） | 通过 |

复现用例：`repro/own-compound/ref_agg_assign_scope.cx`（通过侧）与
`ref_agg_assign_escape.rejected.cx`（拒绝侧）。

> 矩阵本身不覆盖 ref（矩阵面向 own 所有权扩散）；ref 的覆盖在
> `driver_m3_test.cpp` 与 `defer_test.cpp`。

