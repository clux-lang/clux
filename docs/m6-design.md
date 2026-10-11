# clux M6 FFI 系统设计

## 设计原则

clux 采用标准 C ABI，通过 `cfunc` / `extern cfunc` / `extern var` 实现与 C 代码互操作
（`extern lib "<路径>" as <别名>;` 声明动态库）。
运行期动态调用使用 libffi（git submodule 引入），无需手写 trampoline。

> **实现状态：✅ Phase 1–5 全部落地 + 聚合类型完备（M6 完成）** —— 前端（`extern lib` /
> `extern cfunc` / `extern func`（等价写法）/ `extern var` 语法 + fmt + 明确诊断）；
> 运行期 `dlopen/dlsym` 加载与符号解析（库表在 `module_t.ffi_libs`，**判据是关键字
> 而非后缀**；路径 `"*"` = **当前进程自身**，见 §14.18）；调用路径
> （`BCODE_EXTERN_CALL` + libffi 精确类型描述符，定参与 C 变参统一入口，
> 经 MASM `ml64` 端口，见 §14.12/§14.19.2）；
> **数据符号**（库导出的全局变量）读写（`BCODE_EXTERN_LOAD`/`EXTERN_STORE`，见 §14.17）；
> §2 C ABI 类型约束（`ffi_c_abi_reject_reason`，接入 sema 与 `cfunc_ptr`）；
> **回调（Phase 5）**：宿主侧 `clux_ffi_closure_create`（§14.13–§14.15）与
> **语言侧 `cfunc_ptr(f)`**（§14.16）均已打通，clux 函数可被 C（含 libc `qsort`）
> 当回调调用。
>
> **C 类型覆盖（§14.20）**：标量 `i8..u64`/`f32`/`f64`/`bool`、指针族、`opaque`、
> **`str`（char\*）**、**`enum`**、**`struct`/`cunion` 按值**（传参、返回、变参位置、
> 回调双向）全部支持；**`usize` = C 的 `size_t`**（别名：64 位平台即 `u64`，
> 32 位平台即 `u32`），FFI 里声明 C 的 `size_t` 形参就用它；**程序自定义类型可出现
> 在 FFI 签名里**（含 `type` 别名与指向它们的指针，修掉了 pass 顺序缺陷）。
> `struct` 的描述符按字段递归构造，`cunion` 等价建模为"size/align 个整数成员"
> （libffi 无 union 类型）。
> 未做（有意留有边界）：FFI 实参位置的**隐式**转换（整数族/指针族以外要求类型可接受，
> 见 §14.18.2）；`option`/tag union/切片（胖指针）/元组/闭包/`[N]T` 裸数组无 C 对应，
> 一律在编译期拒绝。
>
> **前置依赖（M5 已就绪）**：M5 已实现模块的相对路径解析与代理导出，并**明确拒绝**
> 非相对路径（`import path must be relative (...) package and dynamic-library
> imports are not supported in M5`）。
>
> ⚠️ **M6 需要重新决定动态库的绑定语法**：本文档 §1.1 原以**文件名后缀**
> （`.so` / `.dll` / `.dylib`）判定"这是动态库"。**后缀不表示文件类型**——它只是
> 名字，同名文件也可能是普通数据或另一个源码模块，靠后缀推断会给出误导性诊断、
> 并把错误假设固化进语言。因此 M6 应改为**显式语法**表达绑定意图（例如专用的
> 导入形式或 `extern cfunc` 声明所在模块的显式标注），后缀最多作为提示而**不作为判据**。

---

## 1. 语法

### 1.1 动态库导入

```
import libc from "libc.so.6";       /* Linux */
import libc from "libc.dll";         /* Windows */
import libc from "libobjc.dylib";   /* macOS */
```

- **语法定稿（2026-10，Phase 2 已落地）**：动态库由**关键字**声明，判据是
  `extern lib`，**绝不按后缀判类**（后缀不表示文件类型）：

  ```
  extern lib "libc.so.6" as libc;     /* 路径只是 dlopen 参数 */
  extern lib "user32.dll" as user32;
  ```

  - `import`（AST_IMPORT）语义**保持不变**：只表示源码模块 ✓
  - 上面的 `import libc from "libc.so.6";` 形式**已废弃**（它会让 import 一条
    语句承担两种语义，且诱导按后缀推断）
  - 运行期 `dlopen`/`LoadLibrary` 的句柄存 `module_t->dl_handle`（Phase 1 待接入
    libffi；third_party 目前无 libffi，需要 submodule 或最小 ABI 层）
  - 前端已实现：lexer 关键字 `extern`/`lib`/`cfunc`、AST_EXTERN_LIB、parser、
    fmt（`clux format` 幂等且不丢声明）、sema 对未实现的运行期给出**明确诊断**
    （`extern lib 'x' is not implemented yet (M6 FFI: dlopen/dlsym path pending)`）
- M5 已拒绝一切非相对路径（统一诊断）；M6 实现 `dlopen`/`dlsym`
  （或 `LoadLibrary`/`GetProcAddress`）时接续该位置。
- 运行期 `dlopen` 加载动态库，句柄存于 `module_t->dl_handle`

### 1.2 extern cfunc 声明（无函数体，dlsym 查找）

```
import libc from "libc.so.6";

extern cfunc libc::malloc(size: u64): fatal opaque;
extern cfunc libc::free(ptr: fatal opaque): void;
extern cfunc libc::printf(format: str, ...): i32;
```

- 只能声明在**顶层**（和 `func`/`var`/`type` 同级）
- 必须通过 `库名::函数名` 绑定到已导入的动态库
- `...` 表示 C 可变参数，**只在 `extern cfunc` 中允许**
- 调用时必须带库名前缀：`libc::printf("hello\n")`

### 1.3 cfunc 定义（有函数体，C ABI）

```
cfunc callback(data: ref *u8, len: u64): i32 {
  return 0;
}
```

- 用 clux 语法写函数体，走 VM 执行，但符号/调用约定是 C ABI
- 可作为函数指针传递给 C 接口
- 可被动态库导出（C 侧通过 `dlsym` 调用）
- 禁止闭包（无捕获列表）

---

## 2. cfunc 约束

### 2.1 允许的类型（C 兼容）

| clux 类型 | 说明 |
|-----------|------|
| i32 / i64 / u32 / u64 | 整数 |
| f32 / f64 | 浮点 |
| bool | 布尔 |
| str | 字符串（映射 `const char*`） |
| opaque | 不透明指针（映射 `void*`） |
| enum | 枚举（按底层宽度映射） |
| struct | 结构体（按字段布局映射） |
| cunion | C 语义 union |

### 2.2 禁止的类型

| clux 类型 | 原因 |
|-----------|------|
| option | clux 特有，无 C 对应 |
| tag union | clux 特有，带 tag |
| slice | 胖指针，非 C ABI |
| 闭包类型 | C 无闭包概念 |

### 2.3 指针约束

- 只允许 `ref *T` 和 `fatal *T`
- **禁止 `own *T`**——C 侧无所有权概念
- `ref *T`：借用指针，C 侧不释放，clux 侧保留所有权
- `fatal *T`：raw 指针，不追踪所有权，C 侧自由操作

### 2.4 其他约束

- 禁止闭包（无捕获列表）
- `...` 可变参数只在 `extern cfunc` 声明中允许，`cfunc` 定义不允许

---

## 3. 数据结构

### 3.1 module_t 新增字段

```c
typedef struct module_t {
    bytecode_t *bc;            /* 模块字节码（动态库模块为 NULL） */
    scope_t    *global_scope;  /* 模块全局作用域 */
    strmap_t   *exports;       /* name → value_t* 借用映射 */
    char       *canonical;     /* 规范路径（key，owned） */
    void       *dl_handle;     /* M6: dlopen 句柄，普通模块为 NULL */
} module_t;
```

- 普通模块：`dl_handle = NULL`，`bc` 非空
- 动态库模块：`dl_handle` = `dlopen` 返回值，`bc = NULL`，`global_scope` 为空 scope，`exports` 为空 strmap
- `module_destroy` 中 `dlclose` 非 NULL 的 handle

### 3.2 ffi_func_t（继承 func_t）

```c
typedef struct {
    func_t      base;          /* 继承：cfunc 指向 ffi_trampoline */
    ffi_cif     *cif;          /* libffi 调用接口（按签名构造） */
    void        *fnptr;        /* C 函数指针：
                                  - extern cfunc: dlsym 得到
                                  - cfunc 定义: ffi_closure 生成 */
    ffi_closure *closure;      /* libffi closure（cfunc 定义时，C 侧回调入口） */
    void        *dl_handle;    /* 借用 module_t->dl_handle（extern cfunc 查找来源） */
    bool         is_variadic;  /* extern cfunc 的 ... 标记 */
} ffi_func_t;
```

- **extern cfunc**：`fnptr` = `dlsym(handle, name)`，`closure` = NULL
- **cfunc 定义**：`fnptr` = `ffi_closure` 生成的函数指针，`closure` 非 NULL

---

## 4. 字节码指令

### 4.1 BCODE_EXTERN_FUNC（STR 操作数：函数名）

运行期首次执行时：
1. 从当前上下文获取 `module_t->dl_handle`
2. `dlsym(handle, func_name)` → 得到 `void*` 函数指针
3. 按签名类型构造 `ffi_cif`（可变参数用 `ffi_prep_cif_var`）
4. 构造 `ffi_func_t`，`base.cfunc` 指向 ffi trampoline
5. 压栈 func value

后续调用直接复用缓存的 `ffi_func_t`。

### 4.2 cfunc 定义的编译

- 编译器照常生成字节码函数体（和普通 `func` 一样）
- 额外标记 `is_cfunc = true`
- 函数对象构造时（PUSH_FUNCTION 阶段）创建 `ffi_closure`：
  - C 函数指针 = closure 生成的入口
  - C 侧通过该指针调用 → libffi 回调 → trampoline → `bcode_call_cfunc`
- closure 创建时机：函数对象构造时立即创建（C 侧随时可能取函数指针）

---

## 5. 运行期调用流程

### 5.1 extern cfunc 调用（clux → C）

```
libc::printf("hello %d\n", 42)
  │
  ├─ IMPORT "libc.so.6"          → 压 module 值（dlopen 已在 import 时完成）
  ├─ EXTERN_FUNC "printf"        → dlsym + 建 ffi_cif + 构造 ffi_func_t → 压 func 值
  ├─ PUSH_STR "hello %d\n"
  ├─ PUSH_I32 42
  └─ CALL 2                      → func_vcall → ffi_trampoline
                                   │
                                   ├─ value_t 参数逐一转 C 值
                                   ├─ ffi_call(cif, fnptr, &ret, c_args)
                                   └─ 返回值 C→value_t 转换
```

### 5.2 cfunc 定义被 C 调用（C → clux）

```
C 侧: int32_t (*)(uint8_t*, uint64_t) fn = get_callback();
      fn(data, len);
  │
  └─ ffi_closure 入口 → libffi 回调 → trampoline
                         │
                         ├─ C 参数转 value_t（按签名类型）
                         ├─ bcode_call_cfunc 执行 clux 函数体
                         └─ 返回值 value_t→C 转换
```

---

## 6. 类型映射

### 6.1 clux → ffi_type 映射表

| clux | ffi_type | C 类型 |
|------|----------|--------|
| i32 | ffi_type_sint32 | int32_t |
| i64 | ffi_type_sint64 | int64_t |
| u32 | ffi_type_uint32 | uint32_t |
| u64 | ffi_type_uint64 | uint64_t |
| f32 | ffi_type_float | float |
| f64 | ffi_type_double | double |
| bool | ffi_type_uint8 | _Bool |
| str | ffi_type_pointer | const char* |
| opaque | ffi_type_pointer | void* |
| ref *T | ffi_type_pointer | T* |
| fatal *T | ffi_type_pointer | T* |
| enum | 按底层宽度选 ffi_type_sintN | enum |
| struct | 按字段构造 ffi_type | struct（按值传递） |

### 6.2 可变参数

- `extern cfunc` 的 `...` 参数：libffi 用 `ffi_prep_cif_var` 处理
- 固定参数 + 可变参数分开传
- 可变参数的类型由调用点实参推断（运行期动态构造 ffi_type 数组）

---

## 7. 所有权与 FFI 边界

M3 §8.5：所有权规则对 FFI 边界同样生效，不豁免。不需要 `unsafe` 块。

### 7.1 参数传递

| clux 参数 | C 侧接收 | 所有权语义 |
|-----------|---------|-----------|
| `str` | `const char*` | C 侧不释放，clux 保留所有权 |
| `ref *T` | `T*` | 借用，C 侧不释放，clux 保留所有权 |
| `fatal *T` | `T*` | raw 指针，C 侧自由操作 |
| `opaque` | `void*` | raw 指针，C 侧自由操作 |
| struct 按值 | struct | ffi 按布局拷贝，两侧各有一份 |
| i32/i64/f32/f64/bool | 按值 | 拷贝，无所有权问题 |

### 7.2 返回值

| clux 返回类型 | C 侧返回 | 所有权语义 |
|--------------|---------|-----------|
| `fatal opaque` | `void*` | 调用方获得 raw 指针，需手动 free |
| `fatal *T` | `T*` | 调用方获得 fatal 指针 |
| `ref *T` | `T*` | 借用（C 侧必须保证生命周期） |
| `str` | `const char*` | clux 侧拥有字符串（需后续释放？待定） |
| i32/i64/f32/f64/bool | 按值 | 拷贝 |
| struct 按值 | struct | 拷贝 |

---

## 8. libffi 集成

### 8.1 引入方式

- git submodule 引入 libffi
- 路径：`third_party/libffi`
- CMake：`add_subdirectory(third_party/libffi)` 或 `FetchContent`

### 8.2 平台条件编译

```c
#ifdef _WIN32
  #include <windows.h>
  /* LoadLibrary / GetProcAddress / FreeLibrary */
#else
  #include <dlfcn.h>
  /* dlopen / dlsym / dlclose */
#endif
```

### 8.3 关键 libffi API

- `ffi_prep_cif` / `ffi_prep_cif_var`：构造调用接口（固定/可变参数）
- `ffi_call`：动态调用 C 函数
- `ffi_closure_alloc` / `ffi_closure_free`：创建/释放 closure（cfunc 定义）
- `ffi_prep_closure_loc`：绑定 closure 回调

---

## 9. sema 变更

### 9.1 动态库 import 处理

- 路径以 `.so` / `.dll` / `.dylib` 结尾：
  - 不递归 `compile_module`
  - 注册别号为 `SEMA_SYM_MODULE` 符号，但标记 `is_dynamic = true`
  - 不构建 exports（运行期 dlsym 动态查找）

### 9.2 extern cfunc 声明

- 验证 `lib::func` 中的 `lib` 是动态库符号（`SEMA_SYM_MODULE` 且 `is_dynamic`）
- 注册 `func` 为全局 FUNC 符号，签名类型已知
- 记录 `dl_handle` 关联（从 module 符号获取）
- 类型检查正常进行（调用点按签名校验参数）

### 9.3 cfunc 定义

- 照常 sema 分析（作用域树 + shadow 执行）
- 额外校验 C 兼容类型约束：
  - 参数/返回类型在允许列表中
  - 无 `own *T`、无 option/tag union/slice
  - 无闭包捕获列表
  - 无 `...` 可变参数

### 9.4 CTFE 约束

- CTFE 禁止调用 `extern cfunc`（无 AST_FUNC_DEF，不可解释执行）
- CTFE 禁止调用 `cfunc` 定义（C ABI 函数不参与编译期求值）

---

## 10. compiler 变更

### 10.1 extern cfunc

- 不生成字节码函数体
- 调用点 `libc::printf(...)` 生成：
  - `IMPORT <canonical path>` → 压 module 值
  - `EXTERN_FUNC <func name>` → dlsym + 建 ffi_func_t → 压 func 值
  - 参数压栈
  - `CALL <argc>`

### 10.2 cfunc 定义

- 照常编译函数体（和普通 func 一样）
- 函数对象构造时额外创建 ffi_closure
- `PUSH_FUNCTION` 阶段：构造 `ffi_func_t`（而非 `bcode_function_t`），同时创建 closure

---

## 11. vm 变更

### 11.1 新增指令处理

- `op_extern_func`：读 STR 函数名 → 从当前 module 的 dl_handle 用 dlsym 查找 → 构造 ffi_cif + ffi_func_t → 压栈

### 11.2 ffi_trampoline

```
value_t *ffi_trampoline(vm_t *vm, func_t *self, size_t argc, value_t **args):
  ffi_func_t *ff = (ffi_func_t *)self;
  
  /* 1. value_t 参数 → C 值（按签名类型逐一转换） */
  /* 2. ffi_call(ff->cif, ff->fnptr, &ret, c_args) */
  /* 3. 返回值 C→value_t 转换 */
  /* 4. 返回结果 value */
```

### 11.3 ffi_closure_callback（cfunc 定义被 C 调用）

```
void ffi_closure_callback(ffi_cif *cif, void *ret, void **args, void *user_data):
  ffi_func_t *ff = (ffi_func_t *)user_data;
  
  /* 1. C 参数 → value_t（按签名类型） */
  /* 2. bcode_call_cfunc(vm, ff, argc, value_args) */
  /* 3. 返回值 value_t→C 写入 ret */
```

---

## 12. 销毁

### 12.1 ffi_func_t 销毁

- `ffi_closure_free(closure)`（cfunc 定义时）
- `ffi_cif_free(cif)`（如果 libffi 提供释放 API；否则 cif 是栈/静态分配则不需要）
- `func_destroy` 释放 func_t 基类部分

### 12.2 module_t 销毁

- `dlclose(dl_handle)`（非 NULL 时）
- 其余销毁顺序不变

### 12.3 vm_destroy 销毁顺序

```
1. 作用域链（root_scope, global_scope）
2. 字符串池
3. 模块表（module_destroy：dlclose 动态库模块 + scope_destroy + bcode_destroy）
4. 执行器操作数栈
5. 函数池（func_destroy / ffi_func_t 销毁：closure_free + cif_free + func_destroy）
6. 类型池
7. 其余 vm 资源
```

---

## 13. 实现阶段

### Phase 1: libffi 集成 + module_t 扩展
- git submodule 引入 libffi
- CMake 集成 libffi 构建
- module_t 新增 dl_handle 字段
- 动态库 import 的 dlopen/dlclose（平台条件编译）

### Phase 2: parser — cfunc / extern cfunc 语法
- `cfunc` 关键字修饰函数定义
- `extern cfunc lib::func(...)` 声明语法
- `...` 可变参数语法（仅 extern cfunc）
- AST 节点：AST_CFUNC_DEF / AST_EXTERN_FUNC

### Phase 3: sema — extern cfunc / cfunc 校验
- 动态库 import 注册（is_dynamic 标记）
- extern cfunc 声明校验 + 符号注册
- cfunc 定义 C 兼容类型约束校验
- CTFE 禁止调用约束

### Phase 4: ffi_func_t + 字节码指令
- ffi_func_t 数据结构
- BCODE_EXTERN_FUNC 指令 + op_extern_func 实现
- ffi_trampoline（value→C 转换 + ffi_call + C→value）
- 类型映射表实现

### Phase 5: cfunc 定义 + ffi_closure
- cfunc 定义的编译（字节码体 + ffi_closure 创建）
- ffi_closure_callback（C→clux 回调）
- 函数指针传递给 C 接口

### Phase 6: 端到端测试 + 文档
- extern cfunc 调用外部 C 库（printf/malloc/free）
- cfunc 定义作为回调传给 C（qsort 等）
- 动态库导出 cfunc
- 全测试通过 + 文档更新
---

## 14. 实现现状与"最后一公里"规格（2026-10）

### 14.1 已完成（可运行、有测试）

| 部分 | 位置 | 证据 |
|------|------|------|
| 语法定稿 `extern lib "<路径>" as <名>;`（关键字判据，不做后缀判类） | lexer / parser / AST_EXTERN_LIB / fmt | `clux format` 幂等保留；`fake.dll` 文本文件加载失败 |
| `cfunc <名>(参数): 返回 { 体 }` 定义 | AST 标志 `is_cfunc` + `parse_cfunc_def` | 解析/类型检查通过，format 保留 `cfunc` |
| `extern cfunc <库>::<名>(参数): 返回;`（含 `...`） | `parse_extern_cfunc` + lexer 合成 `...` | 解析通过；format 原样保留 `crt::printf(format: str, ...): i32;` |
| 动态库加载（dlopen/LoadLibrary）+ 句柄生命周期 | `src/vm/ffi_lib.c`、`module_t.ffi_libs`（别名 → 句柄） | 真实加载 msvcrt/kernel32；重复别名诊断 |
| 符号解析（dlsym/GetProcAddress） | driver 在 pool 释放前扫描 AST | 缺符号 → `GetProcAddress error 127`；未声明库 → 明确诊断 |
| **最小 ABI 层 v0（能真调用 C）** | `ffi_call_int(sym, argc, argv, out, err, ecap)` | `malloc(16)` → 写 `"hello"` → `strlen` = 5 → `free` |
| FFI 调用点明确诊断 | `SEMA_SYM_EXTERN_LIB` + `::` 分支 | `FFI call 'crt::malloc' is not implemented yet (M6 Phase 4/5: ...)` |

### 14.2 最后一公里（下一步实现，规格已定）

目标：让 `crt::malloc(16)` 在 clux 源码里**直接可用**。

1. **sema**：`extern cfunc` 不再跳过——建函数对象（签名来自解析后的 param/return 类型）、
   分配 `fid`（与普通函数同一分配器，保证 `CALL` 路径不用改），符号注册为
   `SEMA_SYM_FUNC` 且带 `is_extern` 标志；`crt::malloc` 的 `::` 分支从"诊断"改为
   "返回该函数符号的 shadow 值"。**同批**加入 §2 类型约束校验
   （允许 `i32/i64/u32/u64/f32/f64/bool/str/opaque/enum/struct/cunion` 与
   `ref/weak/fatal` 指针；禁止 option / tag union / slice / 闭包 / 类型值），
   以及"浮点参数在最小 ABI 层下不可用"的显式拒绝（若走 libffi 则允许）。
2. **compiler**：识别 `is_extern` 函数调用，发射 **`BCODE_EXTERN_CALL`**
   （操作数：`<str "lib::name">` + `<u8 argc>` + `<u8 ret_kind>`），而不是普通 `CALL`；
   该函数**不进入**运行时函数队列（`prescan_func` 已早退），因此不生成函数体字节码。
3. **vm**：新 opcode 处理器
   - 从**当前执行模块**取库表（`bc->module_path` → `vm->modules` → `module_t.ffi_libs`），
     按 `"lib::name"` 取句柄 → `ffi_lib_sym`（命中即缓存到按 `strmap` 的**符号缓存**，
     避免每次调用都 dlsym）；
   - 按 `argc` 从执行栈弹出实参并打包为 `int64_t[]`
     （整数按声明宽度**符号/零扩展**；指针原样传递；`str` 传 `const char*`；
     浮点 → v0 明确报错）；
   - 调 `ffi_call_int`，按 `ret_kind` 把 `int64` 结果**截断/扩展**为声明类型后压栈；
     `void` 返回压 undefined。
4. **测试**：字节码级用例（`strlen` 真调用：把宿主 C 字符串指针作为 `i64` 压栈 →
   `EXTERN_CALL` → 期望 5）；源码级用例（`crt::malloc(16)` 返回 `opaque` 非空、
   `free` 成功）；错误路径（浮点参数被拒、未声明库、符号缺失）。
5. **`.cxs`/`.cxb`**：`BCODE_EXTERN_CALL` 需在反汇编/汇编往返中稳定（新增助记符 +
   操作数渲染，纳入既有往返字节一致测试）。

### 14.3 libffi 决策点（阻塞 Phase 5，且影响 14.2 第 1 步）

- **回调（`cfunc` 被 C 调用，Phase 5）无法用函数指针强转绕过**，必须 libffi
  （`ffi_closure`）或手写 asm trampoline；
- **浮点/结构体传参**在 x64 需要 XMM/栈布局约定，最小 ABI 层不做；
- 因此：若选 **libffi**（submodule 或 vendor），14.2 可实现为"按 `ffi_type` 描述符
  调用"，浮点/结构体/回调一次到位；若继续用**最小 ABI 层**，14.2 只支持
  整数/指针，浮点在 sema 层拒绝，回调留待后续。
### 14.4 libffi 接入进展与 `ffi_call` 崩溃诊断（2026-10，逐条实测）

**已就位**：libffi **v3.8.0 以 git submodule** 引入（`.gitmodules` + gitlink，`git submodule update --init --depth 1 third_party/libffi`）；树外端口 `third_party/libffi-port/`（CMake + `fficonfig.h` + `ffi_cfi.h` + `preprocess_asm.cmake`）能编出 `ffi.lib` 并链进 `clux_vm`。

**关键构建发现**（都踩过）：
1. 静态构建宏是 **`FFI_STATIC_BUILD`**（3.8 起 `FFI_BUILDING` 失效），否则 `FFI_API` 展开为 `dllimport`；
2. Windows x64 源清单是 `prep_cif.c`/`types.c`/`raw_api.c`/`java_raw_api.c`/`closures.c`/`dlmalloc.c` + `x86/{ffi.c,ffiw64.c,win64.S}`，**不含** `ffi64.c`（Unix 后端，内部用 `FFI_UNIX64`/`FFI_EFI64`）；
3. `ffi.h` 需要 `ffitarget.h`（故端口把 `src/x86` 设为 PUBLIC include）；`ffi_cfi.h` 上游由 configure 生成（clone 里没有），手写"空 CFI 宏"变体；
4. `win64.S` 是 GAS 语法 + `cfi_*`，clang 的 COFF 汇编器不接受 → 端口用 `clang -E -x assembler-with-cpp` 预处理后剥掉 `cfi_*`/`.seh_*` 再汇编（实测生成物 `cfi_`/`.seh_` 残留均为 0）。

**`ffi_call` 崩溃诊断链**（每步实测，均未改仓库代码）：

| 实验 | 结果 | 结论 |
|---|---|---|
| `ffi_prep_cif`（0/1 参） | `FFI_OK`，`abi=1(FFI_WIN64)`，`bytes=32`，`flags=12(=FFI_TYPE_SINT64)`，`rtype` 有效 | `cif` **完全正确** ✓ |
| `ffi_call` 调 `kernel32!GetCurrentProcessId`（0 参） | 崩 | 与参数打包无关 |
| `ffi_call` 调**本地 C 函数** `zero()`（0 参） | 崩 | 与 DLL/GetProcAddress 无关；libffi 调用路径本身不工作 |
| 剥 `.seh_*` 一并去掉 | 仍崩 | 元数据剥离不是原因 |
| 补 `X86_64`、补 `_CET_ENDBR` | 仍崩 | 两个假设排除 |
| `llvm-nm` | `ffi_call`/`ffi_prep_cif_machdep` 均来自 `ffiw64.c` ✓、`ffi_call_win64` 来自生成的汇编 ✓ | 链的是正确实现 |
| `llvm-objdump` 反汇编 `ffi_call_win64` | 序言与上游源码逐条一致（`movq (%rsp),%rax` → `%rdx`→`%rbp` → **`movq %rcx,%rsp`**） | 汇编正确 |
| SEH 捕获取故障地址 | **`__except` 未触发**、进程硬崩 | `%rsp` 被写坏 → 异常分发也跑不了 |

**当前结论（下一步的落点）**：`ffi_call_win64` 的第一条有效动作是把 `rcx` 装入 `rsp`（`arg0 = %rcx` 必须是 `ffi_call` 用 `alloca` 备好的新栈区）。汇编正确、`cif` 正确、契约（`ffi_call_win64(void *stack, struct win64_call_frame *, void *closure)`，见 `ffiw64.c:51/227`）一致，但 `rsp` 被写坏 ⇒ **C 侧（`ffi_call_int`）传给汇编的 `stack` 是垃圾**。
下一步取证：反汇编 `ffiw64.c.obj` 中 `ffi_call`/`ffi_call_int` **调用 `ffi_call_win64` 之前**的指令序列，确认 `rcx` 的来源（`alloca` 结果 vs 某字段），并核对 `ffiw64.c:160-196` 的 `stack`/`frame` 计算。

**测试与回退状态**：调用路径暂用**已验证的最小 ABI 层**（整数/指针，函数指针按参数个数分派）；`ffi_call_typed`（浮点/结构体）仍在，浮点用例（`sqrt`/`pow`）挂在 `CLUX_FFI_TYPED_CALLS` 下不编译，调通后加一个宏即生效。
### 14.5 浮点调用已可用（走类型化调用层，不依赖 libffi 汇编）

`ffi_call_f64(sym, argc, double *args, double *out, err, ecap)`（`include/vm/ffi_lib.h`）：
用"按参数个数分派的**类型化**函数指针"实现，x64 下 `double` 参数走 XMM0-3、返回走 XMM0，
编译器生成的调用序列即正确 ABI 约定，因此**不需要 libffi 的动态汇编**。

实测（`FfiLibTest`）：`sqrt(9.0)=3.0` ✓、`pow(2,10)=1024.0` ✓；配合原有整数/指针层
（`malloc`→写→`strlen`=5→`free` ✓），FFI 调用层现已覆盖 **整数/指针/浮点**。

`libffi` 的 win64 汇编路径本身仍未修好（见 §14.4 诊断链；本轮又排除两个假设：
`_CET_ENDBR` 在生成汇编里并不存在、加 `-fcf-protection=branch` 编译调用方仍崩；
`ffi_call_plan_invoke` 只是 `ffi_call` 的包装，无法绕开汇编）。上游正统 Windows 路径需要
`msvcc.sh` + MASM（`ml64`），本机没有该工具链。**语言层的 FFI 调用将先用类型化调用层实现，
libffi 仅在需要结构体/变参/回调时再回头处理其汇编构建。**
### 14.6 `BCODE_EXTERN_CALL` 已落地（VM 侧）

- **操作码**：追加在 `bcode_op_t` **末尾**（`include/vm/bcode.h`）——不得插在中间，否则会
  改变既有操作码编号、静默误读旧 `.cxb`/`.cxs`；操作数 `STR "lib::name" + U32 argc + U32 ret_kind`，
  已在 `src/vm/bcode_asm_defs.c` 登记（`.cxs` 往返 5/5 字节一致验证通过）。
- **处理器** `op_extern_call`（`src/vm/exec.c`，注册于派发表）：
  `bc->module_path` → `vm_module_lookup` → `module_t.ffi_libs`（别名 → 句柄）→
  `ffi_lib_sym` → 按 `argc` 弹实参（按 int64 宽度）→ `ffi_call_int` → 按 `ret_kind`
  归一化（VOID/I32/I64/PTR）。缺库、缺符号、`lib::name` 畸形、参数过多都给出明确诊断。
  `ret_kind = F64` 走 **`ffi_call_f64`**（v0 规则：F64 返回 ⇒ 全 double 签名；实参须为
  f32/f64，整数实参明确报错而不猜着转换）。
- **C 侧访问器**：端口为 `module_t` 增加了 `ffi_libs`（见 §14.4/§14.5 的库表改造）。
- **已验证（字节码级真调用）**：`tests/ffi_extern_call_test.cpp`（5 例）——宿主 C 字符串
  指针作为 i64 压栈 → `EXTERN_CALL "crt::strlen" argc=1 ret=i64` → **返回 5** ✓；
  `PUSH_F64 9.0 → EXTERN_CALL "crt::sqrt" argc=1 ret=F64` → **3.0** ✓；另有 3 类反例
  （库别名未加载、符号不存在、`lib::name` 畸形）各自明确诊断 ✓。测试自建模块并把
  `msvcrt.dll` 放进 `module_t.ffi_libs`、`bc->module_path` 指向它，用 `exec_drive(vm, bc, 0)`
  执行（`exec_run` 不执行顶层语句）。
- **下一步**：见 §14.7（前端接线方案）。
### 14.7 前端接线方案（`crt::strlen(s)` 源码可调）——下一轮执行

运行时已就绪：`BCODE_EXTERN_CALL <STR "lib::name"> <U32 argc> <U32 ret_kind>` +
`op_extern_call`（已验证 `strlen`→5、`sqrt`→3.0，含 3 类反例诊断，见 §14.6）。

**sema（两处）**
1. `src/sema/stmt.c` 的 `pass1_names`：当前对 `extern cfunc` **静默跳过**（为了让无体定义不
   走函数注册路径）。改为**建函数符号**：
   - 解析签名：参数用 `sema_resolve_type_slot(sema, &vd->type_expr)`（`comptime.c` 里有大量
     同款调用），返回类型用 `sema_resolve_type_slot(sema, &fd->return_expr)`；
   - 用 `sema_scope_define(sema->global_scope, 限定名("lib::fn"), &init)` 注册
     `kind = SEMA_SYM_FUNC` 的符号（限定名避免与普通函数重名冲突），并在符号上记录
     "这是 extern cfunc"（可复用 `sema_symbol_t` 的现有字段或加一个 `bool is_extern`）；
   - `fid` **不要分配**（没有函数体、不进入运行时函数队列，编译器也不会为它生成函数体）。
2. `src/sema/expr.c` 的 `::` 分支（约 2027-2052，已插入 `SEMA_SYM_EXTERN_LIB` 诊断）：
   把"报未实现"改为**查限定名符号**：命中 `SEMA_SYM_FUNC` 且 `is_extern` → 返回该函数的
   shadow 值（类型 = 签名）；未命中则保留"未声明符号"诊断。

**compiler（一处）**
3. `src/compiler/compile_expr.c`：函数调用点（现在是 `LOAD_FUNCTION/CALL`）在 callee 解析为
   "extern cfunc"符号时，改发 `BCODE_EXTERN_CALL`：
   - `STR` 操作数 = `lib::name`（sema 在符号上留存的限定名）；
   - `U32 argc` = 实参个数；`U32 ret_kind` = 按返回类型映射：
     `void→0`、`i32→1`、`i64/u64→2`、指针/opaque→3、`f32/f64→4`；
   - 实参按现有表达式求值压栈即可（`op_extern_call` 负责按 int64/double 取用）；
   - **不做** `LOAD_FUNCTION`/`BIND_FUNC`（无函数体）。

**测试（源码级）**
4. `tests/driver_module_bcode_test.cpp` 增源码级用例：
   `extern lib "msvcrt.dll" as crt; extern cfunc crt::strlen(s: str): u64;`
   `func main(): i32 { if (crt::strlen("hello") != 5) { return 1; } return 0; }` → 期望 exit 0；
   另加 `crt::malloc(16)` 返回指针非空、`crt::sqrt(9.0)` 浮点、以及缺库/缺符号的反例。
5. 文档：`docs/m6-design.md` §14.2 标注"已实现"，milestones 追加对应条目。

**注意（踩过的坑）**
- 顶层桩函数与枚举插在**末尾**（`bcode.h`）：中途插入会改操作码编号 → 静默误读旧容器；
- `exec_run` **不执行顶层语句**（只跑类型提升区 + 函数注册段），字节码测试要用
  `exec_drive(vm, bc, 0)`；
- 处理器里"取实参"的两条互斥路径（int64 / double）必须各自完整且只执行一条
  （曾把 F64 分支放在弹参之后 → 栈已空 → 误报 bad f64 argument）。
### 14.8 §2 类型约束判定已落地（尚未接入 sema）

`ffi_c_abi_reject_reason(const type_t *)`（`include/vm/ffi_lib.h` + `src/vm/ffi_lib.c`）：
`NULL` = 可用于 C ABI，否则返回人类可读原因。允许：整数/浮点/`bool`/`str`/`opaque`/
`enum`/`struct`/`cunion`/指针（weak·fatal·ref·share）/`void`（仅返回位置）；拒绝：
`option`、tag union（提示改用 `cunion`）、切片（胖指针）、元组、闭包、类型值、
`const/volatile` 包装。单测 2 例（用真实 VM 类型句柄 + `type_option_intern`/
`type_slice_intern` 构造反例）已通过。

**接入方式**：与 §14.7 的前端接线同批做——在 `pass1_names` 建 `extern cfunc` 符号时，
对每个已解析的参数类型与返回类型调用本函数，非 `NULL` 即以该原因作为诊断文本报错。
（`void` 只允许出现在返回位置：参数位置需另行拒绝。）

### 14.7 补遗：实施所需 API（已探明）

- **建函数签名**：`type_func_sig(vm, params[], nparams, ret, is_variadic)`
  （`vm/type_func.h`；`vm_test.cpp` 有用法示例：`type_func_sig(vm, NULL, 0, vm->type_i64, true)`）；
- **注册符号**：`sema_symbol_t init = {0}; init.kind = SEMA_SYM_FUNC; init.type = <sig>;`
  然后 `sema_scope_define(sema->global_scope, <限定名>, &init)`（`import` 别名的注册同款）；
- **解析类型槽位**：`sema_resolve_type_slot(sema, &slot)`（参数槽 `ast_var_def_t.type_expr`、
  返回槽 `ast_func_def_t.return_expr`）；
- **`::` 分支返回**：`value_make_shadow(sema->vm, <sig>)`（模块成员分支已是同款写法）；
- **compiler 侧**：`lib::name(...)` 的调用点在 callee 为 extern cfunc 时改发
  `EXTERN_CALL`（`STR lib::name` + `U32 argc` + `U32 ret_kind`），**不**做
  `LOAD_FUNCTION`/`BIND_FUNC`（无函数体）。
### 14.9 Phase 5（回调）可行性结论：与调用路径同根受阻

实测（本轮）：libffi 的 `ffi_prep_closure_loc` 在 Windows x64 上依赖两个**汇编符号**
——`ffi_closure_win64`（`ffiw64.c:295` 写入 trampoline）与 `ffi_closure_win64_alt`
（`ffiw64.c:288` 经 `ffi_tramp_set_parms` 绑定），二者都由 `src/x86/win64.S` 提供
（生成的 `win64_nocfi.s` 第 130/147 行）。因此：

- **Phase 5 回调与 §14.4 的 `ffi_call` 崩溃是同一根因**：只要 win64 汇编路径在本机
  不可用，`ffi_call` 与 `ffi_closure` 就**同时**不可用；
- 结论：**回调无法通过"类型化函数指针"绕过**（回调需要运行期生成可执行 trampoline，
  C 语言层面没有等价物），因此 Phase 5 的可行路线只有两条：
  1. **解决 win64 汇编构建**（上游正统路径：`msvcc.sh` 把 `.S` 转 MASM 交给 `ml64`
     —— 需要 VS Build Tools / MSBuild，本机没有）；或
  2. **先修好当前端口**（诊断已收敛到"汇编入口正确、C 侧传参正确、但 `rcx`（新栈区）
     在运行期是垃圾"这一步，见 §14.4 的证据表），再让 `ffi_closure` 复用同一条路径。
- **对当前里程碑的影响**：M6 的"语言层可调用外部函数"（§14.7）**不依赖**回调，
  故可先行完成；Phase 5 保持"待工具链/汇编问题解决"的状态，不在本阶段强行实现。
### 14.7 补遗二：三处代码锚点（已核实，可直接照改）

| 步骤 | 文件:行 | 现状 | 改法 |
|------|---------|------|------|
| ① 注册 extern cfunc 符号 | `src/sema/sema.c:779` | `if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_extern_cfunc) continue;`（**静默跳过**，见 §14.6） | 改为：解析参数/返回类型（`sema_resolve_type_slot`）→ `type_func_sig` 建签名 → **逐个调 `ffi_c_abi_reject_reason` 报错**（§14.8）→ `sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = sig, ...}` + `sema_scope_define(sema->global_scope, <限定名 "lib::name">, &init)`，随后 `continue`（**不分配 fid**，无函数体） |
| ② `::` 分支返回符号 | `src/sema/expr.c` 的 `SEMA_SYM_EXTERN_LIB` 分支（§14.6 里插入的那段） | 目前 `diag_error("FFI call ... is not implemented yet")` | 改为：以 `"<lib>::<name>"` 查全局符号；命中 `SEMA_SYM_FUNC` → `return value_make_shadow(sema->vm, sym->type);`；未命中保留"未声明符号"诊断 |
| ③ compiler 发射 `EXTERN_CALL` | `src/compiler/compile_expr.c:289-300`（装 callee：`LOAD_FUNCTION <fid>`）与 `:447`（`BCODE_CALL`） | callee 一律按 fid 走 `LOAD_FUNCTION` + `BCODE_CALL` | callee 是 extern cfunc 时：**不**发 `LOAD_FUNCTION`，改发 `BCODE_EXTERN_CALL`（`STR "lib::name"` + `U32 argc` + `U32 ret_kind`）；`ret_kind` 用**指针比较**返回类型得出：`type_void→0`、`type_i32→1`、`type_i64/type_u64→2`、`opaque/指针→3`、`type_f32/type_f64→4` |

**样板**：函数符号注册照 `sema.c:1179`（`sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = psig, ...}`）；
`type_func_sig` 用法见 `tests/vm_test.cpp`（`type_func_sig(vm, NULL, 0, vm->type_i64, true)`）。

**另需注意**：`compile.c:235` 的"全局函数名绑定"（`LOAD_FUNCTION <fid>` + `DEFINE name`）**不要**
覆盖 extern cfunc（它们没有 fid）；当前预扫描已跳过无体定义（`compile_func.c` 的 `prescan_func`
早退），故该绑定循环不会遇到它们——接线后需再确认一次。
### 14.7 进度：步骤 ① 已完成（2026-10）

`src/sema/sema.c` 的 `pass1_names` 已把 `extern cfunc` 从"静默跳过"改为**注册限定名
函数符号**（`"<lib>::<name>"`，`SEMA_SYM_FUNC` + `type_func_sig` 签名，**无 fid**）并接入
**§2 C ABI 校验**。实测：`?i32` 返回 → `return type: option has no C representation` ✓；
`fatal []i32` 参数 → 切片被拒 ✓；合法签名不受影响 ✓（新增 2 个 driver 用例）。

**剩余**：步骤 ②（`expr.c` 的 `SEMA_SYM_EXTERN_LIB` 分支改为返回该符号的 shadow）与
步骤 ③（`compile_expr.c:289-300`/`:447` 改发 `EXTERN_CALL`）——**必须同批**，否则调用点会
type-check 通过但在运行期报错。锚点表与 `ret_kind` 映射见 §14.7 补遗二。
### 14.7 补遗三：compiler 侧的两个关键判定（已核实，可省一轮试错）

读 `src/compiler/compile_expr.c` 后确认两件事，直接决定步骤 ③ 怎么写：

1. **如何判定"这是 FFI 调用"**：`AST_CALL`（`:439-451`）里 callee 若是
   **`AST_ENUM_REF`**（即 `<库>::<名>` 的 `::` 形式），就一定是 FFI 调用——
   枚举 variant（`Color::Red`）**不可能被调用**（写了就是语法/语义错），因此这个
   判据是**充分且廉价**的，不需要在 compiler 里另建 extern cfunc 表。
   命中后：**不**编译 callee（不发 `LOAD_FUNCTION`/`PUSH`），只按序编译实参，然后发
   `EXTERN_CALL <STR "lib::name"> <U32 argc> <U32 ret_kind>`，`st_push(c, -(int)argc)`。
2. **`ret_kind` 怎么算**：用 `c_resolve_type(c, <return_expr>)`（同文件 `:466` 有用法）
   把声明的返回类型解析成 `type_t*`，再与 `c->vm->type_void/type_i32/type_i64/
   type_u64/type_f32/type_f64/type_opaque` 做**指针比较**映射为
   0/1/2/2/4/4/3；指针类型（`TYPE_KIND_PTR_*`）也映射为 3。返回 `type_t*` 的来源：
   同文件里按 `lib::name` 在 `AST_FUNC_DEF(is_extern_cfunc)` 中查找（可在
   `compile_prescan_funcs`（`compile.c:146` 附近）顺手收集，或在该判定点做一次
   线性扫描——FFI 调用点数量少，扫描成本可忽略）。
3. **不要忘记**：`compile.c:235` 的全局函数名绑定（`LOAD_FUNCTION <fid>` + `DEFINE`）
   不得覆盖 extern cfunc（无 fid）；`prescan_func` 已早退，接线后复核一次即可。
### 14.7 步骤 ②③ 的实施契约（已预置 AST 槽位）

`ast_call_t` 现已有 `bool is_ffi_call` 与 `uint8_t ffi_ret_kind`（见 `ast_call.h` 注释）：

- **步骤 ②（sema）**：`expr.c` 的 `SEMA_SYM_EXTERN_LIB` 分支改为查限定名符号并返回其
  shadow（func 类型）；**再在 sema 的 `AST_CALL` 处理点**判断 callee 的 shadow 是否为
  extern cfunc 签名，命中则置 `call->is_ffi_call = true` 并按签名返回类型写
  `ffi_ret_kind`（映射同字段注释）。
- **步骤 ③（compiler）**：`compile_expr.c` 的 `AST_CALL`（`:439-451`）在
  `n->is_ffi_call` 为真时：**不**编译 callee，只编译实参，发
  `EXTERN_CALL <STR 限定名> <U32 argc> <U32 ffi_ret_kind>`，`st_push(c, -(int)argc)`
  （限定名由 callee 的 `AST_ENUM_REF` 的 `type_expr` 名 + `variant` 拼出）。
- **两处必须同批**：只做 ② 会让调用点通过类型检查却在运行期报 "invalid function"。
### 14.10 ✅ M6 语言层打通（2026-10，端到端实测）

`crt::abs(-7) == 7` 与 `crt::sqrt(9.0) == 3.0` **在 clux 源码里直接调用成功**（exit 0）。
完整链路：`extern lib` 声明 → driver 编译期 dlopen/dlsym（符号不存在即报错）→
sema `ex_call` 拦截并置 `ast_call_t.is_ffi_call`/`ffi_ret_kind` → compiler 发
`BCODE_EXTERN_CALL` → VM `op_extern_call` 从模块库表取句柄 → 类型化调用层
（整数/指针 `ffi_call_int`、浮点 `ffi_call_f64`）。

- **实现要点**：sema 侧只需在 `ex_call` 开头拦截（callee 为 `AST_ENUM_REF` + 库别名为
  `SEMA_SYM_EXTERN_LIB`），`::` 分支无需改动；`ret_kind` 由 `func_type_return(sym->type)`
  指针比较得出。
- **§2 校验已接入**：`extern cfunc` 建符号时逐参数/返回类型校验（option/切片/元组/
  闭包/类型值被拒，见 §14.8）。
- **未完成**：Phase 5 回调（`ffi_closure` 依赖 win64 汇编，与 §14.4 的 `ffi_call` 崩溃
  同根受阻，见 §14.9）。结构体/变参传参亦受限于类型化调用层（v0 只做整数/指针/浮点）。
### 14.4 补充：已排除的假设清单（截至 2026-10，共 11 项）

`ffi_call` / `ffi_call_win64` 崩溃的排查已排除以下假设（每条均为实测）：

| # | 假设 | 实测结果 |
|---|------|----------|
| 1 | API 误用（描述符/ABI） | `ffi_prep_cif` 返回 `FFI_OK`，`abi=FFI_WIN64(1)`，`bytes=32`，`flags=12(=FFI_TYPE_SINT64)`，`rtype` 有效 → 排除 |
| 2 | 参数打包错 | **0 参**调用同样崩 → 排除 |
| 3 | DLL/`GetProcAddress` 指针问题 | 调**本地 C 函数**同样崩 → 排除 |
| 4 | 汇编逻辑被剥坏 | 反汇编与上游源码逐条一致（含 `movq %rcx,%rsp`、`call *0x10(%rbp)`）→ 排除 |
| 5 | C 侧传参错 | 反汇编 `ffi_call_int`：`rcx=stack`、`rdx=frame`、`r8=closure`，正确 → 排除 |
| 6 | 元数据剥离（`cfi_*`/`.seh_*`） | 全部剥掉仍崩 → 排除 |
| 7 | 缺 `X86_64` 宏 | 补上仍崩 → 排除 |
| 8 | `_CET_ENDBR` 未展开/为空 | 生成汇编中该宏**并不存在**；入口指令干净 → 排除 |
| 9 | CET/IBT（间接调用缺 `endbr64`） | 调用方加 `-fcf-protection=branch` 仍崩 → 排除 |
| 10 | 新栈区**下方**空间不足（Windows 无 red zone） | 传入指针上移 64B 留足余量仍崩 → 排除 |
| 11 | 用 `ffi_call_plan_*` 绕开汇编 | `ffi_call_plan_invoke` 只是 `ffi_call` 的包装（`prep_cif.c:411`）→ 排除 |

**现存事实**：直接调 `ffi_call_win64`（手工搭 frame：`rbp/ret/fn/flags/rvalue`，与上游
`internal64.h` 同布局）在第一条指令序列之后、`call *fn` 之前崩；`__try/__except`
**完全无法触发** ⇒ `rsp` 已被写坏 ⇒ 异常分发失败。上游正统 Windows 路径需
`msvcc.sh` + MASM（`ml64`），本机无该工具链。

**结论**：M6 的调用能力已由**类型化调用层**等价交付（整数/指针/浮点，0/1/多参均已实测）；
libffi 原生路径作为**独立技术债**单列（需要调试器或 MASM 工具链），不再阻塞 M6 功能。
### 14.11 修复：指针返回值的类型（`crt::malloc(16)` 例子打通）

**症状**：目标里点名的 `var p: opaque = crt::malloc(16); crt::free(p);` 运行期报
`run: implicit cast: not a widening conversion`（exit 1）。

**根因**：`op_extern_call` 里 `ret_kind = PTR` 与 `I64` 共用一个分支，指针返回值被压成
**`i64` 类型**；而声明是 `opaque` 变量 → 运行期 i64→opaque 的隐式转换被拒。

**修复**：把 `FFI_RET_PTR` 拆为独立分支，压 **`opaque` 类型**的值（`vm->type_opaque`）。
复验：目标例子 **exit 0** ✓（真 `malloc` + 真 `free`）。

**顺带**：三个"占位诊断时代"的旧断言（仅声明 extern cfunc 即应失败、调用点应报未实现）
已随功能打通更新为 `EXPECT_EQ(..., 0)` ✓。

**方法教训（本轮补齐）**：① 探针文件不要放会被清理的目录（`build/tmp` 每轮清空）；
② "写探针 → 运行 → 读诊断"必须在同一次调用内完成，否则拿到的是"文件不存在"的假失败；
③ 文本替换前先数命中次数（0 命中即未生效，避免误以为已改）。
### 14.12 ✅ 结论更正：libffi 原生路径已打通（此前 §14.4 的结论是错的）

**更正**：§14.4 把根因写成"libffi 的 win64 汇编路径在本机不可用"，这是**错误结论**。真正的
根因是**我选错了汇编器与语法变体**：用 clang 的集成汇编器去汇编 **GAS 语法**的
`src/x86/win64.S`，产出的 `ffi_call_win64` 在运行期写坏 `rsp`（0 参调用即崩、
`__try/__except` 无法触发；反汇编却与上游逐条一致 —— 这正是§14.4 那 11 项假设全部
"排除"却始终不解的原因：前提本身就错了）。

**正确做法（上游 README 的 Windows 正统路径）**：VS Build Tools 里的 **MASM `ml64`**
汇编 libffi 为 MSVC 准备的 **Intel 语法变体 `src/x86/win64_intel.S`**。实测：

- 本机确实装有 VS Build Tools：`D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`
  （`VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\{cl,ml64,link,lib}.exe`、`MSBuild`，
  Windows SDK 于 `C:\Program Files (x86)\Windows Kits\10\bin\x64`）——只是不在 PATH、
  也不在 C 盘，故早期的 `Get-Command`/C 盘搜索都没找到；
- 独立探针（libffi 原生 `ffi_call`）：0 参本地函数 = **42**、`strlen("hello")` = **5**、
  `sqrt(9.0)` = **3.0** ✓；
- 端口已切换：`vswhere` 自动定位 ml64（可用 `-DCLUX_ML64=` 覆盖）；`clang -E -P` 预处理
  `win64_intel.S` → `ml64 /c` → `win64_masm.obj` 作为 `EXTERNAL_OBJECT` 加入 `ffi`；
  **找不到 ml64 时自动回退**旧的 clang 汇编路径（仍可构建，但 `ffi_call` 运行期不可用）。

**据此解除了两个"阻塞"**：
- `ffi_call_typed` 原本是**无条件桩**（始终报 "this build has no libffi"），现已接上
  `ffi_prep_cif` + `ffi_call`（`#if defined(CLUX_HAVE_LIBFFI)`，否则保留原桩）；
- 打开 `CLUX_FFI_TYPED_CALLS` 后，gated 用例 **LibffiCallsFloatFunctionSqrt /
  LibffiCallsTwoDoubleFunctionPow 全部通过** ✓（`FfiLibTest` 12/12、全量 1607/1607）。

**端口改造中踩到的三个坑（供后人）**：① CMake 的 `$ENV{ProgramFiles(x86)}` 因**括号**破坏
解析 → 改用字面路径；② 预处理必须加 **`-P`**，否则 clang 输出的 `# ` 行标记让 ml64 报
`invalid character in file`；③ `ml64` 的 `/Fo` 与路径必须写成**一个** CMake 参数，写成
`/Fo"path"` 会被 VERBATIM 转义成 `\"` → `fatal error A1000: cannot open file`。

**仍待完成**：Phase 5 回调（`ffi_closure_alloc`/`ffi_prep_closure_loc`；符号 `ffi_closure_win64`
已在 ml64 产物中 ✓）。
### 14.13 ✅ 闭包（Phase 5 回调）机制已验证

**端口补漏**：`ffi_closure_alloc`/`ffi_prep_closure_loc` 需要 `ffi_tramp_*`，它们由 libffi 的
`src/tramp.c` 提供——而 `closures.c` 在 `FFI_EXEC_TRAMPOLINE_TABLE` 被定义时**不提供桩**，
所以端口源列表必须显式加入 `tramp.c`（原先缺它 → 链接闭包会报 4 个 undefined symbol：
`ffi_tramp_is_supported/_alloc/_get_addr/_free`）。已加入 `third_party/libffi-port/CMakeLists.txt`。

**实测（独立探针，链接端口产出的 `ffi.lib`）**：注册一个 libffi 闭包处理器，把生成的
trampoline 当普通 C 函数指针调用：

```
closure: 20+22 = 42 (expect 42), calls=1
CLOSURE OK
```

即**运行期生成可执行 trampoline** 在本机构建下完全可用 ✓ —— 目标第 (4) 项的机制基础成立。

**尚缺（语言层接线）**：把 clux 的 `cfunc <名>(...): T { body }` 定义包装成闭包处理器——
用 `ffi_prep_cif` 描述其 §2 签名 → `ffi_closure_alloc` + `ffi_prep_closure_loc` 生成函数指针 →
handler 内按签名把 C 参数编组成 clux 值、调用解释器执行该 `cfunc` 体、再按返回类型回写。
本仓库现有的 `cfunc` 定义目前按**普通 clux 函数**编译（可被 clux 调用，但不能作为 C 回调传出）。
### 14.14 闭包运行时 API 已并入项目 + 一个卡了三轮的诊断教训

**API**（`include/vm/ffi_lib.h` / `src/vm/ffi_lib.c`）：

```c
typedef void (*ffi_closure_handler_t)(void *ret, void **args, void *user);
bool clux_ffi_closure_create(allocator_t *alloc, size_t argc, void **arg_types,
                             void *ret_type, ffi_closure_handler_t handler,
                             void *user, void **out_handle, void **out_code,
                             char *err, size_t ecap);
void clux_ffi_closure_destroy(allocator_t *alloc, void *handle);
```

设计要点（都是踩过坑之后的结论）：

- **公共头只用不透明指针**（`void **arg_types` / `void *ret_type`）——若在头文件里写
  `const struct _ffi_type **`，而该头又在 `ffi_lib.c` 中**早于 `<ffi.h>`** 被包含，则
  `struct _ffi_type` 在两处的含义不同（"原型自带的不完整结构" vs "libffi 的完整结构"），
  clang 会报 `conflicting types`。用不透明指针同时改善了封装：libffi 类型成为 .c 的私有细节。
- **内存**：trampoline 由 libffi 自己分配（不属于本项目 `allocator_t`），box/cif 用
  `malloc`/`free`（本项目没有裸字节分配 API，只有 `allocator_new`/`allocator_new_ex`）。
  因此**必须**调用 `clux_ffi_closure_destroy`，否则静默泄漏可执行内存
  （`delete_allocator` 的泄漏检测看不到 libffi 的内存）。
- **所有权**：显式 `handle`（`out_handle`）换取 `out_code`（可当 C 函数指针用的地址）。
- **端口前置**：`src/tramp.c` 必须加入 `ffi` 目标（§14.13）。

**⚠️ 诊断教训（卡了三轮，值得单列）**：`clux_ffi_closure_create` 连续三轮报
`conflicting types`，而两份声明在**预处理输出里逐字节相同** ✗。真因是**文档注释里写了
`ffi_type**/ffi_type*`** —— 其中的 **`*/` 提前结束了块注释**，后续文字变成代码垃圾。
预处理产物会剥掉注释，所以盯着 `clang -E` 的输出永远看不出问题 ✗。

**规则**：在 C 注释里写含 `*/` 的类型写法（`T**/T*`）会提前闭合注释；当"两份文本相同却
报冲突"时，要回看**含注释的源码**，而不是只看预处理结果。
### 14.15 ✅ 闭包 API 已通过单测（Phase 5 基础设施完成）

`FfiLibTest.ClosureProducesCallableFunctionPointer`：`clux_ffi_closure_create` 生成
trampoline → 当 `int (*)(int,int)` 调用 → `20+22 = 42` ✓（诊断行 `create=1`, `code=0x…`）。

此前该用例失败并非机制问题，而是**用例仍按旧的 `struct _ffi_type **` 签名调用** ✗；
API 改为不透明指针（§14.14）后即通过 ✓。

**Phase 5 剩余（唯一未完成项）**：把 clux 的 `cfunc <名>(...): T { body }` 定义包装成闭包——
用 `clux_ffi_closure_create` 生成函数指针，handler 内按 §2 签名把 C 参数编组成 clux 值、
调用解释器执行该 `cfunc` 体、再按返回类型回写；并让该名字绑定的值成为"函数指针"而非普通
clux 函数，从而可传给 C 侧（如 `qsort` 的比较函数）。

### 14.16 ✅ Phase 5 收口：语言层 `cfunc_ptr(f)` —— clux 源码产出 C 函数指针

§14.15 的"唯一未完成项"已完成。分两步落地，都在 `tests/ffi_callback_e2e_test.cpp`
（`FfiCallbackE2E`，6 例）里有端到端证据。

#### 14.16.1 宿主侧：闭包 handler 接到 VM 里的 clux 函数（第 56 项）

`clux_ffi_closure_create` 的 handler 内按签名把 C 实参编组成 clux 值 →
`value_call(vm, fn, args, argc)`（公开 API，走完整 `func_vcall`：作用域切换、实参
clone/隐式转换、返回值 clone 回调用方作用域）→ 按返回类型宽度回写 `ret` 槽。

**测试怎么拿到"活着的 VM"**：`driver_run_file` 跑完即销毁 VM，
`driver_compile_to_bytecode` 是 driver 内的 `static`；但 `compile_module` 是公开入口
（`include/driver/compile_module.h`）。因此按 driver 同一顺序手工组装：

```c
alloc = create_allocator(malloc, free);
arena = arena_new_default(alloc);
diag  = diag_buf_new(alloc);
vm    = vm_new(alloc);
entry = module_path_normalize(alloc, path);      /* 源码路径 → 规范路径 */
mod   = compile_module(vm, alloc, diag, arena, entry, path_stack);
fn    = scope_lookup(mod->global_scope, strslice_from_cstr("add"));
value_call(vm, fn, args, argc);                  /* 正常调用 clux 函数 */
```

（`exec_run` 只跑"类型提升区 + 函数注册段"，函数值正是由此登记进模块作用域的。）

#### 14.16.2 语言侧：内建 `cfunc_ptr(f) -> opaque`（第 57 项）

```c
/* src/vm/ffi_cfunc.c */
cfunc add(a: i32, b: i32): i32 { return a + b; }   // cfunc 定义（有体）
func get_cb(): opaque { return cfunc_ptr(add); }   // 取出 C 函数指针
```

- 注册：`vm_register_cfunc_ptr`（`vm.c`，与 `printf`/`upgrade`/`len` 同批）+
  `sema.c` 符号预注册。签名 = variadic + 返回 `opaque`，参数在 cfunc 内部自验
  （与 `len` 同一套路）。源码里 `cfunc_ptr(f)` 与调用普通函数写法一致。
- **可编组类型表**（先过 §2 谓词，再限定本层子集）：
  `i8/i16/i32/i64/u8/u16/u32/u64`、`bool`、`f32`、`f64`、`opaque`。
  §2 允许但本层尚未编组的（`str`/`enum`/`struct`/`cunion`/`own·ref·fatal·share`）
  与 §2 拒绝的（`option`/tag union/切片/元组/闭包/类型值）都给**明确原因**，
  不猜着传参。
- **生命周期（关键设计）**：句柄/上下文/trampoline 地址挂在 **`func_t`** 上
  （`ffi_closure` / `ffi_closure_ctx` / `ffi_closure_code`），由 `func_destroy`
  释放——函数对象本就是 **VM 生命周期**对象（`vm->functions`），与
  `owns_closure_scope`/`closure_scope` 同一模型。于是：
  ① 重复 `cfunc_ptr(f)` 复用同一 trampoline（指针相等，可断言）；
  ② libffi 的可执行内存不会静默泄漏（`delete_allocator` 亦不漏报）；
  ③ 不必给"指针值"造新类型——返回的 `opaque` 值只是 trampoline 地址的**借用视图**。
- **与 §3.2 设计稿的差异**：设计稿是 `ffi_func_t` + 定义时立即建闭包，且 clux 侧调用
  也经 `ffi_call` 走同一个指针（双向统一）；现实现把闭包**惰性**建在既有 `func_t` 上——
  不改函数对象布局、不改 clux 调用路径（clux 调用仍是解释器直调，少一次 ABI 往返）。
  代价是：要拿指针必须显式写 `cfunc_ptr(f)`（不是"名字绑定即指针"）。

#### 14.16.3 三个踩坑（都值得单列）

1. **`ffi_prep_cif` 借用 `arg_types` 数组** —— `cif->arg_types` 存的是**指针**，
   C 侧调用时才解引用，所以数组必须在闭包整个生命周期内有效。
   首版把调用方的数组直接交给 libffi：宿主级用例侥幸通过（数组是测试函数的局部变量，
   调用发生在同一函数内），**语言级用例必崩**（`cfunc_ptr` 的 arg_types 在已被弹出的
   解释器栈帧里）——症状是 `0xC0000005`。修法：`clux_ffi_closure_create` 内
   `malloc` 一份副本存进 box，`destroy` 时释放。**API 契约随之明确**：调用方不必
   让 arg_types 活过 create 返回。
2. **`qsort` 不稳定** —— 语言级用例的比较函数恒返回 0，首版断言"顺序不变"是错的：
   C 标准不要求 `qsort` 稳定，等值元素的重排是实现自由（MSVC 快排实测 5 元素 10 次
   比较后顺序确实变了）。改为断言**元素集合不变** + 闭包内自增的 clux 全局计数
   ≥ n−1（后者才真正证明"C 库调到了 clux 回调"）。
3. **注释坑复发（§14.14 同坑）** —— 新文件头注释里写了行内 `/* ... */`，
   其中的 `*/` 提前闭合块注释，后续中文正文变成代码（`-Wcomment` + 一串 unicode
   错误）。规则：**C 块注释里绝不出现 `*/`**（示例代码用 `//`）。

#### 14.16.4 纯 clux 端到端（最强的一条链）

`cfunc_ptr` + `opaque` 参数 + `ref *T` 解引用组合起来，**不写一行宿主代码**就能让
libc `qsort` 用 clux 比较函数排序（`examples/ffi_callback/ffi_callback.cx`，同时是
`FfiCallbackE2E.PureCluxProgramSortsWithCQsortCallback` 的受检样本）：

```c
extern lib "msvcrt.dll" as crt;
extern cfunc crt::qsort(base: opaque, n: u64, size: u64, cmp: opaque): void;

var CALLS: i32 = 0;

cfunc cmp(a: opaque, b: opaque): i32 {
    CALLS = CALLS + 1;
    var x: i32 = (a as ref *i32).*;   // 等价于 C 的 *(const int*)a
    var y: i32 = (b as ref *i32).*;
    if (x < y) { return -1; }
    if (x > y) { return 1; }
    return 0;
}

func main(): i32 {
    var arr = .[5]i32{4, 2, 5, 1, 3};
    crt::qsort(arr.&, 5, 4, cfunc_ptr(cmp));   // 数组地址 + C 函数指针一起交给 C 库
    if (arr[0] > arr[1]) { return 1; }         // 自检：已升序
    ...
    if (CALLS < 4) { return 3; }               // 自检：C 库确实回调了 clux
    return 0;
}
```

实测输出：`before: .[5]i32 { 4, 2, 5, 1, 3 }` → `after: .[5]i32 { 1, 2, 3, 4, 5 }`，
`qsort called the clux comparator 10 times`，exit 0 ✓。

要点：**clux 有指针解引用**（`.&` 取地址 / `.*` 解引用 / `as ref *T` 转换），
因此回调里能真正取到元素值——设计稿里"只能由 C 侧解引用"的顾虑不成立
（此前是因为 `clux format` 会静默丢弃这些语法，见 §14.16.5）。

#### 14.16.5 顺带修掉的真实缺陷：`clux format` 丢弃 M3 语法（数据损坏）

写上面的例子时发现：`clux format` 输出的文件里，
`var x: i32 = (a as ref *i32).*;` 变成 `var x: i32 = ;`、
`crt::qsort(arr.&, ...)` 变成 `crt::qsort(, ...)` —— **表达式被整个吃掉**。

- 根因：`src/parser/fmt.c` 的 `render_expr` 没有 `AST_ADDR`（`.&`）、`AST_DEREF`（`.*`）、
  `AST_MOVE`（`move`/`clone`）、`AST_PTR`（`own|ref|fatal *T`）、`AST_NEW`、`AST_SCOPE_ANNOT`
  （`'<a,b> T`）的分支，全部落到 `default: break` → 静默不渲染。
  与 M6 前端"漏 fmt 分支 = 静默改写"是同一类缺陷（那次是丢 `extern lib`/`cfunc` 关键字，
  这次是丢整个表达式/类型）。
- 修法：补齐 6 个渲染分支（`src/parser/fmt.c`）。**后缀族必须保住括号**——
  `.&`/`.*` 绑定力最高（26），操作数里优先级更低者要用
  `render_expr_paren(..., 26)`，否则 `(a as ref *i32).*` 会渲染成
  `a as ref *i32.*` 并被重解析为 `a as ref *(i32.*)`（语义改变）。
- 回归：`tests/fmt_test.cpp` 新增 3 例（`.&`/`.*` 渲染、`own *T`/`new`/`move`/`'<a,b>`
  渲染、M3 语法三遍幂等且关键片段不丢）。
- 校准：修好后 `examples/ffi_callback/ffi_callback.cx` 才可进入"所有示例 format 幂等"的
  巡检（38/38）。

#### 14.16.6 验证与边界

- `FfiCallbackE2E` 7/7：C 函数调 clux `add`（可重入）；**libc `qsort`** 用 clux 比较
  函数升序排序；clux 回调内部再调 C（`crt::abs`）；语言级 `cfunc_ptr` → C 调用；
  语言级 `cfunc_ptr` → libc `qsort`（全局计数证明被调用）；**纯 clux 程序用 qsort
  排序**；`?i32` 签名被拒。
- `DriverM6.FfiProgram*` 4/4：FFI 程序从 `.cxb` / `.cxs` 运行；`.cxb → .cxs → .cxb`
  逐字节一致；FFI 声明在**依赖模块**里时同样落盘。
- 全量：`build exit=0`、**0 告警**、**1622/1622**、示例 36/36（0 泄漏，
  且 36/36 `emit → run .cxb` 全通过）、format 幂等 38/38、`.cxs` 往返字节一致。
- **仍不做（有意边界）**：① FFI 实参位置的隐式转换（要显式 `cfunc_ptr(f)`）；
  ② 闭包对 `str`/`enum`/`struct`/指针类的编组（现在明确报"不可编组"）；
  ③ 结构体/变参的 `ffi_call`（`ffi_call_typed` 只做基本类型）。

#### 14.16.7 FFI 声明随容器落盘（.cxb/.cxs 里的 `extern lib`）

**缺陷**：`module_t.ffi_libs` 只由**编译期**扫描 AST 建立，字节码里只剩
`EXTERN_CALL "别名::符号"` —— 加载方无从知道该 dlopen 什么。于是

```
clux bc emit examples/ffi_callback/ffi_callback.cx -o f.cxb && clux run f.cxb
→ run: exec: extern call: no loaded library 'crt' in this module ...
```

源码直跑一切正常，**只有字节码路径断**（与 M5 修过的"`.cxb` 不携带依赖模块"同类）。

**修法**（容器 v2 → v3，见 `include/vm/bcode_graph.h`）：

1. `bcode_module_image_t` 增加 `ffi_aliases` / `ffi_paths` / `ffi_count`（每模块一段）。
2. 二进制段：导出表之后写 `u32 ffi_count` + `[alias_len][alias][path_len][path] × n`
   （`gb_module_section` 唯一实现，vm 路径与图路径共用）。
   顺带修掉一个陷阱：反序列化的模块循环里 `if (ecount == 0) continue;` 会**跳过
   FFI 段**——改成 `if (ecount > 0) { ... }` 后继续往下读。
3. 文本形态：`externlib "<库路径>" as <别名>` 指令（有声明才写，因此**非 FFI 产物的
   `.cxs` 与旧格式逐字一致**，旧手工样本与往返测试不受影响）。
4. 加载侧（`driver_execute_graph` 重建 `module_t` 之后）：按表 `ffi_lib_open` 填
   `mod->ffi_libs`；模块此时已 `vm_module_bind` 进 `vm->modules`，失败路径由
   `vm_destroy → module_destroy` 统一关闭已开的句柄。
5. 兼容：版本 1（单模块）/2（模块表）/3 都能读；v1/v2 按"无 FFI 声明"处理，
   旧产物继续可运行。

**实测**：纯 clux qsort 回调程序 `emit → run .cxb` ✓（`{4,2,5,1,3}` → `{1,2,3,4,5}`、
回调 10 次）；`.cxb → .cxs`（首行 `externlib "msvcrt.dll" as crt`）`→ .cxb`
**逐字节一致**且可运行 ✓；FFI 在**依赖模块**里时 `externlib` 行落在该模块的
`MODULE "./ffi"` 段内 ✓。

### 14.17 ✅ `extern var`：动态库导出的**全局数据符号**

§14.16 打通的是"库里的**函数**"（`extern cfunc`）。本节补上另一半：库里的
**全局变量**。

```c
extern lib "msvcrt.dll" as crt;
extern var crt::_fmode: i32;       // 文件打开模式
extern var crt::_osplatform: u32;  // 平台标识
extern var crt::_pgmptr: opaque;   // 程序路径指针

func main(): i32 {
    var old: i32 = crt::_fmode;    // 读 → EXTERN_LOAD
    crt::_fmode = 32768;           // 写 → EXTERN_STORE（左值）
    crt::_fmode += 1;              // 复合赋值 → LOAD + 运算 + STORE
    crt::_fmode = old;             // 还原
    return 0;
}
```

#### 14.17.1 语义要点

- **数据符号的"符号地址"就是变量地址**：`dlsym`/`GetProcAddress` 对数据导出返回的
  就是那块存储的首地址，因此读写**直接作用于库里的内存，没有本地副本**
  （`readelf`/`dumpbin /exports` 里导出项形如 `td_i32 = td_i32`）。
- **它是左值**：`lib::name = v` 合法（对比 `extern cfunc` 只能调用）。这与
  `const`/comptime 常量形成对照——后者在 sema 层就拒绝赋值。
- **没有初始化器**（`init = NULL`）：库里已经有内存了，`extern var x: T = v;`
  是语法错误。
- **类型必须显式给出**且属于可编组子集：`i8/i16/i32/i64`、`u8/u16/u32/u64`、
  `bool`、`f32/f64`、`opaque`（指针宽度）。**宽度必须与库里符号的真实类型一致**
  ——读多/写多会踩到相邻符号，所以 §2 允许但布局不定的类型（`str`/`struct`/
  `enum`/切片/指针所有权类型）在这里**明确报错**而不是猜着读写。

#### 14.17.2 实现链路（与 `extern cfunc` 对称）

| 层 | 改动 |
|---|---|
| AST | `ast_var_def_t` 增 `is_extern_var` + `lib_name`；`ast_enum_ref_t` 增 `is_ffi_var` + `ffi_var_kind`（与 `ast_call_t.is_ffi_call`/`ffi_ret_kind` 同一契约：sema 填、compiler 读） |
| parser | `parse_extern_var`：`extern var <库>::<名>: <T>;`（无体、无初始化器）；赋值左值放行 `AST_ENUM_REF`（**语义判定交给 sema**：只有 extern var 可赋值，enum variant / 模块成员 / 函数各给精确诊断） |
| fmt | `extern var lib::name: T;` 原样渲染（**漏了就是静默改写**：会被格式化成普通 `var … = …`） |
| sema | ① `pass1_names` 注册限定名符号 `"<库>::<名>"`（新符号种类 `SEMA_SYM_EXTERN_VAR`）+ 别名存在性与类型子集校验；② `ex_enum_ref` 的 `::` 分支：命中 extern var → 置 AST 标志 + 返回声明类型 shadow（读取）；③ `shadow_assign` 新分支 `shadow_assign_extern_var`：类型校验 + 置标志（写入/复合赋值） |
| compiler | `AST_ENUM_REF`（is_ffi_var）→ `EXTERN_LOAD <"lib::name"> <kind>`；`AST_ASSIGN` 目标同标志 → `EXTERN_STORE`（复合赋值展开 LOAD→op→STORE） |
| VM | `op_extern_load` / `op_extern_store`；与 `op_extern_call` 共用 `ffi_resolve_symbol`（拆 `lib::name` → 库表 → dlsym，诊断带库名/符号名与 OS 文本） |
| driver | 编译期对每个 `extern var` 做 dlsym 校验（与 `extern cfunc` 同一循环）。**顺带修掉一个真实缺陷**：符号解析失败时同一条诊断被打印两次（重复的 `diag_error`），已合并 |
| 编码 | `ffi_data_kind_of`（类型→码）与 `ffi_data_type_of`（码→类型+宽度）**成对实现在 `vm/ffi_lib.c`**，避免两处清单漂移 |

`kind` 码：`0..3` = i8/i16/i32/i64、`4..7` = u8/u16/u32/u64、`8/9` = f32/f64、
`10` = opaque（指针宽度）、`11` = bool。

#### 14.17.3 两个真踩到的坑

1. **bool 不能复用 u8 的类型码** —— 首版把 `bool` 映射成 `BCODE_FFI_DATA_U8`
   （宽度一样），但 `EXTERN_LOAD` 会构造出 **u8 值**，于是 `crt::flag != true`
   报 `run: type does not support implicit cast`（类型不匹配，不是值错）。
   修法：`bool` 单独一码，decode 出 `vm->type_bool`。
2. **`FreeLibrary` 会卸载模块、导出变量复位** —— 测试夹具原先每次读取都
   `ffi_lib_open` + `ffi_lib_close`，引用计数归零导致 DLL 被卸载，下一次
   `LoadLibrary` 拿到**全新副本**（初值），于是"clux 写进去的值"在宿主侧读不到
   （用例内部读回却是对的，极易误判成实现有 bug）。修法：夹具持有一个长期句柄
   （真实程序里库表由 `module_t` 持有到模块销毁，不存在这个问题）。
   **教训**：观察"外部库的状态"时，必须先确认模块仍然驻留。

#### 14.17.4 测试与验证

- 新增 `tests/ffi_testdata.c`（自建极小动态库：12 个导出变量覆盖全部宽度）——
  系统库里没有这些宽度的可写数据符号；CMake 加 `clux_ffi_testdata` SHARED 目标，
  产物与 `clux_test.exe` 同目录（Windows 加载器先搜可执行文件所在目录，测试用
  裸文件名即可打开）。
- 新增 `tests/ffi_extern_var_test.cpp` 13 例：字节码级（i32/u64/f64 读取、
  指针宽度→opaque 且与宿主 dlsym 地址相同、写入后宿主独立复核、bool 类型、
  4 类诊断）；源码级（全宽度读取、写入+还原、复合赋值、opaque 往返、
  `.cxb`/`.cxs` 往返且反汇编保留 `EXTERN_LOAD "…" <kind>`、6 类诊断）。
- `examples/ffi_extern_var/ffi_extern_var.cx`：真实系统库里读/写 `_fmode`
  （写完还原）、`_osplatform`、`_pgmptr`；源码运行与 `emit → run .cxb` 均 exit 0。
- 全量：`build exit=0`、**0 告警**、**1635/1635**。

### 14.18 ✅ 特殊外部模块 `"*"`：当前进程自身（RTLD_DEFAULT 语义）

libc 这类库在进程启动时就已链接进来，**不需要也不该**再 `dlopen` 一次。把
`extern lib` 的**路径写成 `"*"`** 就声明了一个"当前进程"伪库：

```c
extern lib "*" as c;              // 没有文件被加载：这是"进程自身"
extern func  c::abs(x: i32): i32; // extern func 与 extern cfunc 等价
extern cfunc c::strlen(s: str): u64;
extern var   c::_osplatform: u32; // 进程里的数据符号同样可读

func main(): i32 {
    if (c::abs(-7) != 7) { return 1; }
    if (c::strlen("hello") != 5u64) { return 2; }
    return 0;
}
```

#### 14.18.1 语义与实现

- **解析语义 = `dlsym(RTLD_DEFAULT, name)`**：在**全局作用域**里找符号
  （可执行文件及其依赖、以及 `RTLD_GLOBAL` 加载的库）。
  Windows 没有 `RTLD_DEFAULT`，等价实现是"**先主模块、再遍历已加载模块**"
  （`ffi_sym_current_process`：`GetModuleHandle(NULL)` → `K32EnumProcessModules`
  / `EnumProcessModules`（都动态取，不给端口加链接依赖）→ 逐个 `GetProcAddress`）。
  **只查主模块是不够的**——`abs`/`strlen` 由 CRT 提供，在已加载模块里。
- **实现方式：伪库句柄**（不是新的语法别名）。`ffi_lib_open(alloc, "*")` 返回一个
  `is_process = true`、**没有 OS 句柄**的 `ffi_lib_t`；`ffi_lib_sym` 见 `is_process`
  就把请求转交 `ffi_sym_current_process`。于是：
  - 语言层**零特殊语法**：别名照常起、`<别名>::<名>` 照常引用，与显式加载文件走
    完全同一条编译/运行通路（sema 的别名符号、compiler 的 `EXTERN_CALL`/`EXTERN_LOAD`
    都不需要知道 `*` 的存在）；
  - 容器（v3 的 `externlib "*" as c` 段）在加载侧重新 `ffi_lib_open("*")` 即可复原，
    不需要任何额外字段；
  - `ffi_lib_close` 对无句柄的伪库自然安全（`if (handle)` 才 FreeLibrary/dlclose）。
- **判据仍是关键字 + 路径**：`"*"` 是**路径**的取值之一（另一个是任意文件名），
  与"按后缀判类"无关——`extern lib "*.dll"` 之类仍然只是普通路径，交给加载器判定。

#### 14.18.2 顺带修掉的两个真缺陷（都由 `*` 用例暴露）

1. **返回类型不精确**（`ret_kind` 只有 5 个值）：`u64` 返回被造成 **i64** 值，于是
   `if (c::strlen(s) != 5u64)` 在运行期撞上 "i64 vs u64" 的隐式转换，报
   `implicit cast: not a widening conversion`（症状在比较点，根因在调用层）。
   修法：把 `ret_kind` 补成**逐类型**编码（`vm/bcode.h` 的 `BCODE_FFI_RET_*`：
   0..4 保持 v0 原义以兼容旧容器，新增 u32/u64/i8/i16/u8/u16/bool），VM 按码造出
   **与声明一致的类型**。`f32` 返回**明确拒绝**（浮点通路是全 double 签名，f32
   会按 double 读错 XMM0 宽度）——在**声明处**报错，而不是造个 f64 值冒充 f32。
2. **实参完全不校验**：FFI 调用点此前不检查数量与类型，`c::abs(1.5)` 会把 double
   塞进整数寄存器而**毫无提示**。修法：在 `ex_call` 的 FFI 分支按**调用层实际能
   做的事**校验（§14.5 只有"int64 槽"与"全 double 槽"两条通路）：
   - 数量：固定参数之外只允许 variadic 的多余实参；
   - 整数族（int/bool/enum）形参 ⇄ 整数族实参（同走 int64 槽，语义同 C 隐式转换）；
   - `opaque`/指针形参 ⇄ 指针/opaque/slice/str 实参；`str` 形参 ⇄ `str` 实参；
   - 浮点形参只在**浮点签名**里出现；非浮点签名里出现浮点实参 → 报错
     （否则会被塞进整数寄存器）；variadic 的浮点额外实参同样报错（C 要求走 XMM）。

#### 14.18.3 测试与验证

- 新增 `tests/ffi_process_test.cpp` 10 例：
  字节码级（伪库句柄 `ffi_lib_open("*")`；`c::strlen` 返回 **u64 类型**、`c::abs`
  返回 i32、`EXTERN_LOAD "c::_fmode"` 读到进程里的数据符号、未知符号报错）；
  源码级（不加载任何文件的完整程序；`.cxb`/`.cxs` 往返且 `.cxs` 里保留
  `externlib "*" as c`、逐字节一致；`extern func` ≡ `extern cfunc` 且 fmt 归一化；
  数据符号写入后宿主侧可见并还原）；缺陷回归（u64 返回类型；数量/浮点/混合签名/
  f32 返回/符号不存在 5 类诊断）。
- 新增 `examples/ffi_process/ffi_process.cx`：只用 `extern lib "*"` 调 `abs`/`strlen`
  并读 `_osplatform`；源码运行与 `emit → run .cxb` 均 exit 0。
- 全量：`build exit=0`、**0 告警**、**1645/1645**、示例 38/38、format 40/40。

### 14.19 ✅ 删除内建 printf，改用 libc 的原生 printf（并因此补齐精确类型调用）

**动机**：M1 把 C 的 printf **硬编码**成内建函数（`builtin_printf.c`），自己解析格式
串转发给 `printf(spec, ...)`，还加了一个 `%v`"打印任意值"的扩展（`.type { value }`
形态，靠 `value_dump` 实现）。这是把 C 库函数改装成语言内建：格式语义要自己跟 C 保持
一致、`%v` 成了只有 clux 才有的方言、诊断/边界（%s 必须是 str 等）也都得自己维护。
M6 有了 FFI（调用 + 数据符号 + 回调 + 当前进程库 `"*"`）之后，正确的做法是**让它回到
它本来的身份**：libc 里的一个普通函数。

#### 14.19.1 语言层：显式声明，零预置

不做任何"编译器预置 printf"的魔法。源码自己声明（别名用 `libc` 而非 `c`——
`::` 左侧先按作用域解析，**局部变量会遮蔽库别名**）：

```c
extern lib "*" as libc;                              // 当前进程自身
extern func libc::printf(format: str, ...): void;    // 返回值用不到就声明 void
```

- **为什么声明成 `void`**：clux 的表达式语句不允许丢弃非 void 结果
  （`expression result of type i32 is unused; use '_ = expr'`）。C 的 printf 返回
  写入字符数；只做输出时声明 `void` 最自然（ABI 上返回槽被忽略，无副作用），
  需要计数就声明 `i32` 并写 `var n = libc::printf(...)`。
- **`%v` 随之消失**：格式符就是 C 的原生集合。聚合值要打印就逐个成员打印
  （示例里改成了 `%d %d %d %d %d` 逐个打印数组元素）。`value_dump`（`%v` 的实现）
  保留为独立工具（当前只有单测使用，将来 REPL/调试器会用到）。
- 内建函数只剩 `upgrade` / `len` / `cfunc_ptr`（id 按注册顺序分配，**不要写死数字**）。

#### 14.19.2 引擎层：为"真 printf"补齐两件事

删除内建后，printf 走通用的 `EXTERN_CALL`，因此必须支持它的两个特征：

1. **C 变参（`...`）** —— 新增 `ffi_call_typed_items`（`vm/ffi_lib.c`）：
   `nfixed == argc` 走 `ffi_prep_cif`，`nfixed < argc` 走 **`ffi_prep_cif_var`**
   （x64 变参要按需把寄存器实参复制进 shadow store）。
   固定参数个数与"是否变参"打包进 `EXTERN_CALL` 的 **ret_kind 操作数高位**
   （`BCODE_FFI_RET_PACK`：bits0..7 返回类型码、bit8 变参、bits16..31 nfixed）——
   不新增操作数，旧 `.cxb`/`.cxs`（高位恒 0）因此天然读作"非变参" ✓。
2. **C 默认实参提升** —— 变参位置由调用方（VM）施加：`f32 → double`、
   窄整型/`bool` → `int`（提升结果写进 8 字节临时槽）；32/64 位整数与指针已是寄存器
   槽宽度，原样传。定参**不需要**提升：clux 的 `value->data` 与 C **同构**
   （编译器保证平凡布局），直接把 `value_data(v)` 交给 libffi，它按描述符读
   `sizeof` 字节即可。

顺带把**类型映射补成完整的 1:1**（用户要求："基础数据类型可以完整的和 C 1:1 转换"）：

- 调用统一走 libffi 的**精确描述符**（`ffi_data_ffi_type`：i8..i64、u8..u64、
  f32、f64、bool、pointer…），不再有"全 double 签名"这类人为限制：
  **f32 参数与返回值都支持**，混合整数/浮点签名也支持。
- 返回类型码 `BCODE_FFI_RET_*` 补到逐类型（新增 f32；`f32` 返回不再是错误）。
- 实参描述符按**值的类型**取（整数族宽度/符号、浮点同型、`str`/指针/`opaque` → pointer、
  `enum` → 底层整数、const/volatile 先剥限定符）；sema 侧的校验就是"能否共用同一个
  描述符"：整数族 ⇄ 整数族（同 C 隐式转换）、浮点**必须同型**、指针族 ⇄ 指针族、
  `str` ⇄ `str`（结构体/cunion 见 §14.20：按字段构造描述符后同样支持）。
- libffi 的 `ret_type` **不能为 NULL**（会直接崩），调用层用 `NULL = void` 约定并补
  `ffi_type_void`——这是本轮的崩溃根因，值得记一笔。

#### 14.19.3 一个真踩到的坑：FFI 实参求值必须经**真正的 AST 槽**

给 FFI 调用点加实参校验时，最初写成 `for (a = call->args; ...) sema_expr(sema, &a, ...)`——
`&a` 是**局部副本**，而 comptime 折叠是"就地替换节点"（`add(1,2)` → 字面量 `3`），
写在副本上就丢了：编译器随后仍去编译未折叠的调用，运行期报 `exec: undefined variable`
（`PUSH "add"` 找不到函数——comptime 函数不进运行时）。改为沿真实槽遍历
（`for (slot = &call->args; *slot; slot = &(*slot)->next) sema_expr(sema, slot, ...)`)
即可。**规则：任何可能改写 AST 的求值都必须拿到真正的节点槽。**

#### 14.19.4 影响面与验证

- 删除：`src/vm/builtin_printf.c`（整文件）、`vm_register_printf`、sema 的 printf
  预注册；内建 id 顺移（upgrade=0/len=1/cfunc_ptr=2）。
- 示例 36 个文件、测试 10 个文件改用 `libc::printf`；示例用别名 `libc`（避开变量名
  遮蔽），测试统一经 `tests/test_common.h` 的 `clux_with_printf_prelude()` 在写文件时
  补声明（只对含 `libc::printf(` 的片段生效，格式化类测试不需要）。
- 两个"内建函数值"用例改用 `len`（printf 不再是内建，`extern cfunc` 也没有运行期
  函数值，只有调用点语义）。
- 验证（强制全量重编）：`build exit=0`、**0 告警**、**1645/1645**、
  示例 **38/38**（0 泄漏）且 38/38 `emit → run .cxb` 全通过、format 幂等 40/40。
  实测原生 printf：`%d %s %.2f %c %x %f %llu`、0/1/2/5 个变参、`u64` 变参
  （`18000000000000000000`）、`i8` 提升（`-7`）、`f32` 返回（`sqrtf`）全部正确。

### 14.20 ✅ 聚合类型完备：struct / cunion / enum / char*（含 FFI 签名里的程序自定义类型）

**动机（用户提问："M6 彻底完备了吗，兼容数据类型包括结构体都支持了吗"）**：用探针逐个
类型试出来的边界是——标量/指针/opaque 早已支持，但 `str` 只能传参不能返回、
**enum 与 struct 连声明都用不了**（报 `missing type`）、`extern var` 只认标量/指针、
回调只编组标量。其中 `missing type` 不是"没实现"而是**一个真缺陷**：FFI 签名校验跑在
`pass1_names`，那时程序自定义类型只是"登记"未"激活"，`resolve` 得到 NULL。连
"传结构体指针"（`ref *P`）这条标准做法都被它挡住。

#### 14.20.1 修缺陷：FFI 签名校验挪到程序类型激活之后（pass1c）

- `pass1_names`：只登记**类型已就绪**（纯内建标量签名）的 extern 符号，不报诊断；
  未就绪的（含程序自定义类型）留白。
- **新增 `pass1c_extern_abi`**（在 `pass1b_types` 之后、`pass2_types` 之前）：
  统一 resolve + §2 校验 + 补登记。`check_extern_cfunc` / `check_extern_var` 两个
  助手 `report=false/true` 复用同一份逻辑，避免同一错误报两次。
- 未知类型的诊断从 `missing type` 改成 `type could not be resolved`。
- 效果：`struct`/`cunion`/`enum`/`type` 别名/`ref *P` 都能写进 FFI 签名 ✓

#### 14.20.2 类型怎么跨层传：**类型值随指令走**（不引入类型 id 操作数）

`EXTERN_CALL` / `EXTERN_LOAD` / `EXTERN_STORE` 的操作数个数是**冻结**的
（旧 `.cxb`/`.cxs` 按固定个数解析）。程序自定义类型的宽度/布局又必须运行期查表。
解法是复用 `CONSTRUCT` 已有的机制：**compiler 在指令之前 `compile_type_expr` 压一个
"类型值"**，运行期 pop 出来当类型用。

- 返回：新码 `BCODE_FFI_RET_TYPED`（struct/cunion/enum/带类型的指针）；
- 数据符号：新码 `BCODE_FFI_DATA_TYPED`（struct/cunion/enum）；
- `str` 各自有码（`BCODE_FFI_RET_STR` / `BCODE_FFI_DATA_STR`），无需类型值；
- 变参信息（是否变参 + 固定参数个数）仍在 ret_kind 高位，本轮把 nfixed 从
  bits16..31 挪到 **bits9..15**（旧产物这些位恒 0，语义不变 ✓）。

#### 14.20.3 描述符构造：`ffi_type_for`（递归）

`vm/ffi_lib.c` 新增 `ffi_type_for(t, vm, &owned, err, ecap)`：

- 标量/`str`/指针/`opaque` → libffi 的**静态**描述符（`out_owned=false`）；
- `enum` → 递归到**底层整数**；
- `struct` → 按字段顺序递归新建 `ffi_type{FFI_TYPE_STRUCT}`（elements 以 NULL 结尾）；
- `cunion` → libffi **没有** union 类型：等价建模成 **size/align 个 align 宽度的
  整数成员**（`struct { intN a; intN b; … }`）。union 的 size 必是 align 的整数倍
  （`size = align_up(max member, align)`），所以这样建出来的 size/align 与真 union
  完全一致，x64 的 ABI 分类（INTEGER/SSE/MEMORY）与寄存器/栈占用因此相同 ✓
- 新建的描述符用 `malloc` 分配、`ffi_type_release` 释放（与闭包 box 同一约定：
  libffi 相关内存不走项目 allocator）。调用点用完即释放；闭包把它们挂到
  `cfunc_ctx_t`（新增 `dispose_fn`）活到闭包销毁——libffi 的 `cif` 持有这些指针。

**实参编组零特判**：clux 的 `value->data` 与 C 同构（编译器保证平凡布局），
所以 `ffi_arg_desc` 直接把 `value_data(v)` 交给 libffi，结构体就是那块 C 布局字节。
回调方向同理（handler 按参数类型的 size 拷贝 libffi 给的指针）。

#### 14.20.4 sema：类型可接受性

- 参数：整数族⇄整数族、浮点**同型**、`str`⇄`str`、指针族⇄指针族、
  **struct/cunion 同型**（描述符按字段构造，布局必须完全一致）；
- 变参：整数族/浮点/指针/`str`/**struct/cunion** 都可以（C 对聚合类型不做默认提升）；
- 返回：标量/`opaque`/`str`/struct/cunion/enum/指针族（其余仍报
  `return type is not marshalable`）；
- `extern var`：标量/指针/`str`/struct/cunion/enum（`option` 等明确拒绝）。
- 顺带把 `FFI_MAX_ARGS` 从 8 提到 **16**（真 printf 很容易超 8 个实参）。

#### 14.20.5 测试与验证

- `tests/ffi_testdata.c` 增补：`CluxPoint`(8B INTEGER)、`CluxDPoint`(16B SSE)、
  `CluxBig`(24B MEMORY)、`CluxNested`(嵌套)、`CluxColor`(enum)、
  `td_point`/`td_color` 数据符号、`clux_greeting`(char*)、
  `clux_call_point_cb`(struct 回调)、`clux_point_va_sum`(变参 struct) 等导出。
- 新增 `tests/ffi_struct_test.cpp` 11 例（判定方式：clux 程序把检查结果写进库里的
  `td_i32`，宿主侧读回——`driver_run_file` 的返回值只表示"编译并运行成功"，
  **不代表 main 的返回值**）：三种 ABI 类别的按值传参/返回、嵌套结构体、
  指向结构体的指针、变参里的结构体、struct 数据符号读写（宿主侧复核）、
  enum 传参/返回/数据符号、char* 返回（用 libc `strcmp` 比对内容）、
  struct 回调（C 按值传给 clux 函数并接收其按值返回的 struct）、
  容器往返（`emit → run .cxb`）、程序自定义类型签名（别名 + 指针通过 / 未知类型报错）。
- 全量：`build exit=0`、**0 告警**、**1668/1668**、示例 40/40、format 42/42。

### 14.21 ✅ `data(s)`：从序列取首元素指针（C ABI 的 `(T*, size_t)` 适配）

**动机**：C 的接口几乎都是 `(T *ptr, size_t n)`，而 clux 的序列是**胖指针**（slice
带 len、数组自带长度、str 自带 NUL）。两边对接需要"把首元素地址取出来"：

```c
extern cfunc libc::memcmp(a: opaque, b: opaque, n: usize): i32;
var a = .[4]i32{7, 7, 7, 7};
_ = libc::memcmp(data(a), data(a), sizeof(i32) * 4usize);   // (ptr, len) 与 C 同形
```

#### 14.21.1 语义

`data(s) -> ref *T`（T = 元素类型；str → `u8`，即 C 的 `const char *`）：

| 序列 | 结果 |
|---|---|
| `own/ref/fatal []T` | 指向切片首元素（切片自带缓冲区） |
| `[N]T` | 指向数组值自身的数据块（借用，随该值生命周期） |
| `str` | 指向首字节；空串给的是**有效的**空串指针（不是 NULL） |

恒 `ref`（借用、永不拥有，dispose 跳过）；空切片可能得到 NULL（C 侧不得解引用，
用 `len(s) == 0` 判空）。与 `len(s)`（= usize = C 的 `size_t`）配对。

#### 14.21.2 关键决策：它是**指令**（`BCODE_DATA`）而不是内置函数

第一版实现成内置 cfunc，实测**悬垂**：内置函数的实参会被 `value_call` **clone 进被调
作用域**（这是既有语义，保证内置改不到调用方的值），所以函数形态拿到的是**副本**的
地址——slice 缓冲区副本、数组副本块，调用一结束就失效（读回 `0xDDDDDDDD` 的填充值）。
"取存储身份"必须像 `x.&`（ADDR）那样**在调用点就地完成**：编译器压序列（`op_push`
给的是借用视图，`value_data` 就是调用方的存储），新指令从中读出真实的 ptr 字段。

- 新操作码 `BCODE_DATA` 追加在末尾（不动既有编号）＋ `.cxs` 助记符 `DATA`；
- sema 侧仍按内建名解析（符号标记见下），把返回类型精化为 `ref *<元素类型>` 并置
  `ast_call_t.is_data_call`；compiler 据此改发指令（**不是** CALL）；
- 运行期 `op_data`：按值的类型取 `slice_view().ptr` / 数组的元素块 / str 的 char*，
  再 `type_ptr_intern(PtrRef, elem)` + `ptr_make_value` ✓。

#### 14.21.3 顺带修掉的两个真缺陷

1. **内建特判从未生效**（老缺陷）：`ex_call` 求值 callee 时 AST_IDENT 会被折叠成
   `FUNC_REF`，而 `upgrade` 的返回类型精化与 `len` 的实参校验都是"按 callee 名字 +
   `AST_IDENT`"匹配的 → **一直是死代码**（`upgrade` 因此只能靠 `?share *opaque` 的
   隐式转换侥幸通过；`len(5)` 也不会被拦）。修法：在 `sema_symbol_t` 上加
   **`builtin_op` 标记**（`SEMA_BUILTIN_{LEN,UPGRADE,DATA,CFUNC_PTR}`），注册时打标、
   调用点按标记匹配——与节点形态、名字、内建 id 顺序都无关。这正是"不该用 fid 判内建"
   的第二次教训（第一次是 printf 从内建删除后 `fid==1` 失效）。
2. **空字符串字面量 panic**：`""` 触发 `arena: zero-size allocation`
   （`parse_string_lit` 对 `out_len == 0` 直接 `arena_alloc(0)`）。修法：空字面量分配
   1 字节写 `'\0'`（长度仍 0）——顺带让 `data("")` 给出**有效**的空串指针而不是 NULL，
   对 C 侧更正确。

#### 14.21.4 边界（有意保留）

- clux **没有指针算术**：`p[i]`（p 为 `ref *i32`）是编译错误
  （`cannot index value of type ref i32`）。指针只用于喂 C；clux 侧遍历用序列下标，
  或只解引用首元素 `p.*`。
- 借用有效期 = 序列值的生命周期；把 `data()` 的结果存起来后让序列先死，就是悬垂
  （与 `x.&` 同一约定，C 里也一样）。

#### 14.21.5 测试与验证

- `tests/ffi_testdata.c` 增补 C ABI 形态的导出：`clux_sum_i32(const int32_t*, int32_t)`、
  `clux_first_i32`、`clux_fill_i32`（C 通过指针**写回**调用方缓冲区）、
  `clux_str_first_byte(const char*)`。
- 新增 `tests/data_builtin_test.cpp` 7 例：**悬垂回归**（C 写回后 clux 侧看得见 →
  证明是同一块内存而不是副本）、slice/ref-slice/array 的 `(ptr,len)` 求和、
  `data(str)` 的 char*、结果类型精确为 `ref *T`（含元素类型随序列变化）、
  诊断（非序列实参 / 参数个数）、空切片（len 0 + C 侧安全返回 0）。
- `examples/ffi_data/ffi_data.cx`：`data`+`len` 与 `memcmp`/`strlen` 对接，
  并演示 `p.*` 与"clux 侧遍历用序列"的边界。
- 全量：`build exit=0`、**0 告警**、**1668/1668**、示例 **40/40**、format **42/42**。