# clux 反射设计文档

## 目标

反射是 clux 在编译期获取类型元数据的能力。反射库（std/Reflect）用**泛型 + comptime func + builtin 关键字**实现，CTFE 折叠产出编译期常量数据，运行期只读、零开销。

设计原则：

- **反射库是 clux 源码实现**——std/Reflect 模块用 `comptime func` + 泛型 + builtin 组合出完整反射 API，不是 compiler 特殊识别的调用。
- **compiler 只提供少量 builtin 关键字**——类似 `sizeof`/`alignof`，在 AST 阶段有专门节点，sema/CTFE 直接识别求值。builtin 不需要函数声明/签名，类型由 sema 根据节点语义推断。
- **编译期可变，运行期只读**——反射信息在编译期由 CTFE 折叠产出（`comptime func` 调用折叠为常量），运行期不可修改（`const`）。
- **无数据字段持有元数据**——所有反射 Info 类型都不持有数据字段，所有元数据通过方法在 CTFE 时用 builtin 折叠产出。泛型参数（T、idx）一路传递，每个方法内部调对应 builtin。
- **泛型一路传递**——`TypeInfo[T]` → `StructInfo[T]` → `FieldInfo[T, idx]` → `TypeInfo[FieldType]`，泛型参数贯穿整个反射数据树，每个方法在编译期解析到具体的 builtin 调用。
- **interface 不参与反射**——interface 不是类型（§31.4），没有反射元数据。

> **实现状态（2026-10-22 复核）**：
> - **注入面已改为 `T.<key>`**（type value 的成员注入），不再使用 builtin 关键字/关键字宏；
>   P0–P2 已实现并有测试覆盖，见 [reflect-builtin-design.md](reflect-builtin-design.md)。
> - **本文档只保留 `std/Reflect` 库（P3）的设计**：`TypeInfo[T]` / `StructInfo[T]` /
>   `FieldInfo[T, idx]` 等泛型封装，依赖 M7 泛型 + CTFE；不含实现计划。

---

## builtin 关键字

反射的底层数据来源是一组 **builtin 关键字**——类似 `sizeof`，在 AST 阶段有专门节点，sema/CTFE 直接识别求值。builtin 不需要函数声明/签名，类型由 sema 根据节点语义推断。

> 下文用类似函数调用的形式（`typename(T)`）描述 builtin 的**语义**，但实际是关键字/AST 节点，不是函数调用。

### 通用（所有类型）

| builtin | 语义 | 返回 |
|---------|------|------|
| `typename(T)` | 类型名 | `str` |
| `sizeof(T)` | 类型大小 | `u64` |
| `alignof(T)` | 类型对齐 | `u64` |
| `typekind(T)` | 类型种类 | `TypeKind` 枚举 |

`TypeKind` 枚举区分类型种类：Struct / Enum / Union / Func / Array / Slice / Ptr / Tuple / Primitive / ...。

### struct

| builtin | 语义 | 返回 |
|---------|------|------|
| `field_count(T)` | 字段数量 | `u64` |
| `field_name(T, id: u64)` | 按索引取字段名 | `str` |
| `field_type(T, name: str)` | 按名字取字段类型 | `type`（类型值） |
| `field_offset(T, name: str)` | 按名字取字段偏移 | `u64` |
| `methods(T)` | 方法名数组（无序） | `[N]str`（N 由 CTFE 推断填入） |
| `attributes(T)` | 属性名数组（无序） | `[N]str`（N 由 CTFE 推断填入） |
| `method_type(T, name: str)` | 按名字取方法签名类型 | `type` |
| `attribute_type(T, name: str)` | 按名字取属性类型 | `type` |

**方法/属性无序**：不像字段有声明顺序，方法/属性是无序集合。`methods(T)` / `attributes(T)` 返回名字数组（编译期定长 `[N]str`，N 由 CTFE 推断），按名字遍历，详细信息按名字查询。

### enum

| builtin | 语义 | 返回 |
|---------|------|------|
| `enum_variant_count(T)` | 变体数量 | `u64` |
| `enum_variant_name(T, id: u64)` | 按索引取变体名 | `str` |
| `enum_variant_type(T, name: str)` | 按名字取变体关联类型 | `type` |

### union

| builtin | 语义 | 返回 |
|---------|------|------|
| `union_variant_count(T)` | 变体数量 | `u64` |
| `union_variant_name(T, id: u64)` | 按索引取变体名 | `str` |
| `union_variant_type(T, name: str)` | 按名字取变体关联类型 | `type` |

### func

| builtin | 语义 | 返回 |
|---------|------|------|
| `func_param_count(T)` | 参数数量 | `u64` |
| `func_param_type(T, id: u64)` | 按索引取参数类型 | `type` |
| `func_return_type(T)` | 返回类型 | `type` |

### array

| builtin | 语义 | 返回 |
|---------|------|------|
| `array_elem_type(T)` | 元素类型 | `type` |
| `array_len(T)` | 数组长度 | `u64` |

### slice

| builtin | 语义 | 返回 |
|---------|------|------|
| `slice_elem_type(T)` | 元素类型 | `type` |

### ptr

| builtin | 语义 | 返回 |
|---------|------|------|
| `ptr_elem_type(T)` | 指向类型 | `type` |

### tuple

| builtin | 语义 | 返回 |
|---------|------|------|
| `tuple_elem_count(T)` | 元素数量 | `u64` |
| `tuple_elem_type(T, id: u64)` | 按索引取元素类型 | `type` |

---

## 反射 API：Info 类型

反射 API 由一组**泛型 struct + comptime func 方法**组成，挂在 `Reflect` 模块命名空间下。所有 Info 类型遵循同一模式：

- **无数据字段持有元数据**——struct 体不声明存元数据的字段（实例本身是薄包装/空壳）。
- **靠方法 + builtin 折叠产出**——每个方法内部调 builtin，`comptime func` 调用在 CTFE 时折叠为常量。
- **泛型一路传递**——T（被反射的类型）与 idx（索引）等参数贯穿方法链，每个方法在编译期解析到具体 builtin 调用。
- **返回 `fatal const T`**——`comptime func` 返回 `fatal const InfoType[...]`（move 语义 + 只读），CTFE 折叠为编译期常量值。

### TypeInfo[T] —— 通用基本信息

所有类型都有 `TypeInfo[T]`，提供基本信息与按种类获取详细信息的入口。

```
struct TypeInfo[T] {
    comptime func get(): fatal const TypeInfo[T] { ... }

    // 基本信息（builtin 直接折叠）：
    comptime func getName(): str { ... }            // typename(T)
    comptime func getSize(): u64 { ... }            // sizeof(T)
    comptime func getAlign(): u64 { ... }           // alignof(T)
    comptime func getKind(): TypeKind { ... }       // typekind(T)

    // 按种类获取详细信息（T 不是对应种类时编译期错误）：
    comptime func asStruct(): fatal const StructInfo[T] { ... }
    comptime func asEnum(): fatal const EnumInfo[T] { ... }
    comptime func asUnion(): fatal const UnionInfo[T] { ... }
    comptime func asFunc(): fatal const FuncInfo[T] { ... }
    comptime func asArray(): fatal const ArrayInfo[T] { ... }
    comptime func asSlice(): fatal const SliceInfo[T] { ... }
    comptime func asPtr(): fatal const PtrInfo[T] { ... }
    comptime func asTuple(): fatal const TupleInfo[T] { ... }
}
```

`asStruct()` 等方法在 CTFE 折叠时检查 `typekind(T)`，T 不是对应种类时**编译期错误**。

### StructInfo[T] —— struct 详细信息

字段信息按需获取——`StructInfo[T]` 不持有字段表，通过方法返回 `FieldInfo`。

```
struct StructInfo[T] {
    comptime func get(): fatal const StructInfo[T] { ... }

    comptime func getFieldCount(): u64 { ... }     // field_count(T)
    comptime func getField[idx: comptime u64](): fatal const FieldInfo[T, idx] { ... }
    // field_count + getField[idx] 按索引遍历字段
}
```

### FieldInfo[T, idx] —— 字段信息

```
struct FieldInfo[T, idx: comptime u64] {
    comptime func get(): fatal const FieldInfo[T, idx] { ... }

    comptime func getName(): str { ... }            // field_name(T, idx)
    comptime func getType(): TypeInfo[FieldType] { ... }
    // FieldType = field_type(T, field_name(T, idx))
    // 泛型一路传递：field_type 返回类型值 → TypeInfo[类型值]::get()
    comptime func getOffset(): u64 { ... }          // field_offset(T, field_name(T, idx))
}
```

`getType()` 的实现链：`idx (u64) --field_name(T, idx)--> name (str) --field_type(T, name)--> type (类型值) --TypeInfo[type]::get()--> TypeInfo`。泛型一路传递到 `TypeInfo[FieldType]`。

### EnumInfo[T] / EnumVariantInfo[T, idx] —— enum 反射

```
struct EnumInfo[T] {
    comptime func get(): fatal const EnumInfo[T] { ... }
    comptime func getVariantCount(): u64 { ... }    // enum_variant_count(T)
    comptime func getVariant[idx: comptime u64](): fatal const EnumVariantInfo[T, idx] { ... }
}

struct EnumVariantInfo[T, idx: comptime u64] {
    comptime func get(): fatal const EnumVariantInfo[T, idx] { ... }
    comptime func getName(): str { ... }            // enum_variant_name(T, idx)
    comptime func getType(): TypeInfo[VariantType] { ... }
    // VariantType = enum_variant_type(T, enum_variant_name(T, idx))
}
```

### UnionInfo[T] / UnionVariantInfo[T, idx] —— union 反射

与 enum 同构：

```
struct UnionInfo[T] {
    comptime func get(): fatal const UnionInfo[T] { ... }
    comptime func getVariantCount(): u64 { ... }    // union_variant_count(T)
    comptime func getVariant[idx: comptime u64](): fatal const UnionVariantInfo[T, idx] { ... }
}

struct UnionVariantInfo[T, idx: comptime u64] {
    comptime func get(): fatal const UnionVariantInfo[T, idx] { ... }
    comptime func getName(): str { ... }            // union_variant_name(T, idx)
    comptime func getType(): TypeInfo[VariantType] { ... }
    // VariantType = union_variant_type(T, union_variant_name(T, idx))
}
```

### FuncInfo[T] —— func 反射

```
struct FuncInfo[T] {
    comptime func get(): fatal const FuncInfo[T] { ... }
    comptime func getParamCount(): u64 { ... }      // func_param_count(T)
    comptime func getParamType[idx: comptime u64](): TypeInfo[ParamType] { ... }
    // ParamType = func_param_type(T, idx)
    comptime func getReturnType(): TypeInfo[RetType] { ... }
    // RetType = func_return_type(T)
}
```

### ArrayInfo[T] —— array 反射

```
struct ArrayInfo[T] {
    comptime func get(): fatal const ArrayInfo[T] { ... }
    comptime func getElemType(): TypeInfo[ElemType] { ... }   // array_elem_type(T)
    comptime func getLen(): u64 { ... }                        // array_len(T)
}
```

### SliceInfo[T] —— slice 反射

```
struct SliceInfo[T] {
    comptime func get(): fatal const SliceInfo[T] { ... }
    comptime func getElemType(): TypeInfo[ElemType] { ... }   // slice_elem_type(T)
}
```

### PtrInfo[T] —— ptr 反射

```
struct PtrInfo[T] {
    comptime func get(): fatal const PtrInfo[T] { ... }
    comptime func getElemType(): TypeInfo[ElemType] { ... }   // ptr_elem_type(T)
}
```

### TupleInfo[T] —— tuple 反射

```
struct TupleInfo[T] {
    comptime func get(): fatal const TupleInfo[T] { ... }
    comptime func getElemCount(): u64 { ... }                 // tuple_elem_count(T)
    comptime func getElemType[idx: comptime u64](): TypeInfo[ElemType] { ... }
    // ElemType = tuple_elem_type(T, idx)
}
```

---

## 使用示例

```
// 获取类型基本信息
var info = Reflect::TypeInfo[i32]::get();
var name = info.getName();    // "i32"（CTFE 折叠为字符串常量）
var size = info.getSize();    // 4

// 反射 struct 字段
struct Point { x: i32; y: i64; }
var sinfo = Reflect::TypeInfo[Point]::get().asStruct();
var fcount = sinfo.getFieldCount();                  // 2
var finfo = sinfo.getField[0]();                     // FieldInfo[Point, 0]
var fname = finfo.getName();                         // "x"
var ftype = finfo.getType();                         // TypeInfo[i32]
var foff  = finfo.getOffset();                       // 0

// 泛型一路传递：finfo.getType() 内部
//   idx=0 --field_name(Point, 0)--> "x" --field_type(Point, "x")--> i32 --TypeInfo[i32]::get()--> TypeInfo[i32]

// CTFE 消费反射（如自动序列化的字段遍历）
comptime func dump_fields[T](): void {
    var info = Reflect::TypeInfo[T]::get().asStruct();
    var n = info.getFieldCount();
    for (i = 0; i < n; i += 1) {
        // 注意：getField[i] 的 i 需 comptime 已知——for-index 的 i 是运行期变量
        // 实际 CTFE 遍历需编译期循环展开（递归实例化或 comptime for）
    }
}
```

---

## 与其他机制的关系

- **M7 泛型**：反射 API 是泛型的典型应用——`TypeInfo[T]` / `StructInfo[T]` / `FieldInfo[T, idx]` 等都是泛型类型，实例化产生具体反射数据。
- **CTFE**：`comptime func` 调用在 CTFE 时折叠为编译期常量，反射数据是 CTFE 产物。
- **类型值一等值（M2 §1）**：`field_type(T, name)` 等返回类型值，可直接作为泛型实参（`TypeInfo[类型值]`），无需额外语法。
- **builtin 关键字**：反射的底层数据来源，AST 节点级，sema 直接求值，不需要函数声明。
- **std/Reflect 库（stdlib §5.1）**：本文档设计的反射 API 即 std/Reflect 库的内容，用 clux 源码实现。
- **JSON/YAML 自动序列化（stdlib §3.4）**：依赖反射遍历 struct 字段，自动派生 to_json/from_json。
- **interface 不参与反射**：interface 不是类型（m7 §31.4），没有反射元数据。
