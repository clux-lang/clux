#ifndef _H_CLUX_PARSER_LEXER_
#define _H_CLUX_PARSER_LEXER_
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "location.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- Token kinds ---- */

typedef enum {
  TOKEN_TYPE_ERROR, /* lexical error: unrecognized input (message attached
                       to the token; recovery is the pipeline's call) */
  TOKEN_TYPE_IDENTIFIER,
  TOKEN_TYPE_CHARACTER, /* character literal, e.g. 'a' (value type: u8) */
  TOKEN_TYPE_STRING,    /* string literal, e.g. "abc" */
  TOKEN_TYPE_NUMERIC,   /* integer/float literal, incl. base prefix and type
                           suffix; value parsed later by the Parser */
  TOKEN_TYPE_KEYWORD,
  TOKEN_TYPE_SYMBOL,  /* operator or punctuation (maximal munch, 1-2 chars) */
  TOKEN_TYPE_COMMENT, /* line comment: // ... */
  TOKEN_TYPE_MULTILINE_COMMENT, /* block comment: slash-star ... star-slash
                                   (nestable) */
  TOKEN_TYPE_WHITESPACE,        /* one merged run of whitespace */
  TOKEN_TYPE_EOF,
} token_kind_t;

/* ---- Opaque types ---- */

typedef struct _token_t token_t;
typedef struct _lexer_t lexer_t;

/* ---- Lexer lifecycle ---- */

/**
 * Create a lexer over `stream`.
 *
 * The lexer takes ownership of `stream` (closes it on lexer_close).
 * The source must support direct data access (istream_data != NULL),
 * i.e. a memory-backed source; file loading is the caller's job.
 * `filename` is stored by reference (not copied) into every token's
 * location and must outlive the lexer.
 *
 * A leading UTF-8 BOM (EF BB BF), if present, is skipped: the first
 * token starts after it.
 *
 * Returns NULL for invalid arguments or non-direct-access sources.
 * Panics on out-of-memory.
 */
lexer_t *
lexer_create(allocator_t *allocator, istream_t *stream, const char *filename);

/**
 * Close the lexer (closing the underlying istream) and nullify the
 * caller's pointer. No-op if `lexer` or `*lexer` is NULL.
 *
 * Tokens already handed out by lexer_next are owned by whoever holds
 * them; closing the lexer does not free them, but their text slices
 * point into the source and become dangling.
 */
void lexer_close(lexer_t **lexer);

/* ---- Token production ---- */

/**
 * Return the next token. All tokens are produced, including WHITESPACE
 * (consecutive runs merged into one token) and comments. After the
 * input is exhausted, returns TOKEN_TYPE_EOF; repeated calls keep
 * returning EOF (idempotent).
 *
 * The returned token is owned by the caller and must be freed with
 * token_free. Returns NULL only for a NULL lexer.
 *
 * This is the ONLY way to pull tokens: there is no peek, no pending
 * buffer, no checkpoint/rewind. Backtracking is the pipeline's job —
 * it keeps the tokens it has already pulled (see below).
 *
 * Lexical errors do not stop the lexer and are not recorded inside it:
 * an offending input yields exactly one TOKEN_TYPE_ERROR token (message
 * via token_get_error_message) and lexing resumes after it. Deciding
 * whether one error is fatal is entirely up to the pipeline.
 */
token_t *lexer_next(lexer_t *lexer);

/**
 * Building a token pool on top of lexer_next (pipeline side):
 *
 *   vec_t *pool = vec_new(allocator, true);   // owns_element: frees tokens
 *   for (;;) {
 *       token_t *t = lexer_next(lexer);
 *       token_kind_t k = token_get_kind(t);
 *       vec_push(pool, allocator, t);
 *       if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
 *   }
 *
 * The result is a plain vec_t of token_t* in source order — the Parser
 * walks it by index and may jump forwards or backwards freely, with no
 * re-lexing and no lookahead buffer.
 */

/* ---- Token accessors ---- */

/** Return the token kind. TOKEN_TYPE_ERROR for NULL. */
token_kind_t token_get_kind(const token_t *self);

/** Return the token location, or NULL. */
const location_t *token_get_location(const token_t *self);

/**
 * Return a slice of the source text covered by the token (zero-copy,
 * O(1)). The slice is NOT NUL-terminated; its length is written to
 * `*out_len`. The pointer is valid as long as the source (hence the
 * lexer) is alive. Returns NULL (and *out_len = 0) for a NULL token.
 */
const char *token_get_text(const token_t *self, size_t *out_len);

/** Return true if the token text equals `str` (byte-exact). */
bool token_is(const token_t *self, const char *str);

/**
 * Return the message carried by a TOKEN_TYPE_ERROR token, or NULL for
 * any other token (and for NULL). The message is owned by the token.
 */
const char *token_get_error_message(const token_t *self);

/**
 * Return a static, human-readable name for a token kind (e.g.
 * "TOKEN_TYPE_KEYWORD"). Intended for diagnostics and the token-table
 * output; do not free the returned string. Returns "TOKEN_TYPE_UNKNOWN"
 * for an out-of-range kind.
 */
const char *token_kind_name(token_kind_t kind);

/* ---- Token helpers ---- */

/**
 * Create a standalone token with an empty text slice and no message
 * (mainly used internally by the lexer and by tests).
 * Panics on out-of-memory. Returns NULL for invalid args.
 */
token_t *
create_token(allocator_t *allocator, token_kind_t kind, location_t location);

/**
 * Create a SYMBOL token whose text is `text` (e.g. a synthesized
 * shift operator "<<"), allocated entirely from the arena: the token
 * struct and its text copy live in `arena` and are released together
 * with the arena (no per-token allocator tracking, no manual free).
 *
 * Used by the parser to synthesize operators the lexer no longer
 * produces as two-char tokens (shift operators vs nested tuple
 * syntax conflict). Returns NULL for invalid args; panics on OOM.
 */
const token_t *arena_token_symbol(arena_t *arena, const char *text,
                                  const location_t *loc);

/** Free a token and nullify the caller's pointer. No-op if NULL. */
void token_free(allocator_t *allocator, token_t **token);

#ifdef __cplusplus
}
#endif
#endif
