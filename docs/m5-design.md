# clux M5 设计文档：模块系统

## 目标

引入**模块系统**，支持跨文件代码组织。设计参考 TypeScript，但更简化。

- **export** 修饰全局实体（var/func/type/struct/enum/union/cunion）使其对外可见
- **import** 导入整个命名空间，通过 `::` 访问成员
- **代理导出** `export * from "./path"` 转发依赖模块的导出符号
- **路径导入** 仅支持相对路径（`./` 或 `../` 开头），包导入（非路径）暂不实现
- **模块是运行期全局单例**，编译期收集依赖 + 拓扑排序，运行期首次 import 时初始化

---

## 1. 语法

### 1.1 export

```
export func printf(fmt: str, ...): void { ... }
export var id: i32 = 123;
export type Point: struct { x: i32; y: i32; }
export struct Vec { x: i32; y: i32; }
export enum Color { Red, Green, Blue }
```

- `export` 是前缀关键字，修饰全局定义语句
- 可修饰所有全局实体：`var` / `func` / `type` / `struct` / `enum` / `union` / `cunion`
- 非 export 的全局实体是模块私有的，仅模块内部可见
- `export` 不改变实体的语义，仅标记进入模块导出表

### 1.2 import

```
import std from "./std";
import utils from "../lib/utils";
```

- `import <name> from "<path>"` — 导入整个模块为命名空间 `<name>`
- `<name>` 是用户指定的别名，只要是合法标识符即可，不要求和文件名匹配
- `<path>` 必须以 `./` 或 `../` 开头（相对路径，相对于当前文件所在目录）
- 路径分隔符支持 `/` 和 `\`（Windows 反斜杠），编译器统一规范化为 `/`
- 非路径导入（如 `import std from "std"`）是包导入，**暂不实现**（编译期报错"package import not supported"）

### 1.3 代理导出

```
export * from "./std";
```

- `export * from "<path>"` — 将依赖模块的所有导出符号原样转发到当前模块的导出表
- 全量转发，不支持选择性代理（`export { printf } from "./std"`）
- 如果转发符号与当前模块已有导出符号同名 → **编译错误**（符号冲突）

### 1.4 命名空间访问 `::`

```
import std from "./std";

func main(): void {
    std::printf("%d\n", std::id);
    var p: std::Point = .Point { .x = 1, .y = 2 };
    var c: std::Color = std::Color::Red;
}
```

- `::` 是命名空间/作用域成员访问符，编译期解析
- **复用已有 `::` 运算符**（原用于 enum variant 访问 `Color::Red`），sema 层根据左值类型分派：
  - 左值是模块命名空间 → 解析为模块导出成员
  - 左值是枚举类型 → 解析为 enum variant
  - 否则 → 编译错误
- `::` 与 `.` 的区别：`::` 是**编译期命名空间解析**（不经值，直接查符号表）；`.` 是**运行期字段访问**（经值，按偏移读取）

### 1.5 import 位置

- `import` 语句只要求在**顶层作用域**（不能在函数体内）
- 不要求必须在文件最前面——import 前可以有其他全局语句，但语义上所有 import 在模块编译时优先处理

---

## 2. 路径解析

### 2.1 相对路径

- `./std` → 当前文件所在目录下的 `std.cx`
- `../lib/utils` → 上级目录 `lib/` 下的 `utils.cx`
- 路径相对于**被导入文件**的目录（类似 Node.js `__dirname`），不是工作目录
- 路径分隔符：`/` 和 `\` 均合法，编译器统一规范化为 `/`
- 文件扩展名 `.cx` 可省略（`./std` → `./std.cx`），也允许显式写 `.cx`

### 2.2 路径校验

- 必须以 `./` 或 `../` 开头，否则报错："import path must start with './' or '../'"
- 路径不存在（文件找不到）→ 编译错误："module not found: './std'"
- 路径规范化后去重：`./std` 和 `./../src/std` 如果指向同一文件，视为同一模块

### 2.3 动态库导入（FFI 预备，sema 报错）

```
import libdemo from "libdemo.so";   // Linux/macOS
import libdemo from "libdemo.dll";  // Windows
```

- 路径以 `.so` / `.dll` / `.dylib` 结尾的是**动态库导入**——为 M6 FFI 铺路
- 语义：加载动态库，后续通过 `libdemo::function_name` 获取函数句柄（类似 `dlsym`）
- M5 **parser 层支持此语法**（解析为 AST_IMPORT，路径标记为动态库）
- M5 **sema 层报错**："dynamic library import not supported in M5 (requires FFI)"
- M6 FFI 实现时：Linux/macOS 用 `dlopen`/`dlsym`，Windows 用 `LoadLibrary`/`GetProcAddress`

### 2.4 包导入（暂不实现）

```
import std from "std";  // 非路径 → 包导入
```

- 非路径导入（不以 `./` 或 `../` 开头，且不以 `.so`/`.dll`/`.dylib` 结尾）是包导入
- M5 **暂不实现**包导入，遇到时报错："package import not supported in M5"

### 2.5 导入形式分类

| 路径形式 | 类型 | M5 状态 |
|----------|------|---------|
| `./std` / `../lib/utils` | 模块导入（.cx 文件） | ✅ 实现 |
| `libdemo.so` / `libdemo.dll` / `libdemo.dylib` | 动态库导入（FFI） | ⚠️ parser 支持，sema 报错 |
| `std`（非路径、非动态库后缀） | 包导入 | ❌ 报错 |

---

## 3. 模块对象

### 3.1 运行期表示

- 模块是**全局单例**，随 VM 销毁销毁，程序生命周期内常驻
- VM 维护 `modules: map<module_id, module_value_t*>`，module_id 是编译期分配的唯一 u32
- 模块对象不是 struct——它是**命名空间**，成员是编译期符号表映射
- 未来 C 后端转译时，命名空间完全消减：`std::printf` → `std_printf`（符号重命名）

### 3.2 模块对象的类型

- 引入 `TYPE_KIND_MODULE` — 模块类型，是一种特殊的聚合类型
- 模块类型的成员 = 导出符号表（名字 → 类型）
- 与 struct 的区别：模块类型不可实例化（不能 `.<module>{...}` 构造），不可赋值，不可传递
- 模块类型仅用于 `import` 绑定的变量类型 + `::` 成员访问的类型检查

### 3.3 导出成员约束

- export var 受全局 own 禁止规则约束（§9.4）：不能 export `own *T` / `own []T`
- 可导出的 var 类型：标量 / str / ref / func / type value
- export func：函数 value，通过 `::` 访问后可调用
- export type/struct/enum/union/cunion：类型定义，通过 `::` 访问后可用于类型标注或构造

---

## 4. 编译流程

### 4.1 模块发现与拓扑排序

1. 编译器从入口文件开始（`clux run main.cx`）
2. 解析文件中的所有 `import` 语句，收集依赖
3. 对每个依赖递归解析（深度优先），发现新模块时递归编译
4. **循环依赖检测**：如果 A → B → A（经任意路径），编译错误 "circular dependency: A -> B -> A"
5. 拓扑排序：被依赖模块先编译、先初始化
6. 每个模块分配唯一 `module_id`（u32），按拓扑序递增分配

### 4.2 编译期符号收集

每个模块编译时：
1. 解析所有全局语句（var/func/type/struct/enum/...）
2. `export` 标记的符号进入模块**导出表**（export_table: map<name, symbol_info>）
3. `export * from "./path"` 时，递归编译被依赖模块，取其导出表，原样合并到当前模块导出表
4. 合并时如果同名 → 编译错误 "export conflict: symbol 'printf' already exported"
5. 非 export 符号是模块私有，不进入导出表

### 4.3 import 编译

`import std from "./std"` 编译为：
1. 编译期：解析路径 → 递归编译 `./std.cx` → 获取模块的 module_id + 导出表
2. 在当前模块的作用域中注册 `std` 为模块命名空间符号（绑定 module_id + 导出表）
3. `std::printf` 解析：查 `std` 的导出表 → 找到 `printf` → 解析为对应符号
4. 运行期字节码：`IMPORT <module_id>` 压入模块对象 → `GET_MEMBER <"printf">` 取成员

### 4.4 字节码指令

| 指令 | 操作数 | 语义 |
|------|--------|------|
| `IMPORT` | u32 module_id | 查 VM modules map → 未缓存则执行模块初始化 → 压模块对象 |
| `GET_MEMBER` | strtable_idx (成员名) | 弹模块对象 → 按名取导出成员 → 压成员值 |

- `IMPORT <id>`：运行期首次执行时，触发模块全局初始化（执行模块级 var 求值 + 注册函数等），构造模块对象存入 map，返回模块对象。后续相同 id 的 IMPORT 直接返回缓存。
- `GET_MEMBER <name>`：从模块对象中按名取成员（函数引用/变量值/类型值）
- `std::printf(...)` 编译为：`IMPORT <id>` → `GET_MEMBER "printf"` → `CALL <argc>`

### 4.5 代理导出编译

`export * from "./std"` 编译为：
1. 编译期：递归编译 `./std.cx`，获取其导出表
2. 将导出表中的所有符号合并到当前模块的导出表
3. 运行期：当前模块初始化时，执行 `IMPORT <std_id>` 确保依赖模块已初始化
4. 当前模块的 `GET_MEMBER` 请求转发到依赖模块（或编译期直接解析为依赖模块的成员）

---

## 5. 运行期行为

### 5.1 模块初始化

- 模块在**首次被 IMPORT 时**初始化（惰性）
- 初始化步骤：
  1. 执行模块所有全局 var 的初始化表达式（按声明顺序）
  2. 注册全局 func / type / struct / enum 定义
  3. 构造模块对象（聚合导出成员），存入 `vm->modules[module_id]`
- 初始化是**幂等的**的——后续 IMPORT 直接返回缓存

### 5.2 初始化顺序

- 按 import 拓扑序：被依赖模块先初始化
- A imports B, B imports C → 初始化顺序: C → B → A
- 模块内多个全局 var 按声明顺序初始化
- 入口模块最后初始化，其 `func main()` 被调用

### 5.3 模块全局变量

- 模块全局 var 是模块私有的（除非 export）
- export var 在模块对象中保存一份值副本（或借用引用）
- 模块全局 var 的生命周期 = 模块对象生命周期 = VM 生命周期

---

## 6. `::` 运算符语义

### 6.1 分派规则

`lhs::name` 的 sema 层分派：

| lhs 类型 | 行为 |
|----------|------|
| 模块命名空间 | 查模块导出表 → 解析为导出符号 |
| 枚举类型 | 查 enum variant → 解析为 variant 值 |
| 其他 | 编译错误："'::' requires a module namespace or enum type on left side" |

### 6.2 与 `.` 的区别

| 特性 | `::` | `.` |
|------|------|-----|
| 解析时机 | 编译期 | 运行期 |
| 左值 | 模块命名空间 / 枚举类型 | 值（struct/tuple/数组） |
| 成员来源 | 符号表（导出表/variant 表） | 值的内存布局（字段偏移） |
| 可链式 | `std::Color::Red`（先模块→再枚举） | `p.x.y`（先 struct→再 struct） |

### 6.3 链式访问

```
import std from "./std";
var c: std::Color = std::Color::Red;
```

- `std::Color` → std 模块的导出类型 Color（enum 类型）
- `std::Color::Red` → Color 枚举的 Red variant
- `::` 链式：模块 → 枚举 → variant

---

## 7. 循环依赖检测

### 7.1 规则

- **禁止循环依赖**——编译期报错
- 检测方式：编译模块时记录"正在编译"状态，如果递归编译依赖时遇到"正在编译"的模块 → 环路检测
- 错误信息包含完整依赖链："circular dependency: a.cx -> b.cx -> a.cx"

### 7.2 范围

- 仅检查 import 依赖图（`import` + `export * from`）
- 不检查函数调用依赖（函数可以互相调用，包括跨模块）

---

## 8. 入口约定

- `clux run <file>` 中 `<file>` 是入口模块
- 入口模块的 `func main()` 是程序入口
- 非入口模块**不需要** `func main()`
- 入口模块也可以有 `export` 和 `import`（它本身也是模块，只是恰好有 main）

---

## 9. 与现有系统的交互

### 9.1 与 M3 所有权体系

- 模块对象是全局单例，不受 move/clone 约束
- export var 受全局 own 禁止规则约束（§9.4）
- 模块对象不可 move/clone/assign（它是命名空间，不是值）

### 9.2 与 M4 切片

- 模块可以 export ref []T（借用切片，指向模块全局数据）
- 模块不能 export own []T（全局 own 禁止）

### 9.3 与 printf 内建

- 当前 printf 是硬编码内建函数（id=0），不在任何模块中
- M5 不改变 printf 的地位——它仍然全局可用，不需要 import
- 未来标准库可能提供 `std::printf`，但 M5 暂不涉及

---

## 10. 类型系统

### 10.1 TYPE_KIND_MODULE

```c
TYPE_KIND_MODULE,  // 模块类型（命名空间，不可实例化）
```

- 模块类型的 `size = 0`（不占值空间，IMPORT 指令压的是模块对象指针）
- 模块类型的成员表 = 导出符号表
- 模块类型不可用于：变量构造、赋值、move/clone、函数参数、返回值类型
- 模块类型仅用于：`import` 绑定 + `::` 成员访问

### 10.2 module_type_t

```c
typedef struct {
    const type_t base;        /* kind = TYPE_KIND_MODULE */
    strslice_t name;          /* 命名空间名（import 别名） */
    uint32_t module_id;       /* 编译期分配的唯一 id */
    /* 导出成员表：name -> type */
    struct {
        strslice_t name;
        const type_t *type;
    } *members;
    size_t member_count;
} module_type_t;
```

---

## 11. 字节码指令

### 11.1 新增指令

| 指令 | 操作数 | 栈效果 | 语义 |
|------|--------|--------|------|
| `IMPORT` | u32 module_id | → module_obj | 查 VM modules map → 未缓存则初始化 → 压模块对象 |
| `GET_MEMBER` | strtable_idx (name) | module_obj → member_value | 弹模块对象 → 按名取导出成员 → 压成员值 |

### 11.2 编译模式

```
import std from "./std";
std::printf("%d\n", std::id);
```

编译为：
```
IMPORT <std_module_id>       ; 压 std 模块对象
GET_MEMBER "printf"          ; 取 printf 函数
PUSH_STR "%d\n"              ; 压参数
IMPORT <std_module_id>       ; 压 std 模块对象（缓存命中，直接返回）
GET_MEMBER "id"              ; 取 id 值
CALL 2                       ; 调用 printf("%d\n", id)
```

### 11.3 优化

- 同一作用域内重复 IMPORT 同一模块 → 编译器可优化为一次 IMPORT + DUP（或 STORE + LOAD）
- M5 暂不做此优化，保持简单

---

## 12. 编译器架构

### 12.1 模块管理器

```c
typedef struct {
    /* 已编译模块缓存：规范路径 → module_info */
    strmap_t *compiled;
    /* 正在编译的模块栈（循环检测） */
    vec_t(cstr_slice_t) compiling;
    /* 下一个 module_id */
    uint32_t next_id;
} module_manager_t;
```

### 12.2 编译入口

1. 解析入口文件 → 收集 import → 递归编译依赖
2. 拓扑排序 → 按序编译每个模块的字节码
3. 每个模块的字节码独立：全局 var 初始化 + 函数定义
4. 入口模块的字节码最后，包含 `func main()` 的调用

### 12.3 模块字节码布局

每个模块编译为独立的字节码段：
- 段首：模块全局 var 初始化代码
- 段中：函数定义（PUSH_FUNCTION + BIND_FUNC）
- IMPORT 指令在首次执行时触发被依赖模块的初始化

---

## 13. 实现路线

### Phase 1: Parser + AST
- `export` / `import` 关键字
- `export * from` 语法
- AST_EXPORT / AST_IMPORT / AST_REEXPORT 节点
- `::` 运算符已有（enum），扩展为模块成员访问
- 动态库导入语法（`import x from "lib.so"`）——parser 支持，path 标记后缀类型

### Phase 2: 模块管理器 + 路径解析
- module_manager_t 实现
- 路径解析 + 规范化（`/` 和 `\` 统一）
- 循环依赖检测
- 拓扑排序

### Phase 3: Sema
- 模块符号表 + 导出表
- import 注册命名空间符号
- `::` 成员访问分派（模块 vs 枚举）
- 代理导出符号合并 + 冲突检测
- **动态库导入 sema 报错**（"dynamic library import not supported in M5"）
- 包导入 sema 报错（"package import not supported in M5"）

### Phase 4: Compiler + VM
- IMPORT / GET_MEMBER 字节码指令
- 模块对象运行期表示（modules map + module_value_t）
- 模块初始化（全局 var 求值 + 函数注册）
- TYPE_KIND_MODULE 类型系统

### Phase 5: 测试 + examples
- 基本 import/export 端到端
- 代理导出
- 循环依赖检测（编译错误）
- 路径解析（相对路径 + Windows/Linux 分隔符）
- `::` 链式访问（模块 → 枚举 → variant）
- 动态库导入 sema 报错（编译错误验证）

---

## 后续（非本里程碑）

- **包导入**（`import std from "std"`）：需要包管理器 + 标准库目录约定
- **选择性导入**（`import { printf } from "./std"`）：暂不实现
- **C 后端符号消减**：`std::printf` → `std_printf` 重命名（M7 C 后端实现）
- **模块缓存**（.cxb 预编译）：增量编译
