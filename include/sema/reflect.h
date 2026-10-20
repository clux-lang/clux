#ifndef _H_CLUX_SEMA_REFLECT_
#define _H_CLUX_SEMA_REFLECT_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "vm/type.h"
#include "vm/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===========================================================================
 * 反射元数据（`T.<key>` 注入）—— builtin 取值核心
 *
 * 设计见 docs/reflect-builtin-design.md：
 *   - 注入面是 **type value 的成员**（`T.name` / `T.field_count` / `T.field_type(0)`）；
 *   - 求值在 sema（折叠成常量），运行期零指令；
 *   - 值域：str / usize / type / bool；
 *   - kind 与 ownership 都返回**编译期字符串**（自描述）。
 *
 * 本文件只做"类型 + key + 可选索引 → 值描述"，不碰 AST：sema 与 CTFE 共用这
 * 一份实现（避免两套下钻逻辑漂移）。
 * =========================================================================== */

typedef enum {
  SEMA_RV_STR = 0, /* str（名字 / kind / ownership） */
  SEMA_RV_USIZE,   /* usize（计数 / 偏移 / 大小 / 对齐 / tag） */
  SEMA_RV_I64,     /* i64（enum 变体值） */
  SEMA_RV_BOOL,    /* bool（is_variadic / is_c_abi） */
  SEMA_RV_TYPE     /* type（类型值：field_type / elem_type / return_type …） */
} sema_rv_kind_t;

typedef struct {
  sema_rv_kind_t kind;
  strslice_t     s;   /* SEMA_RV_STR：字符串（指向类型池/arena，不拥有） */
  uint64_t       u;   /* SEMA_RV_USIZE */
  int64_t        i;   /* SEMA_RV_I64 */
  bool           b;   /* SEMA_RV_BOOL */
  const type_t  *t;   /* SEMA_RV_TYPE */
} sema_rv_t;

/* key 是否带一个索引实参（`field_type(0)`）——**按类型判定**：数组/切片的
   `elem_type`/`elem_ownership` 不带索引，元组的同名 key 带索引。 */
bool sema_reflect_needs_index(const type_t *T, strslice_t key);

/** key 是否是反射成员名（带/不带索引都算）。链式表达式识别用。 */
bool sema_reflect_is_key(strslice_t key);

/** rv → 真实 value（CTFE 与 sema 共用）；失败返回 NULL。 */
value_t *sema_reflect_value(vm_t *vm, const sema_rv_t *rv);

/**
 * 查询反射 key：T 为被反射的类型，key 为成员名，has_index/index 为可选索引。
 * 成功返回 true（写 out）；失败返回 false，并在 err 中写诊断文本
 * （包含可用 key 列表 / 越界 / kind 不符等原因）。
 */
bool sema_reflect_lookup(const type_t *T, strslice_t key, bool has_index,
                         uint64_t index, sema_rv_t *out, char *err,
                         size_t ecap);

/** 类型 kind 的字符串名（"struct" / "int" …）；未知返回 "unknown"。 */
const char *sema_reflect_kind_name(const type_t *T);

/** 所有权槽位的字符串名（"own" / "ref" / "fatal" / "none"）。 */
const char *sema_reflect_ownership_name(ownership_t own);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_SEMA_REFLECT_ */
