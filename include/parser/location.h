#ifndef _H_CLUX_PARSER_LOCATION_
#define _H_CLUX_PARSER_LOCATION_
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _position_t {
  size_t offset;
  size_t column;
  size_t line;
} position_t;
typedef struct _location_t {
  position_t begin;
  position_t end;
  const char *filename;
} location_t;
#ifdef __cplusplus
}
#endif
#endif