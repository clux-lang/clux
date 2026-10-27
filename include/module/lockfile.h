#ifndef _H_CLUX_MODULE_LOCKFILE_
#define _H_CLUX_MODULE_LOCKFILE_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/arena.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * Generate `<root>/clux.lock` from the current package graph.
 *
 * Records each package's clux.mod hash, postinstall command/script hashes and
 * hashes of the native artifacts referenced by native.link/runtime. Fails if
 * a referenced artifact (or an inferred postinstall script) is missing.
 */
bool clux_lock_write(allocator_t *alloc, arena_t *arena, const char *root,
                     char *err, size_t errcap);

/**
 * Verify `<root>/clux.lock` against the current package graph and artifacts.
 * Returns false with a human-readable reason in err.
 */
bool clux_lock_verify(allocator_t *alloc, arena_t *arena, const char *root,
                      char *err, size_t errcap);

/**
 * Verify the lockfile for the package containing `src` (a file or directory).
 * Creates its own allocator/arena.
 */
bool clux_lock_verify_for_source(const char *src, char *err, size_t errcap);

/**
 * Infer the postinstall script file from a command (sh build.sh,
 * powershell -File build.ps1, cmd /c build.bat, or a direct .sh/.bat path)
 * and hash it. Returns true with empty script_path if no script is inferable;
 * false if an inferred script is missing/unreadable.
 */
bool clux_lock_postinstall_script(const char *root, const char *command,
                                  char *script_path, size_t path_cap,
                                  char *script_hash, size_t hash_cap);

#ifdef __cplusplus
}
#endif
#endif
