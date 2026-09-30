#ifndef _H_CLUX_CORE_ALLOCATOR_
#define _H_CLUX_CORE_ALLOCATOR_
#ifdef __cplusplus
extern "C" {
#endif

/* ---- Primitive function types ---- */

/** Allocation function signature (e.g. malloc). */
typedef void *(alloc_fn_t)(size_t size);

/** Deallocation function signature (e.g. free). */
typedef void(free_fn_t)(void *data);

/* ---- Opaque allocator type ---- */

typedef struct _allocator_t allocator_t;

/* ---- Callback signatures ---- */

/**
 * Called by allocator_free to let an object release internal resources
 * before its memory is reclaimed. `self` is the object being freed.
 * `allocator` may be used to free nested allocations.
 */
typedef void (*dispose_fn_t)(void *self, allocator_t *allocator);

/**
 * Called by allocator_move to transfer ownership of resources from
 * `another` (source) to `self` (destination). After the call,
 * `another` should be in a moved-from state; its memory will be
 * freed immediately afterward.
 */
typedef void (*move_fn_t)(void *self, allocator_t *allocator, void *another);

/**
 * Called by allocator_clone to create a deep copy of `another`
 * (source) into `self` (destination). `another` remains valid.
 */
typedef void (*clone_fn_t)(void *self, allocator_t *allocator, void *another);

/* ---- Runtime type descriptor ---- */

typedef struct _class_t class_t;

struct _class_t {
  const char *
      name; /**< Human-readable type name (must outlive all allocations of this class). */
  size_t size; /**< Size in bytes of a single object. */
  move_fn_t
      move_fn; /**< Move callback, or NULL if the type does not support move. */
  clone_fn_t
      clone_fn; /**< Clone callback, or NULL if the type does not support clone. */
  dispose_fn_t
      dispose_fn; /**< Dispose callback, or NULL if no cleanup is needed. */
};

/* ---- Allocator lifetime ---- */

/**
 * Create a new allocator backed by the given allocation and
 * deallocation functions. Returns NULL if either function pointer
 * is NULL, or panics on out-of-memory.
 */
allocator_t *create_allocator(alloc_fn_t alloc_fn, free_fn_t free_fn);

/**
 * Destroy an allocator and nullify the caller's pointer.
 * No-op if `allocator` or `*allocator` is NULL.
 * Note: does NOT free objects still allocated through this allocator.
 */
void delete_allocator(allocator_t **allocator);

/* ---- Allocation / deallocation ---- */

/**
 * Allocate `count` objects described by `clazz`, zero-initialized.
 * A hidden header is prepended so that allocator_get_class() and
 * allocator_get_count() work on the returned pointer.
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
void *allocator_new(allocator_t *allocator, class_t *clazz, size_t count);

/**
 * Like allocator_new, but builds a heap-allocated class_t from the
 * given parameters. Useful for one-off types without a static class.
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
void *allocator_new_ex(allocator_t *allocator,
                       const char *name,
                       size_t size,
                       move_fn_t move_fn,
                       clone_fn_t clone_fn,
                       dispose_fn_t dispose_fn,
                       size_t count);

/**
 * Free an allocation previously returned by allocator_new / new_ex /
 * move / clone. Calls dispose_fn (if any), then releases memory.
 * Nullifies `*data`. No-op if `allocator`, `data`, or `*data` is NULL.
 */
void allocator_free(allocator_t *allocator, void **data);

/* ---- Move / clone ---- */

/**
 * Move an object: allocate a new instance, call move_fn to transfer
 * ownership, then free the source. The source pointer is nullified.
 * Panics if the type does not support move (move_fn is NULL).
 * Panics on out-of-memory.
 */
void *allocator_move(allocator_t *allocator, void **object);

/**
 * Clone an object: allocate a new instance, call clone_fn to create
 * a deep copy. The source remains valid.
 * Panics if the type does not support clone (clone_fn is NULL).
 * Panics on out-of-memory.
 */
void *allocator_clone(allocator_t *allocator, void **object);

/* ---- Default callbacks for basic (trivially copyable) types ---- */

/**
 * Default move: memcpy from another to self, then memset another to zero.
 * Suitable as move_fn for POD types (e.g. int, double).
 */
void default_move(void *self, allocator_t *allocator, void *another);

/**
 * Default clone: memcpy from another to self.
 * Suitable as clone_fn for POD types.
 */
void default_clone(void *self, allocator_t *allocator, void *another);

/* ---- Accessors ---- */

/** Return the class_t associated with an allocation, or NULL.
 *  The returned pointer is const — modifying a class_t after
 *  objects have been allocated from it leads to undefined behavior. */
const class_t *allocator_get_class(void *data);

/** Return the number of objects in an allocation, or 0. */
size_t allocator_get_count(void *data);

/**
 * Return the number of live allocations still tracked by the allocator,
 * or 0 if `allocator` is NULL. Useful in tests to assert that every
 * allocation has been freed before the allocator is destroyed.
 */
size_t allocator_live_count(allocator_t *allocator);

#ifdef __cplusplus
}
#endif
#endif
