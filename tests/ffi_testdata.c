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
#include <stdarg.h>
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

/* ============================================================================
 * struct / cunion / enum / char* —— M6 §14.20 的被测目标
 *
 * clux 的 struct 布局就是 C 布局（编译器保证平凡同构），因此两侧同名结构体
 * 可以逐字节对应。这里覆盖 x64 的三种传参/返回类别：
 *   - 8 字节（整数寄存器）
 *   - 16 字节全 double（XMM）
 *   - 24 字节（内存传递 / 隐藏指针返回）
 * ==========================================================================*/

typedef struct { int32_t x; int32_t y; } CluxPoint;        /* 8B, INTEGER */
typedef struct { double x; double y; }   CluxDPoint;       /* 16B, SSE */
typedef struct { int32_t a; int64_t b; double c; } CluxBig; /* 24B, MEMORY */

typedef enum { CLUX_COLOR_RED = 1, CLUX_COLOR_GREEN = 2 } CluxColor;

/* 按值收 struct，返回标量 */
CLUX_TESTDATA_EXPORT int32_t clux_point_sum(CluxPoint p) { return p.x + p.y; }
CLUX_TESTDATA_EXPORT double  clux_dpoint_sum(CluxDPoint p) { return p.x + p.y; }
CLUX_TESTDATA_EXPORT int64_t clux_big_sum(CluxBig v) {
    return (int64_t)v.a + v.b + (int64_t)v.c;
}

/* 按值返回 struct（8B 走 RAX、16B SSE 走 XMM0/1、24B 走隐藏指针） */
CLUX_TESTDATA_EXPORT CluxPoint clux_point_make(int32_t x, int32_t y) {
    CluxPoint p = { x, y };
    return p;
}
CLUX_TESTDATA_EXPORT CluxDPoint clux_dpoint_make(double x, double y) {
    CluxDPoint p = { x, y };
    return p;
}
CLUX_TESTDATA_EXPORT CluxBig clux_big_make(int32_t a, int64_t b, double c) {
    CluxBig v = { a, b, c };
    return v;
}

/* struct 数据符号（extern var 读/写整块） */
CLUX_TESTDATA_EXPORT CluxPoint td_point = { 3, 4 };

/* enum（C 里就是整数）：参数/返回/数据符号 */
CLUX_TESTDATA_EXPORT CluxColor clux_color_next(CluxColor c) {
    return c == CLUX_COLOR_RED ? CLUX_COLOR_GREEN : CLUX_COLOR_RED;
}
CLUX_TESTDATA_EXPORT CluxColor td_color = CLUX_COLOR_GREEN;

/* char* 返回（clux 侧声明为 str） */
CLUX_TESTDATA_EXPORT const char *clux_greeting(void) { return "hello from testdata"; }

/* 回调：把 struct 按值传给 clux 函数，并把它的 struct 返回值按值返回 */
typedef CluxPoint (*clux_point_cb)(CluxPoint);
CLUX_TESTDATA_EXPORT int32_t clux_call_point_cb(clux_point_cb cb, int32_t x,
                                                int32_t y) {
    CluxPoint in = { x, y };
    CluxPoint out = cb(in);
    return out.x * 100 + out.y;
}

/* 嵌套结构体（描述符递归构造的被测目标） */
typedef struct { CluxPoint p; int32_t z; } CluxNested;
CLUX_TESTDATA_EXPORT int32_t clux_nested_sum(CluxNested v) {
    return v.p.x + v.p.y + v.z;
}
CLUX_TESTDATA_EXPORT CluxNested clux_nested_make(int32_t x, int32_t y,
                                                 int32_t z) {
    CluxNested v;
    v.p.x = x;
    v.p.y = y;
    v.z = z;
    return v;
}

/* 指向结构体的指针（"传指针"路线：sema 侧要能解析 ref *P） */
CLUX_TESTDATA_EXPORT int32_t clux_point_ptr_sum(const CluxPoint *p) {
    return p ? p->x + p->y : -1;
}

/* 变参里的结构体（C 对聚合类型不做默认提升） */
#include <stdarg.h>
CLUX_TESTDATA_EXPORT int32_t clux_point_va_sum(int32_t n, ...) {
    va_list ap;
    va_start(ap, n);
    int32_t sum = 0;
    for (int32_t i = 0; i < n; i++) {
        CluxPoint p = va_arg(ap, CluxPoint);
        sum += p.x + p.y;
    }
    va_end(ap);
    return sum;
}

/* ---- C ABI 的 (T*, size_t) 形态：`data(s)` + `len(s)` 的被测目标 ----
 * clux 侧 `td::clux_sum_i32(data(s), len(s))` 与 C 的 `sum_i32(ptr, n)` 同形。 */
CLUX_TESTDATA_EXPORT int32_t clux_sum_i32(const int32_t *p, int32_t n) {
    int32_t sum = 0;
    for (int32_t i = 0; i < n; i++) {
        if (p) sum += p[i];
    }
    return sum;
}

/* 只读首元素（验证 data() 给的确实是首元素地址） */
CLUX_TESTDATA_EXPORT int32_t clux_first_i32(const int32_t *p) {
    return p ? p[0] : -1;
}

/* 写回：C 通过指针改调用方的缓冲区（clux 侧看到变化 → 证明是同一块内存） */
CLUX_TESTDATA_EXPORT void clux_fill_i32(int32_t *p, int32_t n, int32_t v) {
    if (!p) return;
    for (int32_t i = 0; i < n; i++) p[i] = v + i;
}

/* char* 形态：data(str) → const char* */
CLUX_TESTDATA_EXPORT int32_t clux_str_first_byte(const char *s) {
    return s ? (int32_t)(unsigned char)s[0] : -1;
}

/* ---- C 函数指针作为**返回值**（`extern cfunc ...: cfunc(i32,i32)->i32`）----
 * clux 侧拿到的就是裸函数指针，可以直接再传给另一个 C 函数（或其 clux
 * `cfunc_ptr` 产物）。 */
typedef int32_t (*clux_binop)(int32_t, int32_t);

static int32_t td_binop_add(int32_t a, int32_t b) { return a + b; }
static int32_t td_binop_sub(int32_t a, int32_t b) { return a - b; }

CLUX_TESTDATA_EXPORT clux_binop clux_get_binop(int32_t which) {
    return which == 0 ? td_binop_add : td_binop_sub;
}

CLUX_TESTDATA_EXPORT int32_t clux_apply_binop(clux_binop op, int32_t a,
                                              int32_t b) {
    return op ? op(a, b) : -1;
}

/* char* 全局（clux 侧 `extern var ...: str;`）：读成 clux_str，写取 .data */
CLUX_TESTDATA_EXPORT const char *td_str = "hello from testdata";
