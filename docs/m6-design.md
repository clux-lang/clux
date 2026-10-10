# clux M6 FFI 系统设计

## 设计原则

clux 采用标准 C ABI，通过 `cfunc` 和 `extern cfunc` 两个关键字实现与 C 代码互操作。
运行期动态调用使用 libffi（git submodule 引入），无需手写 trampoline。

---

## 1. 语法

### 1.1 动态库导入

```
import libc from "libc.so.6";       /* Linux */
import libc from "libc.dll";         /* Windows */
import libc from "libobjc.dylib";   /* macOS */
```

- 路径以 `.so` / `.dll` / `.dylib` 结尾的是**动态库导入**
- M5 parser 已支持，sema 报错；M6 实现 `dlopen`/`dlsym`（或 `LoadLibrary`/`GetProcAddress`）
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
