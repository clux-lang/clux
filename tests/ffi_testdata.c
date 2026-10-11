/* M6 FFI 测试素材：动态库导出的**全局数据符号**（extern var 的被测目标）
 *
 * 系统 C 库里可写的数据符号只有 int / 指针那几类（msvcrt 的 _fmode、
 * _osplatform、_pgmptr …），无法覆盖 i8/i16/i64、无符号各宽度、f32/f64 的读写
 * 宽度。因此自建一个极小的测试动态库：每种可编组类型一个导出变量，读写都真的
 * 作用于**库里的那块内存**（clux 侧 `extern var td::td_i32` 通过 dlsym 拿到的
 * 就是这个地址）。
 *
 * 这些变量会被测试**改写**（验证 EXTERN_STORE），因此约定：
 *   - 每个测试自己保存旧值并在结束前还原（同一进程内 DLL 只加载一次，
 *     导出变量是全进程共享的）；
 *   - 初值取"一眼能认出"的值，便于断言读到的是库里的值而不是本地副本。
 *
 * 编译为 SHARED（Windows: .dll / POSIX: .so），不需要额外依赖。
 */
#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#define CLUX_TESTDATA_EXPORT __declspec(dllexport)
#else
#define CLUX_TESTDATA_EXPORT __attribute__((visibility("default")))
#endif

CLUX_TESTDATA_EXPORT int8_t   td_i8  = -7;
CLUX_TESTDATA_EXPORT int16_t  td_i16 = -3000;
CLUX_TESTDATA_EXPORT int32_t  td_i32 = -123456;
CLUX_TESTDATA_EXPORT int64_t  td_i64 = -5000000000LL;

CLUX_TESTDATA_EXPORT uint8_t  td_u8  = 200;
CLUX_TESTDATA_EXPORT uint16_t td_u16 = 60000;
CLUX_TESTDATA_EXPORT uint32_t td_u32 = 4000000000u;
CLUX_TESTDATA_EXPORT uint64_t td_u64 = 18000000000000000000ull;

CLUX_TESTDATA_EXPORT float    td_f32 = 1.25f;
CLUX_TESTDATA_EXPORT double   td_f64 = 2.5;

CLUX_TESTDATA_EXPORT bool     td_bool = true;

/* 指针：指向本库里的另一个导出变量（非空、稳定），用于 opaque 的指针宽度读写 */
CLUX_TESTDATA_EXPORT void    *td_ptr = (void *)&td_i32;
