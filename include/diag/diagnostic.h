#ifndef _H_CLUX_DIAG_DIAGNOSTIC_
#define _H_CLUX_DIAG_DIAGNOSTIC_
#include "core/allocator.h"
#include "parser/location.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 诊断收集器（公共模块）
 *
 * Lexer / Parser / Sema / Bytecode Compiler / Bytecode VM 共用同一个收集器，
 * driver 统一在流水线出口打印，而不是边错边打。
 * =========================================================================== */

/* ---- Levels ---- */

typedef enum {
  DIAG_ERROR,   /* 硬错误：编译终止 */
  DIAG_WARNING, /* 警告：不终止编译 */
  DIAG_NOTE,    /* 提示：附加信息 */
} diag_level_t;

/* ---- Single diagnostic ---- */

typedef struct _diagnostic_t {
  diag_level_t level;
  location_t   loc;
  char        *message;  /* NUL-terminated, owned by the buffer */
  /* loc.filename 的**自有副本**（NUL 终止，owned by the buffer）。
     location_t 里的 filename 是借用指针（词法器的 filename "must outlive the
     lexer"、驱动侧规范化路径等），调用方可能在诊断打印前释放它；诊断在注册时
     拷贝一份，打印/读源码行只用这份副本。loc 本身保留原值不动。 */
  char        *filename;
} diagnostic_t;

/* ---- Buffer (opaque to callers) ---- */

typedef struct _diag_buf_t diag_buf_t;

/* ---- Lifecycle ---- */

/**
 * Create an empty diagnostic buffer backed by `alloc`.
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
diag_buf_t *diag_buf_new(allocator_t *alloc);

/**
 * Destroy the buffer and nullify the caller's pointer.
 * Frees all stored messages. No-op if `db` or `*db` is NULL.
 */
void diag_buf_destroy(diag_buf_t **db);

/* ---- Recording ---- */

/**
 * Record an error-level diagnostic. The message is formatted with
 * printf semantics (`fmt`, ...) and copied into the buffer.
 * No-op if `db` is NULL.
 */
void diag_error(diag_buf_t *db, location_t loc, const char *fmt, ...);

/** Record a warning-level diagnostic (printf semantics). No-op if `db` is NULL. */
void diag_warning(diag_buf_t *db, location_t loc, const char *fmt, ...);

/** Record a note-level diagnostic (printf semantics). No-op if `db` is NULL. */
void diag_note(diag_buf_t *db, location_t loc, const char *fmt, ...);

/* ---- Queries / output ---- */

/** Return the number of recorded diagnostics. */
size_t diag_count(const diag_buf_t *db);

/** Return true if any error-level diagnostic was recorded. */
bool diag_has_error(const diag_buf_t *db);

/**
 * Print all recorded diagnostics to stderr, one per line, in record order.
 * Format: `<file>:<line>:<col>: <level>: <message>`
 */
void diag_print_all(const diag_buf_t *db);

/**
 * Return the recorded diagnostics as a raw array of `diag_count(db)` items
 * (read-only; valid until the next recording operation or destroy).
 * Returns NULL for a NULL buffer or an empty buffer.
 */
const diagnostic_t *diag_items(const diag_buf_t *db);

#ifdef __cplusplus
}
#endif
#endif
