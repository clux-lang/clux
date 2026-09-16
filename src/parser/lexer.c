#include "parser/lexer.h"
#include "core/allocator.h"
#include "parser/location.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unicode/uchar.h>

/* ---- Internal: token_t definition ---- */

struct _token_t {
  token_kind_t kind;
  location_t location;
  const char *text; /* zero-copy slice into the source (not NUL-terminated) */
  size_t length;
  char *message; /* TOKEN_TYPE_ERROR only: owned, NUL-terminated */
};

/* ---- Internal: lexer_t definition ---- */

struct _lexer_t {
  allocator_t *allocator;
  istream_t *stream;       /* owned; closed by lexer_close */
  const char *filename;    /* borrowed, must outlive the lexer */
  const char *source_data; /* borrowed from istream (data accessor) */
  size_t source_len;
};

/* ---- Internal: class for token_t ---- */

static void token_dispose(void *self, allocator_t *allocator);

static class_t g_token_class = {
    .name = "clux.parser.token",
    .size = sizeof(struct _token_t),
    /* No clone: a shallow copy would share `message` and double-free it. */
    .clone_fn = NULL,
    .move_fn = default_move,
    .dispose_fn = token_dispose,
};

/* ---- Internal: class for the error message buffer ---- */

static class_t g_char_class = {
    .name = "char",
    .size = sizeof(char),
    .clone_fn = default_clone,
    .move_fn = default_move,
    .dispose_fn = NULL,
};

/* ---- Internal: class for lexer_t ---- */

static token_t *lexer_read_token(lexer_t *lexer);
static void lexer_dispose(void *self, allocator_t *allocator);
static void lexer_move_cb(void *self, allocator_t *allocator, void *another);

static class_t lexer_class = {
    .name = "clux.parser.lexer",
    .size = sizeof(lexer_t),
    .move_fn = lexer_move_cb,
    .clone_fn = NULL, /* a lexer wraps a consuming stream; not cloneable */
    .dispose_fn = lexer_dispose,
};

/* ---- Internal: keyword table (M1 language keywords) ---- */

static const char *const g_keywords[] = {
    "as",      "bool",    "break",     "comptime",  "const",   "continue",
    "else",    "extends", "f32",       "f64",       "false",   "for",
    "func",    "i16",     "i32",       "i64",       "i8",      "if",
    "return",  "str",     "true",      "type",      "u16",     "u32",
    "u64",     "u8",      "undefined", "var",       "void",    "volatile",
    "while",
};

/* ---- Internal: character classes ---- */

/**
 * Identifier rules follow the Unicode identifier profile (spec 2.5):
 * a codepoint with the ID_Start property, or '_' as a special case
 * (Unicode classifies '_' as ID_Continue only). Continuation codepoints
 * use ICU's ID_Part test (ID_Continue, extended for identifiers).
 * -1 (EOF) never matches.
 */
static bool is_ident_start(UChar32 cp) {
  if (cp <= 0) return false;
  if (cp == '_') return true;
  return u_isIDStart(cp) ? true : false;
}

static bool is_ident_char(UChar32 cp) {
  if (cp <= 0) return false;
  return is_ident_start(cp) || (u_isIDPart(cp) ? true : false);
}

static bool is_whitespace(UChar32 cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n';
}

static bool is_digit_in_base(UChar32 cp, int base) {
  if (cp >= '0' && cp <= '9') return (cp - '0') < base;
  if (base == 16 && cp >= 'a' && cp <= 'f') return true;
  if (base == 16 && cp >= 'A' && cp <= 'F') return true;
  return false;
}

/* ---- Internal: keyword lookup (linear scan; 29 entries) ---- */

static bool lookup_keyword(const char *text, size_t len) {
  for (size_t i = 0; i < sizeof(g_keywords) / sizeof(g_keywords[0]); i++) {
    const char *kw = g_keywords[i];
    if (strlen(kw) == len && memcmp(text, kw, len) == 0) return true;
  }
  return false;
}

/* ---- Internal: build a location from begin/end stream positions ---- */

static location_t
make_location(const lexer_t *lexer, stream_pos_t begin, stream_pos_t end) {
  location_t loc;
  loc.begin.offset = begin.byte_offset;
  loc.begin.line = begin.line;
  loc.begin.column = begin.cluster_col; /* grapheme-cluster column */
  loc.end.offset = end.byte_offset;
  loc.end.line = end.line;
  loc.end.column = end.cluster_col;
  loc.filename = lexer->filename;
  return loc;
}

/* ---- Internal: token construction ---- */

/**
 * Build a token of `kind` spanning [begin, end) and attach its
 * zero-copy source slice, so the token is self-describing once it sits
 * in the pool (no need to carry the lexer around to read its text).
 */
static token_t *lexer_make_token(lexer_t *lexer,
                                 token_kind_t kind,
                                 stream_pos_t begin,
                                 stream_pos_t end) {
  token_t *token =
      create_token(lexer->allocator, kind, make_location(lexer, begin, end));
  if (!token) return NULL;
  size_t start = begin.byte_offset;
  size_t stop = end.byte_offset;
  if (stop < start) stop = start;
  if (start > lexer->source_len) start = lexer->source_len;
  if (stop > lexer->source_len) stop = lexer->source_len;
  token->text = lexer->source_data + start;
  token->length = stop - start;
  return token;
}

/* ---- Internal: lexical error ---- */

/**
 * Build a TOKEN_TYPE_ERROR token covering the error position, with the
 * formatted message attached to it.
 *
 * The lexer keeps NO error state: it neither stops nor remembers that
 * anything went wrong. The offending input has already been consumed by
 * the caller (every scanner consumes at least one codepoint before
 * failing), so lexing simply resumes after the error token — whether
 * that is acceptable is the pipeline's decision.
 */
static token_t *lexer_fail(lexer_t *lexer, stream_pos_t at, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  token_t *token = lexer_make_token(lexer, TOKEN_TYPE_ERROR, at, at);
  if (!token) return NULL;

  size_t n = strlen(buf) + 1;
  char *message = (char *)allocator_new(lexer->allocator, &g_char_class, n);
  memcpy(message, buf, n);
  token->message = message;
  return token;
}

/* ---- Internal: escape sequences (spec 2.6) ---- */

static bool is_hex_digit(UChar32 cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') ||
         (cp >= 'A' && cp <= 'F');
}

/** True for the simple escapes: \n \t \r \\ \' \" \0 */
static bool is_simple_escape(UChar32 cp) {
  switch (cp) {
  case 'n':
  case 't':
  case 'r':
  case '\\':
  case '\'':
  case '"':
  case '0':
    return true;
  default:
    return false;
  }
}

/**
 * Consume the body of an escape sequence, i.e. everything after the
 * backslash that has already been read: either a simple escape or
 * '\xHH' with 1-2 hex digits.
 *
 * Returns NULL on success, or a fatal (fail-fast) error token:
 *   - "\\" at EOF      -> dangling backslash
 *   - "\\" + newline   -> newline in literal
 *   - unknown escape   -> invalid escape sequence
 *   - "\\x" + non-hex  -> invalid hex escape
 */
static token_t *lexer_read_escape(lexer_t *lexer,
                                  stream_pos_t begin,
                                  const char *what) {
  UChar32 e = istream_read_cp(lexer->stream);
  if (e == -1)
    return lexer_fail(
        lexer, begin, "unterminated %s (dangling backslash)", what);
  if (e == '\n' || e == '\r')
    return lexer_fail(lexer, begin, "unterminated %s (newline in literal)", what);

  if (e == 'x') { /* \xHH: 1-2 hex digits */
    UChar32 h = istream_peek_cp(lexer->stream);
    if (h == -1 || !is_hex_digit(h))
      return lexer_fail(
          lexer, begin, "invalid hex escape in %s: expected hex digit", what);
    istream_read_cp(lexer->stream);
    if (is_hex_digit(istream_peek_cp(lexer->stream)))
      istream_read_cp(lexer->stream); /* optional second digit */
    return NULL;
  }

  if (!is_simple_escape(e))
    return lexer_fail(
        lexer, begin, "invalid escape sequence in %s (U+%04X)", what, (int)e);
  return NULL;
}

/* ---- Lexer lifecycle ---- */

lexer_t *
lexer_create(allocator_t *allocator, istream_t *stream, const char *filename) {
  if (!allocator || !stream) return NULL;
  const char *data = istream_data(stream);
  if (!data)
    return NULL; /* requires a source with direct data access (mem-backed) */

  lexer_t *lexer = (lexer_t *)allocator_new(allocator, &lexer_class, 1);
  lexer->allocator = allocator;
  lexer->stream = stream;
  lexer->filename = filename;
  lexer->source_data = data;
  lexer->source_len = istream_size(stream);
  /* Skip a leading UTF-8 BOM (EF BB BF): it belongs to no token and must
   * not turn into an "unrecognized character" error on Windows sources. */
  if (lexer->source_len >= 3 && (unsigned char)data[0] == 0xEF &&
      (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
    istream_seek(stream, 3);
  return lexer;
}

void lexer_close(lexer_t **lexer) {
  if (!lexer || !*lexer) return;
  allocator_free((*lexer)->allocator, (void **)lexer);
}

/* ---- Token production ---- */

token_t *lexer_next(lexer_t *lexer) {
  if (!lexer) return NULL;
  return lexer_read_token(lexer);
}

/* ---- Internal: numeric literals ---- */

static token_t *lexer_read_number(lexer_t *lexer, stream_pos_t begin) {
  int base = 10;
  UChar32 cp = istream_read_cp(lexer->stream); /* leading digit */

  /* 0x / 0o / 0b base prefix */
  if (cp == '0') {
    UChar32 nx = istream_peek_cp(lexer->stream);
    if (nx == 'x' || nx == 'X') {
      base = 16;
      istream_read_cp(lexer->stream);
    } else if (nx == 'o' || nx == 'O') {
      base = 8;
      istream_read_cp(lexer->stream);
    } else if (nx == 'b' || nx == 'B') {
      base = 2;
      istream_read_cp(lexer->stream);
    }
  }

  /* integer digits */
  for (;;) {
    cp = istream_peek_cp(lexer->stream);
    if (cp == -1 || !is_digit_in_base(cp, base)) break;
    istream_read_cp(lexer->stream);
  }

  if (base != 10) {
    /* non-decimal literals need at least one digit after the prefix */
    if (istream_tell(lexer->stream).byte_offset == begin.byte_offset + 2)
      return lexer_fail(
          lexer, begin, "invalid numeric literal: no digits after prefix");
    goto done;
  }

  /* fractional part */
  cp = istream_peek_cp(lexer->stream);
  if (cp == '.') {
    istream_read_cp(lexer->stream);
    UChar32 d = istream_peek_cp(lexer->stream);
    if (d == -1 || !is_digit_in_base(d, 10))
      return lexer_fail(lexer, begin, "expected digit after decimal point");
    for (;;) {
      d = istream_peek_cp(lexer->stream);
      if (d == -1 || !is_digit_in_base(d, 10)) break;
      istream_read_cp(lexer->stream);
    }
  }

  /* exponent */
  cp = istream_peek_cp(lexer->stream);
  if (cp == 'e' || cp == 'E') {
    istream_read_cp(lexer->stream);
    UChar32 s = istream_peek_cp(lexer->stream);
    if (s == '+' || s == '-') istream_read_cp(lexer->stream);
    UChar32 d = istream_peek_cp(lexer->stream);
    if (d == -1 || !is_digit_in_base(d, 10))
      return lexer_fail(lexer, begin, "expected digit in exponent");
    for (;;) {
      d = istream_peek_cp(lexer->stream);
      if (d == -1 || !is_digit_in_base(d, 10)) break;
      istream_read_cp(lexer->stream);
    }
  }

done:
  stream_pos_t end = istream_tell(lexer->stream);
  return lexer_make_token(lexer, TOKEN_TYPE_NUMERIC, begin, end);
}

/* ---- Internal: string literals ---- */

static token_t *lexer_read_string(lexer_t *lexer, stream_pos_t begin) {
  istream_read_cp(lexer->stream); /* consume '"' */
  for (;;) {
    UChar32 cp = istream_read_cp(lexer->stream);
    if (cp == -1)
      return lexer_fail(lexer, begin, "unterminated string literal");
    if (cp == '"') break;
    if (cp == '\\') {
      token_t *err = lexer_read_escape(lexer, begin, "string literal");
      if (err) return err;
    } else if (cp == '\n' || cp == '\r') {
      return lexer_fail(
          lexer, begin, "unterminated string literal (newline in string)");
    }
  }
  stream_pos_t end = istream_tell(lexer->stream);
  return lexer_make_token(lexer, TOKEN_TYPE_STRING, begin, end);
}

/* ---- Internal: character literals (spec 2.6: exactly one character) ---- */

static token_t *lexer_read_char(lexer_t *lexer, stream_pos_t begin) {
  istream_read_cp(lexer->stream); /* consume '\'' */
  UChar32 cp = istream_read_cp(lexer->stream);
  if (cp == -1)
    return lexer_fail(lexer, begin, "unterminated character literal");
  if (cp == '\'') return lexer_fail(lexer, begin, "empty character literal");
  if (cp == '\n' || cp == '\r')
    return lexer_fail(
        lexer, begin, "unterminated character literal (newline in literal)");

  if (cp == '\\') {
    token_t *err = lexer_read_escape(lexer, begin, "character literal");
    if (err) return err;
  } else if (cp > 0xFF) {
    /* character literals have value type u8 (spec 2.6) */
    return lexer_fail(lexer,
                      begin,
                      "character literal out of range for u8 (U+%04X)",
                      (int)cp);
  }

  /* exactly one character: the next codepoint must be the closing quote */
  UChar32 close = istream_read_cp(lexer->stream);
  if (close != '\'') {
    if (close == -1)
      return lexer_fail(lexer, begin, "unterminated character literal");
    return lexer_fail(
        lexer, begin, "character literal must contain exactly one character");
  }

  stream_pos_t end = istream_tell(lexer->stream);
  return lexer_make_token(lexer, TOKEN_TYPE_CHARACTER, begin, end);
}

/* ---- Internal: '/' dispatch: comments and symbol ---- */

static token_t *lexer_read_slash(lexer_t *lexer, stream_pos_t begin) {
  /* leading '/' is peeked by the caller; consume it */
  istream_read_cp(lexer->stream);
  UChar32 cp = istream_peek_cp(lexer->stream);

  /* line comment: // ... up to (not incl.) CR, LF or EOF. CR is not part
   * of the comment so that CRLF sources keep "\r\n" as pure whitespace. */
  if (cp == '/') {
    istream_read_cp(lexer->stream);
    for (;;) {
      cp = istream_peek_cp(lexer->stream);
      if (cp == -1 || cp == '\n' || cp == '\r') break;
      istream_read_cp(lexer->stream);
    }
    stream_pos_t end = istream_tell(lexer->stream);
    return lexer_make_token(lexer, TOKEN_TYPE_COMMENT, begin, end);
  }

  if (cp == '*') { /* block comment: slash-star ... star-slash, nestable */
    istream_read_cp(lexer->stream);
    int depth = 1;
    for (;;) {
      cp = istream_read_cp(lexer->stream);
      if (cp == -1)
        return lexer_fail(lexer, begin, "unterminated block comment");
      if (cp == '/') {
        if (istream_peek_cp(lexer->stream) == '*') {
          istream_read_cp(lexer->stream);
          depth++;
        }
      } else if (cp == '*') {
        if (istream_peek_cp(lexer->stream) == '/') {
          istream_read_cp(lexer->stream);
          if (--depth == 0) break;
        }
      }
    }
    stream_pos_t end = istream_tell(lexer->stream);
    return lexer_make_token(lexer, TOKEN_TYPE_MULTILINE_COMMENT, begin, end);
  }

  if (cp == '=') { /* '/=' */
    istream_read_cp(lexer->stream);
    stream_pos_t end = istream_tell(lexer->stream);
    return lexer_make_token(lexer, TOKEN_TYPE_SYMBOL, begin, end);
  }

  /* single '/' */
  stream_pos_t end = istream_tell(lexer->stream);
  return lexer_make_token(lexer, TOKEN_TYPE_SYMBOL, begin, end);
}

/* ---- Internal: symbols (maximal munch) ---- */

static bool is_single_symbol(UChar32 c) {
  switch (c) {
  case '+':
  case '-':
  case '*':
  case '%':
  case '<':
  case '>':
  case '=':
  case '!':
  case '&':
  case '|':
  case '^':
  case '~':
  case '(':
  case ')':
  case '{':
  case '}':
  case '[':
  case ']':
  case ';':
  case ',':
  case ':':
  case '.':
  case '?':
    return true;
  default:
    return false;
  }
}

static token_t *lexer_read_symbol(lexer_t *lexer, stream_pos_t begin) {
  static const char *const kPairs[] = {
      "<<",
      ">>",
      "<=",
      ">=",
      "==",
      "!=",
      "&&",
      "||",
      "+=",
      "-=",
      "*=",
      "/=",
      "%=",
  };
  UChar32 c1 = istream_read_cp(lexer->stream);
  UChar32 c2 = istream_peek_cp(lexer->stream);

  if (c2 != -1) {
    for (size_t i = 0; i < sizeof(kPairs) / sizeof(kPairs[0]); i++) {
      if ((UChar32)kPairs[i][0] == c1 && (UChar32)kPairs[i][1] == c2) {
        istream_read_cp(lexer->stream);
        stream_pos_t end = istream_tell(lexer->stream);
        return lexer_make_token(lexer, TOKEN_TYPE_SYMBOL, begin, end);
      }
    }
  }

  if (is_single_symbol(c1)) {
    stream_pos_t end = istream_tell(lexer->stream);
    return lexer_make_token(lexer, TOKEN_TYPE_SYMBOL, begin, end);
  }

  return lexer_fail(lexer, begin, "unrecognized character U+%04X", (int)c1);
}

/* ---- Internal: read the next raw token from the stream ---- */

static token_t *lexer_read_token(lexer_t *lexer) {
  stream_pos_t begin = istream_tell(lexer->stream);
  if (istream_at_end(lexer->stream))
    return lexer_make_token(lexer, TOKEN_TYPE_EOF, begin, begin);

  UChar32 cp = istream_peek_cp(lexer->stream);

  if (is_whitespace(cp)) {
    /* Consume and merge the whole run of whitespace into one token. */
    while (is_whitespace(cp)) {
      istream_read_cp(lexer->stream);
      cp = istream_peek_cp(lexer->stream);
      if (cp == -1) break;
    }
    stream_pos_t end = istream_tell(lexer->stream);
    return lexer_make_token(lexer, TOKEN_TYPE_WHITESPACE, begin, end);
  }

  if (is_ident_start(cp)) {
    istream_read_cp(lexer->stream);
    for (;;) {
      cp = istream_peek_cp(lexer->stream);
      if (cp == -1 || !is_ident_char(cp)) break;
      istream_read_cp(lexer->stream);
    }
    stream_pos_t end = istream_tell(lexer->stream);
    size_t len = end.byte_offset - begin.byte_offset;
    const char *text = lexer->source_data + begin.byte_offset;
    token_kind_t kind =
        lookup_keyword(text, len) ? TOKEN_TYPE_KEYWORD : TOKEN_TYPE_IDENTIFIER;
    return lexer_make_token(lexer, kind, begin, end);
  }

  if (cp >= '0' && cp <= '9') return lexer_read_number(lexer, begin);

  switch (cp) {
  case '"':
    return lexer_read_string(lexer, begin);
  case '\'':
    return lexer_read_char(lexer, begin);
  case '/':
    return lexer_read_slash(lexer, begin);
  default:
    return lexer_read_symbol(lexer, begin);
  }
}

/* ---- Token accessors ---- */

token_kind_t token_get_kind(const token_t *self) {
  if (!self) return TOKEN_TYPE_ERROR;
  return self->kind;
}

const location_t *token_get_location(const token_t *self) {
  if (!self) return NULL;
  return &self->location;
}

const char *token_get_text(const token_t *self, size_t *out_len) {
  if (!self) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  if (out_len) *out_len = self->length;
  return self->text;
}

bool token_is(const token_t *self, const char *str) {
  if (!self || !str) return false;
  size_t len = strlen(str);
  if (self->length != len) return false;
  if (len == 0) return true;
  if (!self->text) return false;
  return memcmp(self->text, str, len) == 0;
}

const char *token_get_error_message(const token_t *self) {
  if (!self) return NULL;
  return self->message;
}

/* ---- Token kind name (static, for diagnostics / token-table output) ---- */

static const char *const g_token_kind_names[] = {
    "TOKEN_TYPE_ERROR",
    "TOKEN_TYPE_IDENTIFIER",
    "TOKEN_TYPE_CHARACTER",
    "TOKEN_TYPE_STRING",
    "TOKEN_TYPE_NUMERIC",
    "TOKEN_TYPE_KEYWORD",
    "TOKEN_TYPE_SYMBOL",
    "TOKEN_TYPE_COMMENT",
    "TOKEN_TYPE_MULTILINE_COMMENT",
    "TOKEN_TYPE_WHITESPACE",
    "TOKEN_TYPE_EOF",
};

const char *token_kind_name(token_kind_t kind) {
  if (kind < 0 ||
      (size_t)kind >= sizeof(g_token_kind_names) / sizeof(g_token_kind_names[0]))
    return "TOKEN_TYPE_UNKNOWN";
  return g_token_kind_names[(size_t)kind];
}

/* ---- Token helpers ---- */

token_t *
create_token(allocator_t *allocator, token_kind_t kind, location_t location) {
  if (!allocator) return NULL;
  token_t *token = (token_t *)allocator_new(allocator, &g_token_class, 1);
  token->kind = kind;
  token->location = location;
  token->text = NULL;
  token->length = 0;
  return token;
}

void token_free(allocator_t *allocator, token_t **token) {
  if (!allocator || !token || !*token) return;
  allocator_free(allocator, (void **)token);
}

/* ---- Callbacks for token_class ---- */

static void token_dispose(void *self, allocator_t *allocator) {
  token_t *token = (token_t *)self;
  if (!token) return;
  if (token->message) {
    /* message was allocated via g_char_class; free each element (n chars). */
    allocator_free(allocator, (void **)&token->message);
    token->message = NULL;
  }
}

/* ---- Callbacks for lexer_class ---- */

static void lexer_dispose(void *self, allocator_t *allocator) {
  (void)allocator;
  lexer_t *lexer = (lexer_t *)self;
  if (!lexer) return;
  if (lexer->stream) {
    istream_t *s = lexer->stream;
    istream_close(&s);
    lexer->stream = NULL;
  }
  lexer->allocator = NULL;
  lexer->filename = NULL;
  lexer->source_data = NULL;
  lexer->source_len = 0;
}

static void lexer_move_cb(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  lexer_t *dst = (lexer_t *)self;
  lexer_t *src = (lexer_t *)another;
  if (!dst || !src) return;

  dst->allocator = src->allocator;
  dst->stream = src->stream;
  dst->filename = src->filename;
  dst->source_data = src->source_data;
  dst->source_len = src->source_len;

  src->allocator = NULL;
  src->stream = NULL;
  src->filename = NULL;
  src->source_data = NULL;
  src->source_len = 0;
}
