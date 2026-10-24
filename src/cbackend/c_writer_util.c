#include "cbackend/c_writer_internal.h"

void cw_put(string_t *sb, const char *s) { string_append_cstr(sb, s); }

void cw_printf(string_t *sb, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    string_append_cstr(sb, buf);
}

void cw_write_c_bytes(string_t *sb, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\\' || c == '"') {
            string_append_char(sb, '\\');
            string_append_char(sb, (char)c);
        } else if (c >= 0x20 && c < 0x7f) {
            string_append_char(sb, (char)c);
        } else {
            char buf[8];
            snprintf(buf, sizeof buf, "\\%03o", c);
            string_append_cstr(sb, buf);
        }
    }
}

void cw_write_c_string(string_t *sb, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\' || *p == '"') string_append_char(sb, '\\');
        string_append_char(sb, *p);
    }
}

/* `struct clux_s_5` → `clux_s_5`：bitfield 构造辅助函数名的后缀。 */
const char *cw_bf_helper_suffix(const cir_type_t *t) {
    const char *name = t && t->c_name ? t->c_name : "clux_s_bitfield";
    if (strncmp(name, "struct ", 7) == 0) name += 7;
    return name;
}
