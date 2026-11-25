/* 渲染器（逆向编排两条通道中的新建通道之一，设计文档 §4 ②）：
   sema 常量值 / 合成文本 → 全新 AST 节点。产物分配在 out_allocator，
   文本经 program_intern_string 落 program 常量表；location 沿引用点的
   line/col/filename，offset 改指 intern 文本（span 无源码地址，与
   instants 折叠同约定）。 */
#include "sema/sema_internal.h"
#include "core/program.h"
#include "parser/node.h"
#include <stdio.h>
#include <string.h>

const char *
sema_render_strip_suffix(const char *text, size_t len, size_t *num_len) {
  static const char *suffixes[] = {"isize",
                                   "usize",
                                   "i8",
                                   "i16",
                                   "i32",
                                   "i64",
                                   "u8",
                                   "u16",
                                   "u32",
                                   "u64",
                                   "f32",
                                   "f64",
                                   NULL};
  for (size_t idx = 0; suffixes[idx] != NULL; idx++) {
    size_t slen = strlen(suffixes[idx]);
    if (len > slen && strncmp(text + len - slen, suffixes[idx], slen) == 0) {
      *num_len = len - slen;
      return suffixes[idx];
    }
  }
  *num_len = len;
  return NULL;
}

node_t *sema_render_text_node(allocator_t *out,
                              program_t *program,
                              node_kind_t kind,
                              const char *text,
                              const location_t *loc) {
  size_t len = strlen(text);
  const char *stored = program_intern_string(program, text, len);
  node_t *node = create_node(out, kind);
  location_t rendered = *loc;
  rendered.begin.offset = stored;
  rendered.end.offset = stored + len;
  node_set_location(node, &rendered);
  return node;
}

node_t *sema_render_const(allocator_t *out,
                          program_t *program,
                          const sema_const_t *value,
                          const location_t *loc) {
  char buffer[64];
  node_kind_t kind = CLUX_NODE_LITERAL_NUMERIC;
  const char *text = buffer;
  switch (value->kind) {
  case SEMA_CONST_BOOL:
    kind = CLUX_NODE_LITERAL_BOOLEAN;
    text = value->bool_value ? "true" : "false";
    break;
  case SEMA_CONST_FLOAT: {
    /* %.17g 往返 f64；语法层浮点字面量须带小数点或指数，%g 的整数值
       形态（"1"）补 ".0"，随后拼 f 族后缀（CTFE 求值必带，兜底 f64）。 */
    const char *suffix = value->suffix ? value->suffix : "f64";
    int len = snprintf(buffer, sizeof(buffer), "%.17g", value->float_value);
    bool needs_dot = len > 0 && !strchr(buffer, '.') && !strchr(buffer, 'e') &&
                     !strchr(buffer, 'E');
    if ((size_t)len + (needs_dot ? 2 : 0) + strlen(suffix) >= sizeof(buffer)) {
      return NULL;
    }
    if (needs_dot) {
      buffer[len] = '.';
      buffer[len + 1] = '\0';
    }
    strcat(buffer, suffix);
    break;
  }
  case SEMA_CONST_INT:
    snprintf(buffer,
             sizeof(buffer),
             "%s%llu%s",
             value->negative ? "-" : "",
             (unsigned long long)value->magnitude,
             value->suffix ? value->suffix : "i32");
    break;
  }
  return sema_render_text_node(out, program, kind, text, loc);
}
