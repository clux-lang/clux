#include "core/json.h"
#include "core/panic.h"
#include <string.h>
#include <stdlib.h>

/* ===========================================================================
 * 轻量 JSON 解析器实现
 *
 * 递归下降解析器。所有分配走 arena（一次性回收，无需释放）。
 * 解析期间维护行/列号用于错误定位。
 * =========================================================================== */

typedef struct {
    arena_t    *arena;
    const char *text;
    size_t      len;
    size_t      pos;     /* 当前字节偏移 */
    size_t      line;    /* 1-based */
    size_t      col;     /* 1-based */
    json_error_t error;
} json_parser_t;

/* ---- 前向声明 ---- */

static const json_value_t *parse_value(json_parser_t *p);
static const json_value_t *parse_string(json_parser_t *p);
static const json_value_t *parse_number(json_parser_t *p);
static const json_value_t *parse_object(json_parser_t *p);
static const json_value_t *parse_array(json_parser_t *p);
static const json_value_t *parse_literal(json_parser_t *p);

/* ---- 工具函数 ---- */

static void json_error(json_parser_t *p, const char *msg) {
    if (!p->error.msg) {
        p->error.msg  = msg;
        p->error.line = p->line;
        p->error.col  = p->col;
    }
}

static bool at_end(json_parser_t *p) {
    return p->pos >= p->len;
}

static char peek(json_parser_t *p) {
    if (p->pos >= p->len) return '\0';
    return p->text[p->pos];
}

static char peek_at(json_parser_t *p, size_t offset) {
    if (p->pos + offset >= p->len) return '\0';
    return p->text[p->pos + offset];
}

static void advance(json_parser_t *p) {
    if (p->pos >= p->len) return;
    char c = p->text[p->pos++];
    if (c == '\n') {
        p->line++;
        p->col = 1;
    } else {
        p->col++;
    }
}

/* 跳过空白字符 */
static void skip_ws(json_parser_t *p) {
    while (!at_end(p)) {
        char c = peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            advance(p);
        else
            break;
    }
}

/* 分配一个 json_value */
static json_value_t *json_new(json_parser_t *p, json_kind_t kind) {
    json_value_t *v = (json_value_t *)arena_calloc(
        p->arena, 1, sizeof(json_value_t), ALIGNOF(max_align_t));
    if (!v) {
        json_error(p, "out of memory");
        return NULL;
    }
    v->kind = kind;
    return v;
}

/* NUL 终止的字符串拷贝（arena 分配） */
static char *dup_str(json_parser_t *p, const char *src, size_t len) {
    /* arena_alloc 在 size==0 时 panic，所以至少分配 1 字节 */
    char *dst = (char *)arena_alloc(p->arena, len + 1, ALIGNOF(char));
    if (!dst) {
        json_error(p, "out of memory");
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* ---- 解析函数 ---- */

static const json_value_t *parse_value(json_parser_t *p) {
    skip_ws(p);
    if (at_end(p)) {
        json_error(p, "unexpected end of input");
        return NULL;
    }
    char c = peek(p);
    if (c == '"')  return parse_string(p);
    if (c == '{')  return parse_object(p);
    if (c == '[')  return parse_array(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (c == 't' || c == 'f' || c == 'n') return parse_literal(p);
    json_error(p, "unexpected character");
    return NULL;
}

/* 解析字符串（含转义） */
static const json_value_t *parse_string(json_parser_t *p) {
    /* 假设 peek(p) == '"' */
    advance(p); /* 消费开头的 " */

    /* 第一趟：计算解码后长度并校验 */
    /* 由于 arena 分配，我们先收集到临时缓冲区。
       对于 clux.mod 这种小文件，直接用栈缓冲区。 */
    char buf[4096];
    size_t buf_len = 0;
    bool overflow = false;

    while (!at_end(p) && peek(p) != '"') {
        char c = peek(p);
        if (c == '\\') {
            advance(p);
            if (at_end(p)) {
                json_error(p, "unterminated string escape");
                return NULL;
            }
            char esc = peek(p);
            advance(p);
            char decoded = '\0';
            switch (esc) {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case 'n':  decoded = '\n'; break;
                case 't':  decoded = '\t'; break;
                case 'r':  decoded = '\r'; break;
                case 'b':  decoded = '\b'; break;
                case 'f':  decoded = '\f'; break;
                default:
                    json_error(p, "invalid escape sequence");
                    return NULL;
            }
            if (buf_len < sizeof(buf) - 1) {
                buf[buf_len++] = decoded;
            } else {
                overflow = true;
            }
        } else {
            advance(p);
            if (buf_len < sizeof(buf) - 1) {
                buf[buf_len++] = c;
            } else {
                overflow = true;
            }
        }
    }

    if (at_end(p)) {
        json_error(p, "unterminated string");
        return NULL;
    }
    advance(p); /* 消费结尾的 " */

    if (overflow) {
        json_error(p, "string too long (max 4095 bytes)");
        return NULL;
    }

    json_value_t *v = json_new(p, JSON_STRING);
    if (!v) return NULL;
    v->u.str = dup_str(p, buf, buf_len);
    if (!v->u.str) return NULL;
    return v;
}

/* 解析数字 */
static const json_value_t *parse_number(json_parser_t *p) {
    size_t start = p->pos;
    if (peek(p) == '-') advance(p);
    while (!at_end(p) && peek(p) >= '0' && peek(p) <= '9') advance(p);
    if (peek(p) == '.') {
        advance(p);
        while (!at_end(p) && peek(p) >= '0' && peek(p) <= '9') advance(p);
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        advance(p);
        if (peek(p) == '+' || peek(p) == '-') advance(p);
        while (!at_end(p) && peek(p) >= '0' && peek(p) <= '9') advance(p);
    }

    size_t num_len = p->pos - start;
    char tmp[64];
    if (num_len >= sizeof(tmp)) {
        json_error(p, "number too long");
        return NULL;
    }
    memcpy(tmp, p->text + start, num_len);
    tmp[num_len] = '\0';

    json_value_t *v = json_new(p, JSON_NUMBER);
    if (!v) return NULL;
    v->u.num = strtod(tmp, NULL);
    return v;
}

/* 解析 true / false / null */
static const json_value_t *parse_literal(json_parser_t *p) {
    if (peek(p) == 't' && peek_at(p, 1) == 'r' && peek_at(p, 2) == 'u' &&
        peek_at(p, 3) == 'e') {
        p->pos += 4; p->col += 4;
        json_value_t *v = json_new(p, JSON_BOOL);
        if (!v) return NULL;
        v->u.b = true;
        return v;
    }
    if (peek(p) == 'f' && peek_at(p, 1) == 'a' && peek_at(p, 2) == 'l' &&
        peek_at(p, 3) == 's' && peek_at(p, 4) == 'e') {
        p->pos += 5; p->col += 5;
        json_value_t *v = json_new(p, JSON_BOOL);
        if (!v) return NULL;
        v->u.b = false;
        return v;
    }
    if (peek(p) == 'n' && peek_at(p, 1) == 'u' && peek_at(p, 2) == 'l' &&
        peek_at(p, 3) == 'l') {
        p->pos += 4; p->col += 4;
        return json_new(p, JSON_NULL);
    }
    json_error(p, "invalid literal");
    return NULL;
}

/* 解析数组 */
static const json_value_t *parse_array(json_parser_t *p) {
    advance(p); /* 消费 [ */
    skip_ws(p);

    json_value_t *v = json_new(p, JSON_ARRAY);
    if (!v) return NULL;

    /* 临时收集到栈上的动态数组（arena 分配） */
    /* 先计算元素数再分配——两趟或先分配大数组再收缩。
       这里用 arena 分配增长策略：先 16，倍增。 */
    size_t cap = 8;
    json_value_t **items = (json_value_t **)arena_alloc(
        p->arena, cap * sizeof(json_value_t *), ALIGNOF(void *));
    if (!items) { json_error(p, "out of memory"); return NULL; }
    size_t count = 0;

    if (peek(p) == ']') {
        advance(p);
        v->u.arr.items = NULL;
        v->u.arr.count = 0;
        return v;
    }

    for (;;) {
        const json_value_t *item = parse_value(p);
        if (!item) return NULL;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            json_value_t **new_items = (json_value_t **)arena_alloc(
                p->arena, new_cap * sizeof(json_value_t *), ALIGNOF(void *));
            if (!new_items) { json_error(p, "out of memory"); return NULL; }
            memcpy(new_items, items, count * sizeof(json_value_t *));
            items = new_items;
            cap = new_cap;
        }
        items[count++] = (json_value_t *)item;

        skip_ws(p);
        if (peek(p) == ',') {
            advance(p);
            skip_ws(p);
            continue;
        }
        if (peek(p) == ']') {
            advance(p);
            break;
        }
        json_error(p, "expected ',' or ']' in array");
        return NULL;
    }

    v->u.arr.items = items;
    v->u.arr.count = count;
    return v;
}

/* 解析对象 */
static const json_value_t *parse_object(json_parser_t *p) {
    advance(p); /* 消费 { */
    skip_ws(p);

    json_value_t *v = json_new(p, JSON_OBJECT);
    if (!v) return NULL;

    size_t cap = 8;
    json_member_t *members = (json_member_t *)arena_alloc(
        p->arena, cap * sizeof(json_member_t), ALIGNOF(max_align_t));
    if (!members) { json_error(p, "out of memory"); return NULL; }
    size_t count = 0;

    if (peek(p) == '}') {
        advance(p);
        v->u.obj.members = NULL;
        v->u.obj.count = 0;
        return v;
    }

    for (;;) {
        skip_ws(p);
        if (peek(p) != '"') {
            json_error(p, "expected string key in object");
            return NULL;
        }

        /* 解析 key */
        const json_value_t *key_val = parse_string(p);
        if (!key_val) return NULL;

        skip_ws(p);
        if (peek(p) != ':') {
            json_error(p, "expected ':' after object key");
            return NULL;
        }
        advance(p); /* 消费 : */
        skip_ws(p);

        const json_value_t *val = parse_value(p);
        if (!val) return NULL;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            json_member_t *new_members = (json_member_t *)arena_alloc(
                p->arena, new_cap * sizeof(json_member_t), ALIGNOF(max_align_t));
            if (!new_members) { json_error(p, "out of memory"); return NULL; }
            memcpy(new_members, members, count * sizeof(json_member_t));
            members = new_members;
          cap = new_cap;
        }
        /* key 借用源文本——但 parse_string 已经解码转义并 arena 拷贝了。
           这里直接借用 key_val->u.str 的指针+长度。 */
        members[count].key = strslice_from_cstr(key_val->u.str);
        members[count].val = (json_value_t *)val;
        count++;

        skip_ws(p);
        if (peek(p) == ',') {
            advance(p);
            continue;
        }
        if (peek(p) == '}') {
            advance(p);
            break;
        }
        json_error(p, "expected ',' or '}' in object");
        return NULL;
    }

    v->u.obj.members = members;
    v->u.obj.count = count;
    return v;
}

/* ---- 公开 API ---- */

const json_value_t *json_parse(arena_t *arena, const char *text, size_t len,
                               json_error_t *out_error) {
    if (!arena || !text) {
        if (out_error) {
            out_error->msg  = "invalid arguments";
            out_error->line = 0;
            out_error->col  = 0;
        }
        return NULL;
    }

    json_parser_t p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.text  = text;
    p.len   = len;
    p.pos   = 0;
    p.line  = 1;
    p.col   = 1;

    const json_value_t *root = parse_value(&p);
    if (!root) {
        if (out_error) *out_error = p.error;
        return NULL;
    }

    /* 尾部只允许空白 */
    skip_ws(&p);
    if (!at_end(&p)) {
        if (out_error) {
            out_error->msg  = "trailing data after JSON value";
            out_error->line = p.line;
            out_error->col  = p.col;
        }
        return NULL;
    }

    return root;
}

const json_value_t *json_object_get(const json_value_t *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT || !key) return NULL;
    size_t key_len = strlen(key);
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (obj->u.obj.members[i].key.len == key_len &&
            memcmp(obj->u.obj.members[i].key.ptr, key, key_len) == 0)
            return obj->u.obj.members[i].val;
    }
    return NULL;
}

const json_value_t *json_object_get_slice(const json_value_t *obj,
                                          strslice_t key) {
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strslice_eq(obj->u.obj.members[i].key, key))
            return obj->u.obj.members[i].val;
    }
    return NULL;
}

const json_value_t *json_array_get(const json_value_t *arr, size_t index) {
    if (!arr || arr->kind != JSON_ARRAY) return NULL;
    if (index >= arr->u.arr.count) return NULL;
    return arr->u.arr.items[index];
}

const char *json_as_string(const json_value_t *v) {
    if (!v || v->kind != JSON_STRING) return NULL;
    return v->u.str;
}

bool json_as_bool(const json_value_t *v, bool fallback) {
    if (!v || v->kind != JSON_BOOL) return fallback;
    return v->u.b;
}

double json_as_number(const json_value_t *v, double fallback) {
    if (!v || v->kind != JSON_NUMBER) return fallback;
    return v->u.num;
}
