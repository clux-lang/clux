#ifndef _H_CLUX_CORE_FFI_ABI_
#define _H_CLUX_CORE_FFI_ABI_

/* C ABI 类型分类 / 数据符号编码（libffi-free）。
 *
 * 这些判定只依赖 clux 类型表，供 sema（extern cfunc/var 校验）与 C 后端共用；
 * 与动态库加载、libffi 调用无关。原先散在 vm/ffi_lib.h 与 vm/bcode.h，迁移到
 * 只保留 C 后端的构建里不应再依赖 VM 执行器。 */

#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vm_t;

/* ---- extern var（数据符号）的类型码：与旧 CLUX_FFI_DATA_* 数值保持一致 ---- */
#define CLUX_FFI_DATA_I8    0u
#define CLUX_FFI_DATA_I16   1u
#define CLUX_FFI_DATA_I32   2u
#define CLUX_FFI_DATA_I64   3u
#define CLUX_FFI_DATA_U8    4u
#define CLUX_FFI_DATA_U16   5u
#define CLUX_FFI_DATA_U32   6u
#define CLUX_FFI_DATA_U64   7u
#define CLUX_FFI_DATA_F32   8u
#define CLUX_FFI_DATA_F64   9u
#define CLUX_FFI_DATA_PTR   10u
#define CLUX_FFI_DATA_BOOL  11u
#define CLUX_FFI_DATA_STR   12u
#define CLUX_FFI_DATA_TYPED 13u
#define CLUX_FFI_DATA_USIZE 14u

/* ---- extern cfunc 的返回类型码（调用点指令的返回槽编码） ---- */
#define CLUX_FFI_RET_VOID  0u
#define CLUX_FFI_RET_I32   1u
#define CLUX_FFI_RET_I64   2u
#define CLUX_FFI_RET_PTR   3u
#define CLUX_FFI_RET_F64   4u
#define CLUX_FFI_RET_U32   5u
#define CLUX_FFI_RET_U64   6u
#define CLUX_FFI_RET_I8    7u
#define CLUX_FFI_RET_I16   8u
#define CLUX_FFI_RET_U8    9u
#define CLUX_FFI_RET_U16   10u
#define CLUX_FFI_RET_BOOL  11u
#define CLUX_FFI_RET_F32   12u
#define CLUX_FFI_RET_STR   13u
#define CLUX_FFI_RET_TYPED 14u
#define CLUX_FFI_RET_USIZE 15u

#define CLUX_FFI_RET_KIND_MASK 0xFFu
#define CLUX_FFI_RET_PACK(kind, variadic, nfixed)                     \
    (((uint32_t)(kind) & CLUX_FFI_RET_KIND_MASK) |                    \
     ((uint32_t)((variadic) ? 1u : 0u) << 8) |                         \
     (((uint32_t)(nfixed) & 0xFFFFu) << 16))
#define CLUX_FFI_RET_UNPACK_KIND(v) ((uint32_t)(v) & CLUX_FFI_RET_KIND_MASK)
#define CLUX_FFI_RET_UNPACK_VARIADIC(v) (((uint32_t)(v) >> 8) & 1u)
#define CLUX_FFI_RET_UNPACK_NFIXED(v) (((uint32_t)(v) >> 16) & 0xFFFFu)

/* §2 C ABI 类型约束：NULL = 可跨 C ABI；否则为人类可读拒绝原因。 */
const char *ffi_c_abi_reject_reason_own(const type_t *t, ownership_t own);
const char *ffi_c_abi_reject_reason(const type_t *t);
bool ffi_c_abi_check_aggregate(const type_t *t, const struct vm_t *vm, char *err,
                               size_t ecap);

/* 数据符号类型 ↔ 类型码 / 宽度（decode 需要 vm 的内建类型单例）。 */
bool ffi_data_kind_of(const type_t *t, const struct vm_t *vm, uint32_t *out_kind);
const type_t *ffi_data_type_of(const struct vm_t *vm, uint32_t kind,
                               size_t *out_size);
bool ffi_data_kind_needs_type(const type_t *t);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_CORE_FFI_ABI_ */
