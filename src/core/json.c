#include "core/json.h"
#include "core/location.h"
#include "core/string.h"
#include <stdio.h>
#include <string.h>

#define JSON_MAX_DEPTH 256

typedef enum _json_container_kind_t {
  JSON_CONTAINER_OBJECT,
  JSON_CONTAINER_ARRAY,
} json_container_kind_t;

typedef struct _json_container_t {
  json_container_kind_t kind;
  bool first;
  bool pending_key;
} json_container_t;

struct _json_writer_t {
  allocator_t *allocator;
  string_t *out;
  const char *source;
  json_container_t stack[JSON_MAX_DEPTH];
  size_t depth;
};

static void append_indent(json_writer_t *self, size_t depth) {
  for (size_t i = 0; i < depth; i++) {
    string_append_cstr(self->out, "  ");
  }
}

static void append_escaped(json_writer_t *self, const char *s, size_t len) {
  string_append_char(self->out, '"');
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"':
      string_append_cstr(self->out, "\\\"");
      break;
    case '\\':
      string_append_cstr(self->out, "\\\\");
      break;
    case '\b':
      string_append_cstr(self->out, "\\b");
      break;
    case '\f':
      string_append_cstr(self->out, "\\f");
      break;
    case '\n':
      string_append_cstr(self->out, "\\n");
      break;
    case '\r':
      string_append_cstr(self->out, "\\r");
      break;
    case '\t':
      string_append_cstr(self->out, "\\t");
      break;
    default:
      if (c < 0x20) {
        char buf[8];
        int n = snprintf(buf, sizeof(buf), "\\u%04x", c);
        if (n > 0) {
          string_append(self->out, buf, (size_t)n);
        }
      } else {
        string_append_char(self->out, (char)c);
      }
      break;
    }
  }
  string_append_char(self->out, '"');
}

static void append_cstr_escaped(json_writer_t *self, const char *s) {
  if (!s) {
    string_append_cstr(self->out, "null");
    return;
  }
  append_escaped(self, s, strlen(s));
}

static void before_value(json_writer_t *self) {
  if (self->depth == 0) {
    return;
  }
  json_container_t *top = &self->stack[self->depth - 1];
  if (top->kind == JSON_CONTAINER_ARRAY) {
    if (!top->first) {
      string_append_char(self->out, ',');
    }
    top->first = false;
    string_append_char(self->out, '\n');
    append_indent(self, self->depth);
  } else {
    top->pending_key = false;
  }
}

static void push_container(json_writer_t *self, json_container_kind_t kind) {
  if (self->depth >= JSON_MAX_DEPTH) {
    return;
  }
  json_container_t *entry = &self->stack[self->depth];
  entry->kind = kind;
  entry->first = true;
  entry->pending_key = false;
  self->depth++;
}

static void pop_container(json_writer_t *self, char close) {
  if (self->depth == 0) {
    return;
  }
  self->depth--;
  json_container_t *entry = &self->stack[self->depth];
  if (!entry->first) {
    string_append_char(self->out, '\n');
    append_indent(self, self->depth);
  }
  string_append_char(self->out, close);
}

json_writer_t *create_json_writer(allocator_t *allocator, const char *source) {
  json_writer_t *self = allocator_alloc(allocator, sizeof(json_writer_t));
  self->allocator = allocator;
  self->out = create_string(allocator);
  self->source = source;
  self->depth = 0;
  return self;
}

void delete_json_writer(json_writer_t **self) {
  if (!self || !*self) {
    return;
  }
  json_writer_t *writer = *self;
  *self = NULL;
  allocator_t *allocator = writer->allocator;
  delete_string(&writer->out);
  allocator_free(allocator, (void **)&writer);
}

string_t *json_writer_release(json_writer_t *self) {
  string_t *out = self->out;
  self->out = NULL;
  return out;
}

void json_writer_begin_object(json_writer_t *self) {
  before_value(self);
  string_append_char(self->out, '{');
  push_container(self, JSON_CONTAINER_OBJECT);
}

void json_writer_end_object(json_writer_t *self) { pop_container(self, '}'); }

void json_writer_begin_array(json_writer_t *self) {
  before_value(self);
  string_append_char(self->out, '[');
  push_container(self, JSON_CONTAINER_ARRAY);
}

void json_writer_end_array(json_writer_t *self) { pop_container(self, ']'); }

void json_writer_key(json_writer_t *self, const char *key) {
  if (self->depth == 0) {
    return;
  }
  json_container_t *top = &self->stack[self->depth - 1];
  if (top->kind != JSON_CONTAINER_OBJECT) {
    return;
  }
  if (!top->first) {
    string_append_char(self->out, ',');
  }
  top->first = false;
  string_append_char(self->out, '\n');
  append_indent(self, self->depth);
  append_escaped(self, key, strlen(key));
  string_append_cstr(self->out, ": ");
  top->pending_key = true;
}

static void
write_string_len(json_writer_t *self, const char *value, size_t len) {
  before_value(self);
  append_escaped(self, value, len);
}

void json_writer_string(json_writer_t *self, const char *value) {
  before_value(self);
  append_cstr_escaped(self, value);
}

void json_writer_number(json_writer_t *self, size_t value) {
  before_value(self);
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%zu", value);
  if (n > 0) {
    string_append(self->out, buf, (size_t)n);
  }
}

void json_writer_null(json_writer_t *self) {
  before_value(self);
  string_append_cstr(self->out, "null");
}

void json_writer_field_string(json_writer_t *self,
                              const char *key,
                              const char *value) {
  json_writer_key(self, key);
  json_writer_string(self, value);
}

void json_writer_field_number(json_writer_t *self,
                              const char *key,
                              size_t value) {
  json_writer_key(self, key);
  json_writer_number(self, value);
}

static size_t source_offset(json_writer_t *self, const char *offset) {
  if (!self->source || !offset || offset < self->source) {
    return 0;
  }
  return (size_t)(offset - self->source);
}

static void write_position(json_writer_t *self, const position_t *pos) {
  json_writer_begin_object(self);
  json_writer_field_number(self, "line", pos->line);
  json_writer_field_number(self, "column", pos->column);
  json_writer_field_number(self, "offset", source_offset(self, pos->offset));
  json_writer_end_object(self);
}

void json_writer_field_span(json_writer_t *self, const location_t *loc) {
  json_writer_key(self, "span");
  if (!loc) {
    json_writer_null(self);
    return;
  }
  json_writer_begin_object(self);
  json_writer_field_string(self, "file", loc->filename);
  json_writer_key(self, "begin");
  write_position(self, &loc->begin);
  json_writer_key(self, "end");
  write_position(self, &loc->end);
  json_writer_end_object(self);
}

void json_writer_field_text(json_writer_t *self, const location_t *loc) {
  json_writer_key(self, "text");
  if (!self->source || !loc || !loc->begin.offset || !loc->end.offset ||
      loc->end.offset < loc->begin.offset) {
    json_writer_null(self);
    return;
  }
  write_string_len(
      self, loc->begin.offset, (size_t)(loc->end.offset - loc->begin.offset));
}
