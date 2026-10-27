#ifndef _H_CLUX_CORE_SHA256_
#define _H_CLUX_CORE_SHA256_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** SHA-256 of a memory buffer. out is 32 raw bytes. */
void clux_sha256(const void *data, size_t len, uint8_t out[32]);

/** SHA-256 of a file (binary). Returns false if the file cannot be read. */
bool clux_sha256_file(const char *path, uint8_t out[32]);

/** Lower-case hex SHA-256 (out must hold 65 bytes). */
void clux_sha256_hex(const void *data, size_t len, char out[65]);

/** Lower-case hex SHA-256 of a file. Returns false if unreadable. */
bool clux_sha256_file_hex(const char *path, char out[65]);

#ifdef __cplusplus
}
#endif
#endif
