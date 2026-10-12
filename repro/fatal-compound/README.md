# repro/fatal-compound —— 复合类型跨函数边界的 move/clone（fatal T）

这些是最小复现程序，用 `build/clux.exe run` 实跑验证过。**目录里保留的是修复后
应当全绿的对照程序**（`probe1`~`probe5` + `t_*`），`probe3` 是"必须被拒绝"的
反例。缺陷的回归测试见 `tests/driver_fatal_compound_test.cpp`。

构建：`cmake --build build --target clux`（不要 clean）。
运行：`.\build\clux.exe run repro\fatal-compound\<name>.cx`（**反斜杠**或
`./repro/...` 均可；`clux run` 只把 `./ ../ / C:/` 形式的参数当文件路径）。

---

## 缺陷

结构体等复合类型**含 own 指针字段**时，穿过函数边界（参数/返回值）会被隐式
浅拷贝：`memcpy` 只复制裸指针，源与目标各自在作用域退出时递归销毁**同一堆块**
→ double free（实测 access violation，退出码 `0xC0000005`）。

同一缺陷有两个入口，此前都无人拦截：

| # | 触发 | 修复前 |
|---|---|---|
| 1 | `move(x)` / `clone(x)`，x 为含 own 字段的 struct | `op_move`/`op_clone` 只处理 PTR/SLICE，复合类型落到 `value_clone` → `struct_clone` 的整块 `memcpy`（浅拷贝）→ 双释放 |
| 2 | `func read(b: Box)` + `read(a)` | `Box` 的 ownership 是 `OWN_NONE`，参数校验里 `type_is_own(Box)` 为假 → 浅拷贝静默通过 → 被调用方释放了调用方的堆块 |
| 3 | `func mk(): Box { return b; }` | 返回值的隐式拷贝拦截只覆盖 `own/fatal` 指针/切片；含 own 字段的复合类型由 `type_contains_own_or_fatal` 拦（已修） |

## 修复

引入 **`fatal T`**（T 为含 own 资源的 struct/tuple/array/option/union）：与 T
**逐字节同布局**、`ownership = OWN_FATAL` 的将亡值变体类型，是含 own 资源的
复合值跨函数边界的唯一载体。

- **move**：整块 `memcpy` + 递归清空**源**顶层 own/fatal 指针（源不再持有）；
- **clone**：`ptr_clone_block` 递归深拷贝（own 字段分配独立新堆块），**源保持
  有效**（clone 是复制语义；转移走 move / fatal 参数与返回值）；
- **接管**：`fatal T → T` 的隐式转换是**转移**（memcpy + 清空源），不是拷贝；
- **拒绝**：目标持有 own 资源而来源不是将亡值 → 编译期报错，要求
  `move()`/`clone()`（只想借用就写 `ref *T` 形参）；
- **销毁**：`pop_scope → ptr_free_owned_recursive` 按字段偏移递归，与 ownership
  无关 → `fatal Box` 与 `Box` 走完全相同的销毁路径（就绪，无需特判）。

## 用例表

| 文件 | 覆盖 | 期望 |
|---|---|---|
| `t_move_local.cx` | 局部 `move(struct)`：源置空、无双释放 | exit 0 |
| `t_param.cx` | `fatal Box` 形参 + 函数内 `move` 接管 | exit 0 |
| `probe2.cx` | 局部 `clone` + `move` 连续切换 | exit 0 |
| `probe3.cx` | **反例**：`func read(b: Box)` + `read(a)`（隐式拷贝） | exit 1（明确诊断） |
| `probe1.cx` | `fatal Box` 形参 + `fatal Box` 返回值（完整跨边界链） | exit 0 |
| `probe4.cx` | 同上（`make` 是语言关键字，已改名 `mkbox`） | exit 0 |
| `probe5.cx` | move/clone 语义：深拷贝隔离、嵌套 struct、元组含 own、数组元素深拷贝 | exit 0 |
| `t_fatal_array.cx` | `fatal [2]Inner` 形参（数组 fatal 变体 + 跨边界转移） | exit 0 |
| `t_forward.cx` | `fatal Box` 参数原样 `return` 转发 + 局部 `move` 数组 | exit 0 |
| `t_ptr.cx` | 对照：指针 move 未被本次改动破坏 | exit 0 |

## 已知边界

- `fatal T` 只对**含 own 资源**的 T 产生新类型；`fatal Point`（不含 own）折叠为
  `Point` 本身——否则会凭空造出两个互不兼容的类型（`probe5` 的
  `FatalOnResourceFreeTypeIsIdentity` 用例覆盖）。
- `own T` / `ref T`（T 非指针/切片）非法：复合值没有"独占/借用"这回事，借用走
  `ref *T`。parser 直接报错。
- 运行期 `func_vcall` 的 fatal 参数接收 / 返回值交回走**转移**（`value_move`）
  而非深拷贝；判定用 `type_fatal_carries_resource`（只有真带堆资源的 fatal 才转
  移）。
