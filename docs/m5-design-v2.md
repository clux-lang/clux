# clux M5 模块系统重新设计（v2）

## 设计原则

在错误的基础上重新设计，不打补丁。六条核心决策：

1. **每模块独立走完整流水线** — lex → parse → sema → compile，产出 module 对象（bytecode + global_scope + exports）
2. **import 同步递归编译** — 遇到 import 立即递归执行被导入模块的完整流水线；循环依赖用路径栈 vec 检测
3. **sema 是流水线中的一环** — 不存在子 sema，sema 用完即销毁，产物转移给 module 对象
4. **全局表由 vm 持有** — strtable / functions / types / func id 统一在 vm 层，跨模块共享
5. **模块以规范路径为 key 存于 vm** — `vm->modules` 是 `path → module` 映射
6. **module 的 global_scope 被函数引用** — 跨模块调用切换 global_scope，导出函数可访问模块私有全局

---

## 1. module 对象

```c
typedef struct module_t {
    bytecode_t       *bc;            /* 模块字节码（注册段 + 函数体） */
    scope_t          *global_scope;  /* 模块全局作用域（运行期函数/变量绑定来源） */
    strmap_t         *exports;       /* name → value_t* 借用映射（指向 global_scope 中的符号） */
    char             *canonical;     /* 规范路径（key，owned） */
} module_t;
```

- `bc`：该模块自己的字节码，自包含（函数注册段在 PC=0，函数体在后）
- `global_scope`：运行期该模块所有全局变量/函数/类型的绑定位置。DEFINE 指令把函数/var 绑定到这里
- `exports`：导出表，name → value_t* 借用映射。GET_MEMBER 从这里取成员。非导出的符号不在此表中
- `canonical`：规范路径，作为 vm->modules 的 key

## 2. vm 变更

### 2.1 新增 modules map

```c
/* 替换现有 modules_by_path */
strmap_t *modules;  /* 规范路径 → module_t*（vm 拥有生命周期） */
```

### 2.2 全局表已存在，确认跨模块共享

现有 vm 字段天然支持多模块共享：
- `functions` / `functions_by_id` — 函数池统一在 vm
- `types_by_id` — 类型表统一在 vm
- `strs` — 字符串池统一在 vm
- `sig_types` / `const_types` / ... — 各类 intern 类型池统一在 vm

### 2.3 func id 统一分配

func id 由编译器在 vm 层统一分配（现有 `functions_by_id` 机制不变）。
多个模块的编译器共享同一 vm，id 全局唯一。

### 2.4 root_scope 语义调整

```c
scope_t *root_scope;  /* 当前模块的全局作用域（执行该模块 bcode 时设为该模块的 global_scope） */
```

- 不再是 vm 创建时固定的一个 scope
- 执行某模块的 bcode 时，`vm->root_scope = module->global_scope`
- `func_vcall` 已有 `vm->root_scope = fn->root_scope` 切换逻辑——天然支持

## 3. bcode_function_t 变更

```c
typedef struct bcode_function_t {
    func_t    base;
    uint32_t  entry_pc;
    bytecode_t *bc;  /* 新增：函数所属模块的字节码 */
} bcode_function_t;
```

`bcode_call_cfunc` 改为：
```c
value_t *r = exec_drive(vm, bfn->bc, bfn->entry_pc);  // 用函数自己的 bc
```

这解决跨模块调用的根本问题——每个字节码函数知道自己属于哪个 bcode，
调用时切换到正确的 bcode 执行。

## 4. 编译流水线

### 4.1 入口模块

```
driver_run_file(path):
  vm = vm_new(alloc)
  module = compile_module(vm, alloc, path, path_stack)
  // 执行注册段
  exec_run(vm, module->bc)
  // 检查 main 导出
  // value_call(main)
```

### 4.2 compile_module（每模块统一入口）

```
compile_module(vm, alloc, canonical_path, path_stack):
  // 1. 循环依赖检测
  if canonical_path in path_stack:
    error("circular dependency: " + path_stack chain)
    return NULL

  // 2. cache 查找（已编译直接返回）
  module = vm->modules[canonical_path]
  if module: return module

  // 3. push 到路径栈
  path_stack.push(canonical_path)

  // 4. lex → parse
  pool = lex(canonical_path)
  ast  = parse(pool)

  // 5. sema（独立创建，用完即销毁）
  sema = sema_new(vm)  // 借用 vm 的全局表
  sema_analyze(sema, ast)  // 遇到 import 递归 compile_module
  scope_tree = sema->scope_tree
  types      = sema->types
  exports    = sema_build_exports(sema)  // name → value 借用映射

  // 6. compile
  compiler = compiler_new(alloc, vm, diag, pool, scope_tree, types)
  bc = compiler_compile(compiler, ast)
  compiler_destroy(&compiler)

  // 7. sema 销毁（产物已转移：scope_tree → module, types 已在 vm, bc 已生成）
  //    注意：scope_tree 不随 sema 销毁，转移给 module
  sema_destroy(&sema)  // 只释放 sema 自身（funcs 等），不释放 scope_tree

  // 8. 构建 module 对象
  module = { bc, scope_tree as global_scope, exports, canonical_path }

  // 9. 注册到 vm
  vm->modules[canonical_path] = module

  // 10. pop 路径栈
  path_stack.pop()

  return module
```

### 4.3 import 语句在 sema 中的处理

sema 分析到 `import std from "./std"` 时：
1. 规范化路径（base_dir + "./std" → "C:/.../std.cx"）
2. 调用 `compile_module(vm, alloc, "C:/.../std.cx", path_stack)` — 同步递归
3. 拿到 module 对象，从 `module->exports` 中获取导出成员类型信息
4. 在当前模块的全局作用域注册 `std` 为 MODULE 符号，关联 module 对象
5. sema 后续遇到 `std::add` 时，从 module->exports 查 `add` 的类型

## 5. 运行期执行

### 5.1 模块注册段执行

每个模块的 bcode PC=0 开始是注册段（DEFINE/BIND_FUNC 等）：
```
for each module in dependency order:
  vm->root_scope = module->global_scope
  vm->bc = module->bc
  exec_run(vm, module->bc)
```

注册段把函数/var 绑定到 `module->global_scope`（而非统一的 root_scope）。

### 5.2 IMPORT 指令

```
op_import:
  path = bcode_read_str(bc, pc)
  module = vm->modules[path]   // 查 map
  if !module: error
  push module_value(module)    // 压模块值（data = module_t*）
```

### 5.3 GET_MEMBER 指令

```
op_get_member:
  name = bcode_read_str(bc, pc)
  mod_val = pop()
  module = (module_t*)value_data(mod_val)
  member = module->exports[name]  // 从模块导出表取成员
  if !member: error("no exported member 'name'")
  push member
```

### 5.4 跨模块函数调用

```
std::add(3, 4):
  IMPORT "C:/.../std.cx"     → 压 std 模块值
  GET_MEMBER "add"           → 从 std->exports 取 add 函数值
  PUSH_I32 3
  PUSH_I32 4
  CALL 2                     → value_call → func_vcall

func_vcall:
  vm->root_scope = fn->root_scope  // 切换到 std 模块的 global_scope
  bcode_call_cfunc:
    exec_drive(vm, bfn->bc, bfn->entry_pc)  // 用 std 模块的 bcode 执行
  恢复原 root_scope
```

## 6. 生命周期与所有权

```
vm
├── modules (strmap: path → module_t*)
│   └── module_t
│       ├── bc (bytecode_t*, vm_destroy 释放)
│       ├── global_scope (scope_t*, vm_destroy 释放)
│       ├── exports (strmap_t*, 借用 global_scope 中的 value)
│       └── canonical (char*, vm_destroy 释放)
├── functions (vec: func_t*/bcode_function_t*)
│   └── bcode_function_t
│       ├── base.root_scope → 某个 module->global_scope（借用）
│       └── bc → 某个 module->bc（借用）
├── types_by_id, sig_types, ... (跨模块共享)
└── strs (跨模块共享)
```

- module 对象归 vm 拥有，vm_destroy 统一释放
- sema 用完即销毁，不持有任何需要传递给 module 的资源（scope_tree 转移给 module）
- bcode_function_t 的 root_scope 和 bc 借用 module 对象，module 生命周期 >= 函数

## 7. 与旧实现的差异

| 方面 | 旧实现 | 新设计 |
|------|--------|--------|
| 子 sema | sema_create_child + children 列表 + 递归 destroy | 不存在子 sema，每模块独立创建+销毁 |
| module 产物 | module_info 借用子 sema 的 types/scope（悬空风险） | module 自包含 bc + global_scope + exports |
| 全局函数表 | 所有模块 DEFINE 到同一 root_scope | 各模块 DEFINE 到各自 global_scope |
| GET_MEMBER | 从 current_scope 按名查找（无命名空间隔离） | 从 module->exports 按名查找 |
| 跨模块调用 | bcode_call_cfunc 用 vm->bc（错误 bcode） | bcode_function_t 持有 bc，用正确的 bcode |
| 循环依赖 | module_manager begin/end_compile | 路径栈 vec，简单直接 |
| 内存泄漏 | 子模块 scope/tokens/lexer 未释放 | sema 用完即销毁，产物归 module 归 vm |

## 8. 实现步骤

1. **module_t 结构 + vm->modules map** — 定义新结构，替换 modules_by_path
2. **bcode_function_t 加 bc 字段** — bcode_call_cfunc 用 bfn->bc
3. **compile_module 函数** — 统一入口：lex → parse → sema → compile → module
4. **sema import 递归调 compile_module** — 同步递归 + 路径栈循环检测
5. **sema 从 module->exports 取成员类型** — `::` 访问的 sema 解析
6. **compiler IMPORT/GET_MEMBER 不变** — 字节码序列不变
7. **op_get_member 改为查 module->exports** — 替代 scope_lookup
8. **driver_run_file 改为调 compile_module** — 入口模块也走统一路径
9. **模块注册段按序执行** — 各模块 exec_run，切换 root_scope
10. **清理旧代码** — module_manager、sema_create_child、children 等
