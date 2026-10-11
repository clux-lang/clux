#ifndef _H_CLUX_VM_FFI_LIB_
#define _H_CLUX_VM_FFI_LIB_

#include "core/allocator.h"
#include "vm/type.h"   /* type_t：C ABI 类型约束判定（m6-design §2） */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M6 FFI：动态库加载与符号解析（OS 层薄封装）
 *
 *     LoadLibraryA / GetProcAddress / FreeLibrary   （Windows）
 *     dlopen       / dlsym          / dlclose       （POSIX）
 *
 * **本层不需要 libffi**：libffi 只服务于后续的"动态调用"（Phase 4）与"回调"
 * （Phase 5）。因此库加载与符号解析路径可以先独立落地并测试。
 *
 * 判据与语法一致：本层只接受"要加载哪个文件"的路径，**不做任何后缀判类**
 * （`.dll`/`.so`/`.dylib` 都只是名字；同名文件也可能是普通数据）。错误一律
 * 由操作系统判定（如 ERROR_BAD_EXE_FORMAT），不由扩展名推断。
 *
 * 错误约定：失败返回 NULL，并把人类可读原因写入调用方提供的 `err` 缓冲
 * （NUL 终止；ecap 为 0 时忽略）。成功时 err 不被写入。
 */

typedef struct ffi_lib_t ffi_lib_t; /* opaque：内部持 OS 句柄与路径副本 */

/** 打开动态库。成功返回句柄（owned，须 ffi_lib_close），失败返回 NULL。 */
ffi_lib_t *ffi_lib_open(allocator_t *alloc, const char *path, char *err,
                        size_t ecap);

/** 解析符号（函数或数据）。失败返回 NULL 并写入 err。 */
void *ffi_lib_sym(const ffi_lib_t *lib, const char *name, char *err, size_t ecap);

/* ---- M6 FFI：特殊外部模块 `*` = **当前进程自身** ----
 *
 * libc 这类库在进程启动时就已链接进来，不需要（也不该）用 `extern lib` 再主动
 * 加载一次。`*` 就是"当前进程"这个内置库别名：
 *
 *     extern cfunc *::abs(x: i32): i32;   // 直接引用进程里已有的符号
 *     extern var   *::my_global: i32;
 *
 * 解析语义对齐 POSIX 的 `dlsym(RTLD_DEFAULT, name)`——在**全局作用域**里找符号
 * （可执行文件及其依赖，以及 RTLD_GLOBAL 加载的库）。Windows 没有 RTLD_DEFAULT，
 * 等价实现是"先主模块、再遍历已加载模块"（只查主模块查不到 `abs`/`strlen` 这类
 * 由 CRT 提供的符号）。
 *
 * 失败返回 NULL 并把人类可读原因写入 err（与 ffi_lib_sym 同约定）。 */
void *ffi_sym_current_process(const char *name, char *err, size_t ecap);

/** 库别名字面量：`*`（当前进程）。多处需要比较，集中定义避免拼错。 */
#define FFI_PROCESS_ALIAS "*"

/** 关闭句柄并释放本对象（含路径副本）。NULL 安全。 */
void ffi_lib_close(ffi_lib_t *lib);

/* ---- 最小 ABI 层（v0；libffi 到位后可替换实现）----
 *
 * 按**整数/指针 ABI** 调用已解析的符号：参数与返回值都是 int64/指针宽度。
 * 覆盖 malloc/free/strlen/getpid 这类常见 C 接口；浮点与结构体传参需要各平台
 * 不同的寄存器/栈布局（x64 MSVC 走 XMM、SysV 走 XMM0-7…），不在 v0 范围，
 * 由调用方在类型层拒绝（而不是猜着传参）。
 *
 * 约定：成功返回 true 并写 *out；失败返回 false 并写 err（如参数个数超限）。
 */
bool ffi_call_int(void *sym, size_t argc, const int64_t *argv, int64_t *out,
                  char *err, size_t ecap);

/* ---- libffi 通用签名调用（CLUX_HAVE_LIBFFI 时可用）----
 *
 * 与 ffi_call_int 的区别：参数/返回类型由 libffi 的 ffi_type 描述符给出，
 * 因此**浮点与结构体**都能正确传递（x64 需要 XMM 寄存器约定，函数指针强转
 * 做不到）。`struct _ffi_type` 是 libffi 的类型描述符，这里只用不透明指针，
 * 调用方若需构造描述符可包含 <ffi.h>（链上 FFI::ffi 即自动获得）。
 *
 * arg_values 是各实参的**地址**数组（与 libffi 的约定一致）；
 * ret_value 指向足够容纳返回值的存储（可为 NULL 表示丢弃返回值）。
 */
/* ---- 浮点调用（double 参数/返回；不需要 libffi）----
 *
 * 用"按参数个数分派的**类型化**函数指针"实现：`double(double, ...)` 在 x64 下按
 * ABI 走 XMM0-3 与 XMM0 返回，编译器生成的调用序列即正确约定——因此浮点调用
 * 无需 libffi 的动态汇编（本仓库 Windows x64 的 libffi win64 汇编路径有问题，见
 * docs/m6-design.md §14.4）。argc ≤ 4 时全部参数为 double；返回写 *out。
 */
/** §2 C ABI 类型约束：NULL = 可用；否则返回拒绝原因（见 m6-design §2.1/§2.2）。 */
const char *ffi_c_abi_reject_reason(const type_t *t);

/* ---- M6 FFI：动态库导出的**数据符号**（`extern var <库>::<名>: T;`）----
 *
 * dlsym/GetProcAddress 对数据符号返回的**就是变量地址**，因此读写都直接作用于
 * 库里那块内存（没有本地副本）。宽度与类别必须与库里符号的真实类型一致——
 * 读多/写多会踩到相邻符号，所以只放行下面这张表里的类型：
 *
 *     i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool opaque(指针宽度)
 *
 * 类型 ↔ `BCODE_FFI_DATA_*` 码（定义见 vm/bcode.h，是 EXTERN_LOAD/STORE 的
 * 第二操作数）的**编解码在同一文件里成对实现**，避免两处清单漂移。 */
bool ffi_data_kind_of(const type_t *t, const struct vm_t *vm, uint32_t *out_kind);
/** 码 → clux 类型 + 字节宽度；未知码返回 NULL（容器被改坏时的兜底）。 */
const type_t *ffi_data_type_of(const struct vm_t *vm, uint32_t kind,
                               size_t *out_size);
/** 码 → libffi 类型描述符（**不透明指针**，免得本头文件依赖 ffi.h）；
 *  未知码返回 NULL。clux 标量与 C 是 1:1 的，这张表就是那个映射。 */
const void *ffi_data_ffi_type(uint32_t kind);

/** 该类型能否作为 extern var（数据符号）——需要"类型随指令落盘"的那些：
 *  struct/cunion（布局要查类型表）、enum（值要按 enum 类型造）。 */
bool ffi_data_kind_needs_type(const type_t *t);

/* ---- clux 类型 → libffi 描述符（可递归：struct/cunion/enum/指针/str）----
 *
 * 标量直接返回 libffi 的**静态**描述符（out_owned=false，不需要释放）；
 * struct/cunion 会**新建**描述符树（malloc 分配，out_owned 置 true，用完调
 * `ffi_type_release`——与闭包 box 同一约定：libffi 相关内存不走项目 allocator）。
 * 返回 NULL = 该类型无法跨 C ABI。
 *
 * - struct：按字段顺序递归构造成 `ffi_type{FFI_TYPE_STRUCT}`（clux 的布局就是
 *   C 布局，编译器保证平凡同构——所以直接传 value->data 即可）；
 * - cunion：libffi **没有** union 类型 → 等价建模成"size/align 个 align 宽度的
 *   整数"（`struct { intN a; intN b; ... }`：size 与 align 完全一致，ABI 分类与
 *   寄存器/栈占用因此与真 union 相同）；
 * - enum：其底层整数类型。 */
const void *ffi_type_for(const type_t *t, const struct vm_t *vm, bool *out_owned,
                         char *err, size_t ecap);
/** 释放 `ffi_type_for` 新建的描述符树（静态描述符可安全传入）。 */
void ffi_type_release(const void *desc);

/* ---- 精确类型调用（libffi；定参与变参统一入口）----
 *
 * 与 ffi_call_typed 的区别只在**变参**：`nfixed < argc` 时用
 * `ffi_prep_cif_var`（C 的 `...`），此时**变参部分的类型必须是 C 默认实参提升后
 * 的类型**（f32→double、窄整型→int）——提升由调用方负责（它才知道哪些位置是变参）。
 *
 * 约定：`arg_types[i]` 与 `arg_values[i]` 一一对应，`arg_values[i]` 指向该实参的
 * **C 表示**（clux 的 value->data 与 C 同构：直接就是 int32_t/float/double/指针…）。
 * `nfixed == argc` 表示非变参；`ret_type == NULL` 表示 void 返回值。
 * 失败返回 false 并写 err。 */
bool ffi_call_typed_items(void *sym, size_t nfixed, size_t argc,
                          const void *const *arg_types, const void *ret_type,
                          void **arg_values, void *ret_value, char *err,
                          size_t ecap);

/** 闭包处理器：ret 指向返回值槽，args 为参数指针数组，user 为上下文。 */
typedef void (*ffi_closure_handler_t)(void *ret, void **args, void *user);

/** 创建闭包（trampoline 可当 C 函数指针用；handle 供销毁，code 是函数地址）。
 *  arg_types 是 libffi 的 ffi_type 指针数组，ret_type 是 ffi_type 指针；这里用
 *  void 指针保持本头文件不依赖 ffi.h（也避免声明早于 ffi.h 时的类型冲突）。 */
bool clux_ffi_closure_create(allocator_t *alloc, size_t argc, void **arg_types,
                             void *ret_type, ffi_closure_handler_t handler,
                             void *user, void **out_handle, void **out_code,
                             char *err, size_t ecap);

/** 销毁闭包。trampoline 由 libffi 分配（非本 allocator），必须显式释放。 */
void clux_ffi_closure_destroy(allocator_t *alloc, void *handle);

bool ffi_call_f64(void *sym, size_t argc, const double *args, double *out,
                  char *err, size_t ecap);

struct _ffi_type;
bool ffi_call_typed(void *sym, size_t argc,
                    const struct _ffi_type **arg_types,
                    const struct _ffi_type *ret_type, void **arg_values,
                    void *ret_value, char *err, size_t ecap);

/** 本句柄的加载路径（借用，永不为 NULL）。 */
const char *ffi_lib_path(const ffi_lib_t *lib);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_FFI_LIB_ */