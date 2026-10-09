# clux M4 设计文档：切片与字符串

## 目标

引入**切片**（动态长度胖指针）与**运行时字符串**，补全 C 表达力中缺失的动态数据视图。

- **切片 = 胖指针**：`{ ptr: *T, len: u64 }`，编译器视角是包含指针的结构体
- **复用 M3 所有权体系**：切片的所有权修饰放外部（`own []T` / `ref []T` / `fatal []T`），不引入新所有权机制
- **str 与 []u8 分离**：str 是编译期不可变常量，`[]u8` 是运行时可变字节序列

所有语法决策由用户 2026-10-09 逐项确认。

---

## 1. 切片类型

### 1.1 语法

```
own []T     // 拥有堆分配的切片（作用域退出自动释放）
ref []T     // 借用切片（不拥有，指向他人内存）
fatal []T   // 将亡值切片（move/clone 产物，禁止命名）
```

- **裸 `[]T` 不合法**——与指针 `*T` 一样必须带所有权修饰
- **不存在 `share []T` / `weak []T`**——切片不参与 RC 引用计数系
- **`[]` 是前导类型构造符**，与 `[N]T`（静态数组）同族但不同类型

### 1.2 胖指针布局

```
struct slice_t {
    void  *ptr;   // 指向元素序列首地址
    uint64 len;   // 元素数量
};
```

- `sizeof(slice) = 2 * sizeof(void*)`（16 字节 / 64 位平台）
- `alignof(slice) = alignof(void*)`
- C 后端可直接生成此结构，sizeof 反映真实内存布局
- 与 M3 的 `ptr_value_t { void *ptr; }` 同构——切片是"指针 + 长度"的扩展

### 1.3 const 修饰

```
const []T        // 切片本身不可重绑定（ptr/len 不可变，s = s2 非法）
[] const T       // 元素不可写（s[i] = v 非法）
const [] const T // 两者组合
```

- `const []T` 与 `[] const T` 是两个独立的复合类型，语义不同
- 与 M2 的 const 修饰规则一致（const 是前导类型构造符，持 base 指针）

---

## 2. 创建：make 宏函数

### 2.1 语法

```
make(T, N, v1, v2, ...)     // N 个 T，显式初始化
make(T, N, <v, M>, ...)     // fill 值包（同数组 <v,N> 语法）
```

- **`make` 是宏函数**——编译期替换，不是用户可定义的 function
- **`T` 是类型参数**，不作为运行期值传递（类型不可运行期作为参数）
- **`N` 可为运行期值**——与数组边界 `[N]T`（N 须编译期常量）不同，切片长度可运行期确定
- **返回 `fatal []T`**——被 `own []T` 变量接收即创建 own 切片
- **必须给出所有元素的初始值**——clux 无默认零值，`make(T, N)` 不带初始值非法

### 2.2 元素初始化

- 显式初始化：`make(i32, 4, 1, 2, 3, 4)` → 4 个 i32
- fill 值包：`make(i32, 1000, <0, 1000>)` → 1000 个 i32 全 0
- 混用：`make(i32, 10, <1, 4>, 5, 6, <7, 4>)` → 4+1+1+4=10
- **元素数必须 == N**——不等 → 编译错误（与数组 construct 完全显式规则一致）
- **无初始值参数非法**——`make(T, N)` 不带初始值 → 编译错误（clux 无默认零值）
- fill 值包语法 `<v, M>` 复用 M2 §12.3 数组批量初始化规则

### 2.3 示例

```
var v: own []i32 = make(i32, 3, 10, 20, 30);     // [10, 20, 30]
var z: own []i32 = make(i32, 100, <0, 100>);      // 100 个 0
var m: own []i32 = make(i32, 10, <1, 4>, 5, 6, <7, 4>);  // 4+1+1+4=10
var n: i32 = read_input();
var d: own []i32 = make(i32, n, <0, n>);          // 运行期长度，全 0
```

---

## 3. 所有权语义

### 3.1 own []T（唯一所有权）

- **绑定作用域**：`own []T` 声明所在的作用域退出时自动释放堆内存（同 `own *T`）
- **递归销毁**：`own []T` 且 T 含 own 字段时，逐元素递归销毁
- **禁止 copy**：`own []T` 之间赋值是 move 语义，必须显式 `move()` / `clone()`（同 `own *T`）
- **禁止全局 own []T**（同 §10 禁止全局 own）

### 3.2 ref []T（借用引用）

- **从 own 隐式产生**：`var s: own []i32 = make(i32, 4); var r: ref []i32 = s;`
- **从切片语法产生**：`s[a:b]` 恒产 `ref []T`（见 §4）
- **运行期零成本**：ref 切片就是胖指针值，逃逸检查全编译期
- **copy 允许**：`var r2: ref []i32 = r;` 拷贝 ptr+len

### 3.3 fatal []T（将亡值）

- `move(own_slice)` / `clone(own_slice)` → `fatal []T`
- 禁止命名：`var p: fatal []i32 = ...;` 非法
- 禁止丢弃：表达式结束未被 own 接收 / 函数 fatal 参数接收 → 编译错误
- 生命周期 = 一整个表达式

### 3.4 所有权流转与 M3 一致

| 操作 | 输入 | 输出 | 语义 |
|------|------|------|------|
| move | `own []T` | `fatal []T` | 转移所有权，源置空 |
| clone | `own []T` | `fatal []T` | 深拷贝（分配新堆块 + 逐元素 clone） |
| 隐式借用 | `own []T` | `ref []T` | 不递减所有权，复制 ptr+len |
| 切片 | `own []T` / `ref []T` / `fatal []T` | `ref []T` | 借用子区间 |

---

## 4. 切片操作：`[a:b]`

### 4.1 语法

```
s[a:b]       // 子切片，索引 [a, b)
s[:b]        // 前缀 [0, b)
s[a:]        // 后缀 [a, len)
s[:]         // 全量 [0, len)——也用于数组转切片
```

- **恒产 `ref []T`**——无论操作数是 `own []T` / `ref []T` / `fatal []T` / `[N]T` / `str`，切片语法结果一定是 `ref`
- **适用所有可索引对象**：静态数组 `[N]T`、切片 `own []T` / `ref []T` / `fatal []T`、`str`
- **a/b 可为运行期表达式**，须为整数类型
- **运行期边界检查**：`0 <= a <= b <= len`，越界 → 运行期错误

### 4.2 数组转切片

```
var arr = .[3]i32{ 1, 2, 3 };
var s: ref []i32 = arr[:];     // 显式转换，不自动
var mid: ref []i32 = arr[1:3]; // [2, 3]
```

- `[N]T` → `ref []T` **不自动**——必须用 `arr[:]` 或 `arr[a:b]` 显式创建

### 4.3 str 切片

```
var s: str = "hello world";
var prefix: ref [] const u8 = s[0:5];   // "hello"，元素不可写
var whole: ref [] const u8 = s[:];      // "hello world"
```

- `str[a:b]` → `ref [] const u8`——元素不可写（str 是编译期不可变常量）
- `str[:]` → `ref [] const u8`

---

## 5. 长度：len 内建函数

```
len(s) -> u64
```

- **内建函数**，读胖指针的 len 字段
- 适用：`own []T` / `ref []T` / `fatal []T` / `[N]T`（静态数组返回编译期常量 N）
- `[N]T` 的 `len` 可在编译期折叠为常量 N

---

## 6. 下标访问

```
s[i]       // 读：INDEX_GET
s[i] = v   // 写：INDEX_SET
s[i] += v  // 复合赋值
```

- 运行期越界检查：`0 <= i < len`，越界 → 运行期错误（同静态数组）
- 索引须为整数类型
- `[] const T` 的 `s[i] = v` → 编译错误（元素只读）

---

## 7. 切片比较

```
s1 == s2   // ptr 相同 && len 相同
s1 != s2   // !(s1 == s2)
```

- **指针相同 + 长度相同**才相等——不是逐元素比较
- 仅 `ref []T` 之间 / `ref []T` 与 `own []T` 可比较（同 base type）
- 不支持 `<` / `>` 等排序比较

---

## 8. 字符串：str vs []u8

### 8.1 str（编译期字符串常量）

- **不变**：str 是 intern 池内的不可变 C 字符串（M1 已实现）
- data 布局 = 池内 `const char *` 指针
- 字面量 `"hello"` → intern 池
- eq/ne 按内容比较（池去重，指针相等即同内容）

### 8.2 []u8（运行时字符串）

- 运行时字符串用 `own []u8`（拥有）或 `ref []u8`（借用）
- 与 `str` 是**不同类型**，不自动互转
- 通过 `str[a:b]` 或 `str[:]` 可得到 `ref [] const u8`（只读字节切片）

### 8.3 str → ref [] const u8 转换

```
var s: str = "hello";
var bytes: ref [] const u8 = s[:];       // 只读字节视图
var sub: ref [] const u8 = s[1:4];       // "ell"
```

- 唯一转换通道：切片语法 `str[a:b]` / `str[:]`
- 结果是 `ref [] const u8`（元素不可写，因 str 不可变）
- 不存在 `[]u8 → str` 的隐式转换（运行时字节序列不可变回编译期常量）

---

## 9. 与 M3 所有权体系的交互

切片与指针对齐——M3 所有权体系的所有规则适用于切片，切片是指针的扩展（多一个 len 字段）。

### 9.1 作用域标注

切片作为 `ref []T` 同样受 §5 作用域标注约束：

```
func f(a: ref []i32): '<a> ref []i32 {
    return a[0:3];   // 返回值标注 '<a>，借用来源是参数 a
}
```

### 9.2 函数边界

- 参数/返回值只能 `ref []T` 或 `fatal []T`，不能 `own []T`（同 §4 函数边界规则）
- `fatal []T` 参数：唯一允许 fatal 有名的场景，函数内必须消费

### 9.3 闭包捕获

- `own []T` 须 move/clone 捕获（同 §9 闭包捕获指针规则）
- `ref []T` 须闭包标注

### 9.4 全局规则

- 禁止全局 `own []T`（同 §10）
- 全局 `ref []T` 合法（指向静态数据）

### 9.5 嵌套切片

`[][]T` 合法——切片的元素类型可以是切片。与 `* *T`（嵌套指针）对齐：

```
var grid: own [][]i32 = make([]i32, 2, make(i32, 3, 1, 2, 3), make(i32, 3, 4, 5, 6));
var row: ref []i32 = grid[0];   // [1, 2, 3]
var cell: i32 = grid[1][2];     // 6
```

- 嵌套切片的销毁递归传导：`own [][]T` 销毁时逐元素递归销毁内层 `own []T`
- `ref [][]T` 借用，不销毁

### 9.6 切片作为 struct 字段

切片可作为 struct/tuple/union/cunion 的字段类型，所有权传导与指针字段一致：

```
type Buffer: struct {
    data: own []u8,
    cap:  u64,
};

type View: struct {
    data: ref []u8,
    pos:  u64,
};
```

- `own []T` 字段：struct 销毁时递归释放切片堆块
- `ref []T` 字段：借用，不释放（§5 作用域标注约束适用）
- `type_contains_own` 递归检查覆盖 `own []T`（同 §10 禁止全局）

---

## 10. 类型系统

### 10.1 新增 type kind

与指针对齐——三种所有权修饰是三个独立 type kind：

```
TYPE_KIND_SLICE_OWN    // own []T：拥有堆分配，作用域退出自动释放
TYPE_KIND_SLICE_REF    // ref []T：借用，不拥有
TYPE_KIND_SLICE_FATAL  // fatal []T：将亡值，move/clone 产物
```

- 不存在 `TYPE_KIND_SLICE_SHARE` / `TYPE_KIND_SLICE_WEAK`——切片不参与 RC 系
- `slice_type_t` 继承 `type_t`，持 `elem_type` 指针 + 所有权 kind
- 两遍构造协议：`type_slice_push` / `type_slice_set_elem` / `type_slice_seal`（同 array/ptr）
- intern 池：`vm->slice_types`，按 (elem_type, kind) 去重
- 运行期零标志——所有权由 type kind 区分（同 M3 §13.2 指针设计）

### 10.2 与数组的关系

| 属性 | `[N]T`（静态数组） | `[]T`（切片） |
|------|-------------------|--------------|
| 长度 | 编译期常量 N | 运行期值 len |
| 内存 | 内联（值类型，按值拷贝） | 堆分配 + 胖指针 |
| 所有权 | 无（数组本身不拥有，元素内联） | own 拥有堆块 / ref 借用 |
| 构造 | `.[N]T { ... }` | `make(T, N, ...)` |

### 10.3 const 修饰

- `const []T`：`const_type_t { base = slice_type_t{elem=T} }`
- `[] const T`：`slice_type_t { elem = const_type_t{base=T} }`
- 两者是不同的 intern 实例

---

## 11. 内建函数

### 11.1 make（宏函数）

```
make(T, N, init...) -> fatal []T
```

- 编译期替换：T 是类型表达式（不作为运行期参数）
- N 可运行期值
- 元素初始化规则同数组 construct（零值 / 显式 / fill 值包）
- 返回 `fatal []T`

### 11.2 len（内建函数）

```
len(s) -> u64
```

- 适用 `[]T`（所有所有权修饰）和 `[N]T`
- `[N]T` 的 len 编译期折叠为常量
- `[]T` 的 len 运行期读胖指针

### 11.3 与现有内建的关系

| 内建 | id | 说明 |
|------|----|------|
| printf | 0 | M1 |
| upgrade | 1 | M3 §12.4 |
| len | 2 | M4 |

`make` 不是内建函数，是宏函数——编译期替换，不进函数表。

---

## 12. 语法设计决策

### 12.1 `[]` 前导类型构造符

```
own []i32          // 切片类型
ref [] const u8    // 只读字节切片
[]i32              // 非法（裸切片，须带所有权修饰）
```

- `[]` 是前导类型构造符，与 `[N]` 同族
- parser 在 `parse_unary` 前缀 token 收集中加 `[]` 支持
- 所有权修饰（own/ref/fatal）在 `[]` 外部，与指针 `own *T` 一致

### 12.2 切片语法 `[a:b]`

- postfix 运算符，优先级与下标 `a[i]` 相同
- `s[a:b]`、`s[:b]`、`s[a:]`、`s[:]` 四种形式
- `a`/`b` 可省略（默认 0 / len）

### 12.3 make 语法

```
make ( type_expr , expr , init... ) -> fatal []T
```

- 第一个参数是类型表达式（不作为值求值，编译期替换时直接消费类型）
- 第二个参数是长度表达式（运行期值）
- 后续参数是元素初始化值（同数组 construct 语法）

---

## 13. 实现路线

### Phase 1: 类型系统
- `TYPE_KIND_SLICE` 枚举 + `slice_type_t` 结构
- vtable（clone/dispose/assign/eq/ne/get_index/set_index）
- intern 池 + 两遍构造协议
- const 修饰支持

### Phase 2: parser
- `[]` 前导类型构造符解析
- `[a:b]` postfix 切片运算符解析
- `make` 宏函数解析
- 所有权修饰 + `[]` 组合解析

### Phase 3: sema
- 切片类型解析 + 所有权规则
- `make` 宏函数编译期替换
- 切片语法 `[a:b]` 类型检查
- `len` 内建函数注册 + 类型检查
- 作用域标注 / 函数边界 / 闭包捕获 / 全局规则扩展

### Phase 4: compiler + VM
- 字节码指令：SLICE_GET / SLICE_SET（下标）/ SLICE_RANGE（`[a:b]`）
- MAKE_SLICE 指令（make 宏编译产物）
- LEN 指令
- 胖指针 value 布局 + clone/dispose/assign 语义
- 运行期越界检查

### Phase 5: 测试 + examples
- 切片创建/读写下标/子切片/数组转切片
- 所有权 move/clone/borrow
- str 切片 → ref [] const u8
- len 内建函数
- 端到端 examples

---

## 后续（非本里程碑）

- **字符串标准库**（拼接、查找、替换等操作）
- **UTF-8 字符串**（`[]u8` 是字节序列，字符级操作需标准库支持）
