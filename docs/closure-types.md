# 闭包类型设计（closure types）

状态：设计定稿（实现分阶段进行；当前代码仍是"类型擦除 + 堆 env"的旧表示）。

## 1. 核心决策

闭包**不擦除**：捕获列表是类型的一部分。

```clux
func |a: i32, b: own *i32| (i32, i32) -> i32
```

- `func(...) -> R`：**无捕获**函数类型（旧的普通函数值）。
- `func |a: T, b: U| (params) -> R`：闭包类型，捕获槽位类型（按声明序）进入
  类型身份；捕获**名**不参与类型相等（布局/可赋值性只看类型序列）。
- 需要"可调用对象"作为参数时用泛型，而不是擦除：

```clux
func apply[F](f: F, x: i32): i32 { return f(x); }

var base: i32 = 3;
var f: func |base: i32| (i32) -> i32 =
    func |base| (x: i32): i32 { return base + x; };

if (apply[func |base: i32| (i32) -> i32](f, 4) != 7) { return 1; }
```

泛型单态化后 `F` 是具体闭包类型，`f(x)` 按该签名直接检查/调用；没有
`void*` 擦除，也没有函数指针 + 类型转换。

## 2. 值表示：捕获内联 + clone/dispose

闭包值是一个聚合结构，捕获块**内联**在值里（不再单独 `malloc` env）：

```c
/* C 侧（每个闭包类型一个 struct；<id> 为类型 id） */
struct clux_<tag>_cl_<id> {
    <ret> (*fn)(void *env, <params...>);
    struct {                       /* 内联捕获块，声明序 */
        <cap0_type> clux_f_cap0;
        <cap1_type> clux_f_cap1;
        ...
    } env;
};
```

- 调用：`f(args)` → `f.fn(&f.env, args)`（实现函数首参仍是 `void *env`，
  便于与普通函数共用 ABI）。
- 函数体内捕获访问：`((struct env *)env)->clux_f_cap`（**指针**语义，写回
  闭包实例）。
- `env_clone` / `env_dispose` **不作为函数指针存进闭包值**：它们是按闭包
  类型**静态生成**的普通函数（每个 env 布局一个），调用点已知具体闭包类型，
  直接调用它们（`clux_env_clone_<id>(&src.env)` / `clux_env_dispose_<id>(
  &value.env)`），零额外指针、零间接开销。
- 捕获里可能有 `own` / `fatal` / 含 own 的聚合：拷贝闭包必须深拷贝、销毁
  必须释放。捕获块内联只是把"env 的存储"放进闭包值，生命周期规则不变。
- `move`：按位移动，源失效（不再 dispose）；`clone`：`fn` 不变，
  `env = env_clone(&src.env)`；`dispose`：`env_dispose(&value.env)`。
- 无捕获函数值：`struct { fn; }`（`env` 为空，调用传 `NULL`）。

## 3. 所有权规则

- 闭包类型是否"含 own"由捕获槽位递归决定（`type_has_own_slots` 扩展覆盖
  捕获列表）：
  - 含 own → 闭包值 move-only；`clone` 仅在 `clone` 语境/显式克隆时可用；
  - 不含 own → 可自由 copy（但 `env_clone` 仍生成，统一代码路径）。
- `FREE closure` → `env_dispose(&value.env)`（不再有 `clux_func_release`
  的单独 env 指针）。
- 不允许把带捕获闭包赋给 `func(...)`（擦除）。过渡期可保留"加宽"并告警，
  最终删除；迁移方式：泛型参数或显式闭包类型注解。

## 4. 各层改动清单

1. **vm/type_func**：`func_type_t` 增加 `capture_types[]`；`type_func_closure`
   按 (签名, 捕获类型序列) intern；类型名 `func |T, U| (...)`；访问器
   `func_type_is_closure / func_type_capture_count / func_type_capture_type`。
2. **parser/AST**：`ast_func_type_t` 增加捕获列表（`AST_VAR_DEF` 链，每项
   `name: Type`，类型上下文禁止初始值）；`func |a: T| (...) -> R`。
3. **sema**：
   - 闭包字面量/局部函数的类型 = 捕获槽位类型构成的闭包类型（不再返回普通
     `func(...)`）；`capture_type` 结构体的字段顺序即声明序；
   - 泛型 mangle 纳入捕获类型，避免 `apply[闭包A]` / `apply[闭包B]` 合并；
   - 兼容性：闭包类型 ≠ 普通 func；`apply[F]` 调用检查在实例化后自然成立。
4. **compiler**：新增 `FUNC_TYPE_CAPTURE` 字节码（hoist 时在参数/返回后追加
   捕获类型）；`MAKE_FUNCTION` 把捕获内联进新值表示；`SET_CLOSURE` 改写为
   对值内 env 字段赋值（或由新的 CLOSURE 构造序列完成）。
5. **VM 值表示**：闭包类型的 value data = `{ fnptr; caps... }`（类型 size
   不再是 `sizeof(func_t*)`，而是按捕获布局计算）；`func_vcall` 按类型分派，
   从内联块取 env 指针；`func_clone/dispose/assign` 调 `env_clone/env_dispose`
   （函数指针由编译期生成，同 C 后端）。
6. **C 后端**：
   - 闭包类型 → 上述 C struct（字段顺序固定）；调用 `f.fn(&f.env, ...)`；
   - 闭包字面量构造：`{ .fn = impl, .env = { .clux_f_a = a, ... },
     .env_dispose = _, .env_clone = _ }`，不再 `clux_new`；
   - 函数体：`struct env *clux_v___ctx_env = (struct env *)env;`，捕获访问
     走 `->`；
   - `FREE` / 赋值 / 返回按内联聚合处理：own 字段释放、clone 调 `env_clone`；
   - 删除 `clux_func_release` 中"释放 env 指针"的路径。
7. **测试/示例迁移**：
   - 新增：闭包类型注解、`apply[F]`、含 `own` 捕获的 clone/move/dispose、
     C 与 VM 差分（env 内联后无泄漏、无双释放）；
   - 现有 300+ 处 `func(...)` 承载捕获闭包改为泛型/闭包类型；过渡期可先
     允许加宽（deprecated），最终删除。

## 5. 分阶段落地（每步保持套件绿色）

1. 类型系统 + 语法 + sema 解析闭包类型（只允许显式注解，字面量仍可推断为
   普通 func）；
2. C 后端内联 env + clone/dispose（先不改类型层面擦除），验证含 own 捕获；
3. VM 内联值表示 + compiler 捕获类型 hoist；
4. 闭包字面量类型 = 闭包类型，打开 no-erasure 检查；
5. 迁移 tests/examples，删除加宽；
6. 文档/ABI 收尾（导出 C ABI 的闭包约定、跨模块闭包类型）。

## 6. 验收用例

```clux
func apply[F](f: F, x: i32): i32 { return f(x); }

func main(): i32 {
    var base: i32 = 3;
    var f: func |base: i32| (i32) -> i32 =
        func |base| (x: i32): i32 { return base + x; };
    if (apply[func |base: i32| (i32) -> i32](f, 4) != 7) { return 1; }

    var p: own *i32 = new i32{ 5 };
    var g: func |p: own *i32| (i32) -> i32 =
        func |p| (x: i32): i32 { return p.* + x; };
    if (apply[func |p: own *i32| (i32) -> i32](g, 4) != 9) { return 2; }
    return 0;
}
```

C 与 VM 都必须：无泄漏、无环境堆分配（C 代码中无 `clux_new` 闭包 env）、
`own` 捕获在作用域结束时释放一次。
