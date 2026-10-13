# repro/own-compound —— 含 own 指针的复合类型遵守单所有权体系

**所有权扩散**：复合类型（struct / tuple / union / option / array）只要**递归地**
含 own 指针字段，它整体就进入单所有权体系——与 `own *T` 完全同规则。这份目录是
该规则的最小规格说明（每个文件都是可实跑的最小程序，`.rejected.cx` 后缀 =
**必须编译失败**的反例）。

构建：`cmake --build build --target clux`（不要 clean）。
运行：`.\build\clux.exe run repro\own-compound\<name>.cx`（反斜杠或
`./repro/...` 均可；`clux run` 只把 `./ ../ / C:/` 形式的参数当文件路径）。

回归测试：`tests/driver_own_compound_test.cpp`（22 个用例）。

---

## 规则一：构造表达式产出 `fatal T`

`.T{...}` 构造的是**新堆块**（内嵌 own 字段），语义上就是"将亡值"，因此 T 含 own
资源时其类型是 **`fatal T`**，不是 T：

```rust
struct Box { p: own *i32; n: i32; }

var a: Box = .Box{ .p = new i32{5}, .n = 6 };   // fatal Box → own 变量接管（转移）
take(.Box{ .p = new i32{3}, .n = 4 });          // 直接喂 fatal Box 形参（不必再 move）
func mk(): fatal Box { ... }                     // 构造可作 fatal 返回值
```

修复前构造产普通 `Box`：`take(.Box{...})` 会被"含 own 资源不可按值传参"拒绝
（构造明明是新堆块），而 `var b: Box = a` 这种**真拷贝**反而放行。

**实现要点（踩过的坑）**：sema 与运行期类型必须一致。只改 sema 的 shadow 类型，
运行期值仍带非 fatal 类型 → 走到 `fatal T` 形参/返回位时 `type_fatal_carries_resource`
判定失败、退化成 clone，构造临时值仍持有内嵌堆块 → 作用域退出双释放
（实测 access violation）。因此 `ex_construct_fatal` 同时把构造节点的**类型槽位**
改写成 fatal 变体，让编译器发 `LOAD_TYPE <fatal 变体 id>`（布局与 base 逐字节
相同，构造逻辑无感）。

**cunion 是刻意的例外**：其 dispose 是 no-op（无 tag、开发者自负），标成 fatal
只会让所有权规则与销毁语义脱节。

## 规则二：函数参数含 own 字段只能写 `fatal T`

```rust
func bad(b: Box): i32;            // ❌ 按值传 = 与调用方共享内嵌堆块 → 双释放
func good(b: fatal Box): i32;     // ✅ 调用点 move/构造传入，函数内 move 接管
func read(b: ref *Box): i32;      // ✅ 只想借用
```

扩散是**递归**的：`struct Outer { inner: Inner; }`（Inner 含 own）作参数同样只能
`fatal Outer`。

## 规则三：变量声明/赋值禁止 copy

```rust
var a: Box = .Box{...};
var b: Box = a;                   // ❌ copy → 两个值共享内嵌堆块
var b: Box = move(a);             // ✅ 转移（a 进 TDZ）
var b: Box = clone(a);            // ✅ 深拷贝（a 仍有效）
```

含 own 的平凡复合类型（如 `?Box`、`[2]Box`、`<Box, i32>`）同样受限。

## 规则四：参数位含 ref 字段必须标注作用域

与变量声明（`check_var_annot`）、返回值（`sema_check_ret_ref_annot`）**同一条**
§5 规则——聚合内 ref 字段的借源无法静态推断，不标注就是指针逃逸：

```rust
func f(h: '<p> Holder, p: ref *i32): i32;   // ✅
func f(h: Holder): i32;                      // ❌ 报错
```

**顶层 `ref *T` / `ref []T` 不在此列**：那是"借用"本身的声明形式（变量声明侧有
同样的 `type_is_ref` 例外），强制标注只会让每个借用参数都要写 `'<*>` 之类噪声。

## 规则五（细节）：`pop_scope` 只释放 own，fatal 永不由销毁路径释放

`ptr_free_owned_recursive` 的 PTR 分支**只认 `OWN_OWN`**。fatal 的归宿只有两个
——被 own 变量/参数/返回值接管（消费），或被 `_ = ` 显式丢弃；两种情况下堆块都
已归接管方或由 DISPOSE 释放，因此销毁路径碰它就是 double-free。若真遇到
`ptr != NULL` 的 fatal（sema 消费检查被绕过），宁可泄漏也不误释放。

fatal 复合类型与其 base **逐字节同布局**，所以递归销毁按字段偏移分派时与
ownership 无关 → 两者走同一条路径，无需特判。

---

## 用例表

| 文件 | 覆盖 | 期望 |
|---|---|---|
| `probe0_union.cx` | union 含 own payload：构造/接管/move/clone/fatal 传参/显式丢弃 | exit 0 |
| `probe0b_union_param.rejected.cx` | **反例**：union 含 own payload 时按值传参 | exit 1 |
| `probe1_nested_field.cx` | 内层构造（fatal Inner）作外层字段值 → 转移 | exit 0 |
| `probe2_ownptr_copy.rejected.cx` | **反例**：`own *i32` 直接 copy（既有规则对照） | exit 1 |
| `probe2b_struct_copy.rejected.cx` | **反例**：含 own 字段的结构体 copy | exit 1 |
| `probe3_plain_param.rejected.cx` | **反例**：普通 `Box` 形参（应改 `fatal Box`） | exit 1 |
| `probe4_param_ref.rejected.cx` | **反例**：参数含 ref 字段但未标注作用域 | exit 1 |
| `probe5_bare_discard.rejected.cx` | **反例**：fatal 裸表达式语句（应写 `_ = `） | exit 1 |
| `probe6_explicit_discard.cx` | `_ = ` 显式丢弃 fatal 复合值 → 递归释放内嵌 own | exit 0 |
| `ffi_own_ok.cx` | 合法 FFI：`ref *T` / `fatal *T` / 平凡 struct 真调用 C 符号 | exit 0 |
| `ffi_own_signature.rejected.cx` | **反例**：`extern cfunc/var` 签名含 own（顶层指针 / struct 参数 / struct 返回 / extern var） | exit 1 |

## FFI 签名约束：含 own 资源即非法（"签名即约束"）

C 侧没有所有权概念，所以含 own 资源的类型不能出现在 C ABI 签名里——不只是顶层
`own *T`（m6-design §2.3 早已明写要拒，实现漏了），**递归含 own 字段的复合类型
同样非法**：

```rust
extern cfunc libc::f(p: own *i32): i32;   // ❌
extern cfunc libc::g(b: Box): i32;        // ❌ Box 含 own *i32
extern cfunc libc::h(): Box;              // ❌
extern var   libc::v: own *i32;           // ❌
```

理由：C 只看到裸指针字段，会拿到一个仍由 clux 拥有的堆块（谁来 free？）。检查放在
**声明处**（`ffi_c_abi_reject_reason`，per-kind 分派之前），判据与语言侧共用
`type_owns_resources` —— FFI 边界不比语言内部更宽松。share/weak 同为 RC 资源一并
拒绝；`ref *T` / `fatal *T` 不持有资源，仍按指针宽度放行。

回归：`tests/driver_ffi_own_test.cpp`（7 例）。

### union 的 double-free（本目录发现并修复）

`op_construct` 的 **union 分支**原先 memcpy payload 后**没有清空源**（struct 分支
有 `ptr_clear_owned_recursive`）。于是 payload 临时值仍持有内嵌堆块：

1. 作用域退出时 payload 临时值先释放该堆块（`own *i32` 的销毁路径）；
2. union 成员随后再释放**同一个**堆块 → double-free（access violation）。

修法与 struct 分支对齐：memcpy 后对 `casted`（异类型转换产物）与 `payload`
（原值）都做递归清空——两者都是本次构造的**临时值**（表达式求值产物），清空它们
不伤任何具名变量；同类型短路时两者是同一对象，第二次调用幂等空转。

回归用例见 `tests/driver_own_compound_test.cpp` 的 `UnionWithOwnPayload*` 四例。

## 与其他目录的关系

- `repro/fatal-compound/`：`fatal T` 作为**跨函数边界载体**的机制（move 转移、
  clone 深拷贝、fatal 形参与返回值、数组 fatal 变体）。
- 本目录：**所有权扩散**的规则面（哪些位置禁止 copy、参数只允许 fatal、ref 标注、
  销毁边界）。
