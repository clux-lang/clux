#ifndef _H_CLUX_CORE_JSON_
#define _H_CLUX_CORE_JSON_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/strslice.h"

/* ===========================================================================
 * 轻量 JSON 解析器
 *
 * 仅用于解析 clux.mod（包描述文件）。支持标准 JSON 子集：
 *   - object  {"key": value, ...}
 *   - array   [value, ...]
 *   - string  "..."
 *   - number  整数 / 浮点
 *   - true / false / null
 *
 * 解析结果是一棵 arena 分配的不可变树，调用方通过访问器查询。
 * 不支持注释、尾逗号等非标准扩展。
 * =========================================================================== */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,   /* 整数或浮点，按是否有小数点区分 */
    JSON_STRING,   /* NUL 终止的 UTF-8 字符串 */
    JSON_ARRAY,
    JSON_OBJECT,
} json_kind_t;

typedef struct json_value json_value_t;

typedef struct {
    strslice_t key;         /* 借用：指向源文本，不保证 NUL 终止 */
    json_value_t *val;      /* 拥有 */
} json_member_t;

struct json_value {
    json_kind_t kind;
    union {
        bool        b;
        double      num;    /* 所有数字先存 double，整数可无损表示到 2^53 */
        const char *str;    /* NUL 终止拷贝（arena 分配） */
        struct {
            json_value_t **items;
            size_t          count;
        } arr;
        struct {
            json_member_t *members;
            size_t          count;
        } obj;
    } u;
};

/* ---- 解析 ---- */

typedef struct json_error {
    const char *msg;        /* 静态字符串或 arena 拷贝 */
    size_t      line;       /* 1-based 行号 */
    size_t      col;        /* 1-based 列号 */
} json_error_t;

/**
 * 解析 JSON 文本。成功返回根 value（arena 分配），*out_error 不修改。
 * 失败返回 NULL，*out_error 填充错误位置。
 *
 * @param arena   分配 arena（所有 json_value / 字符串拷贝都从这里分配）
 * @param text    JSON 文本（UTF-8，不需要 NUL 终止，长度由 len 给出）
 * @param len     文本长度
 * @param out_error 错误输出（可为 NULL）
 */
const json_value_t *json_parse(arena_t *arena, const char *text, size_t len,
                               json_error_t *out_error);

/* ---- 访问器 ---- */

/** 获取 object 中指定 key 的成员值，找不到返回 NULL。 */
const json_value_t *json_object_get(const json_value_t *obj, const char *key);

/** 同上，用 strslice_t key。 */
const json_value_t *json_object_get_slice(const json_value_t *obj,
                                          strslice_t key);

/** 获取 array 指定下标的元素，越界返回 NULL。 */
const json_value_t *json_array_get(const json_value_t *arr, size_t index);

/** 获取字符串值。非 JSON_STRING 返回 NULL。 */
const char *json_as_string(const json_value_t *v);

/** 获取布尔值。非 JSON_BOOL 返回 fallback。 */
bool json_as_bool(const json_value_t *v, bool fallback);

/** 获取数字值。非 JSON_NUMBER 返回 fallback。 */
double json_as_number(const json_value_t *v, double fallback);

#ifdef __cplusplus
}
#endif
#endif
