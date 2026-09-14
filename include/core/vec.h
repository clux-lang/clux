#ifndef _H_CLUX_CORE_VEC_
#define _H_CLUX_CORE_VEC_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include <stdbool.h>

/* ---- Opaque vec type ---- */

typedef struct _vec_t vec_t;

/* ---- Construction / destruction ---- */

/**
 * Create a new empty vec with initial capacity 0.
 * If `owns_element` is true, vec_free will call allocator_free on each
 * element, and vec_clone will call allocator_clone on each element.
 * If false, the vec only stores pointers without managing their lifetime.
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
vec_t *vec_new(allocator_t *allocator, bool owns_element);

/**
 * Create a new vec with at least `capacity` slots pre-allocated.
 * Same ownership semantics as vec_new.
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
vec_t *
vec_with_capacity(allocator_t *allocator, bool owns_element, size_t capacity);

/**
 * Free the vec and nullify the caller's pointer.
 * If owns_element is true, calls allocator_free on each element first.
 * No-op if `vec` or `*vec` is NULL.
 */
void vec_free(allocator_t *allocator, vec_t **vec);

/* ---- Element access ---- */

/**
 * Return the element at `index`, or NULL if index is out of bounds.
 */
void *vec_get(const vec_t *vec, size_t index);

/**
 * Set the element at `index` to `value`, returning the previous value.
 * Returns NULL if index is out of bounds (no modification).
 * Note: the previous value is NOT freed, even if owns_element is true.
 *       The caller is responsible for managing the returned pointer.
 */
void *vec_set(vec_t *vec, size_t index, void *value);

/**
 * Return the first element, or NULL if the vec is empty.
 */
void *vec_first(const vec_t *vec);

/**
 * Return the last element, or NULL if the vec is empty.
 */
void *vec_last(const vec_t *vec);

/* ---- Insertion / removal ---- */

/**
 * Append `value` to the end of the vec. Grows the internal array if needed.
 * Panics on out-of-memory. No-op if `vec` or `value` is NULL.
 */
void vec_push(vec_t *vec, allocator_t *allocator, void *value);

/**
 * Remove and return the last element. Returns NULL if the vec is empty.
 * The returned pointer is NOT freed, even if owns_element is true;
 * ownership transfers to the caller.
 */
void *vec_pop(vec_t *vec);

/**
 * Insert `value` at `index`, shifting elements [index, len) right by one.
 * Panics on out-of-memory. No-op if `vec` or `value` is NULL.
 * Panics if `index` > vec_len(vec).
 */
void vec_insert(vec_t *vec, allocator_t *allocator, size_t index, void *value);

/**
 * Remove and return the element at `index`, shifting elements [index+1, len)
 * left by one. Returns NULL if index is out of bounds.
 * The returned pointer is NOT freed, even if owns_element is true;
 * ownership transfers to the caller.
 */
void *vec_remove(vec_t *vec, size_t index);

/**
 * Remove and return the element at `index`, replacing it with the last
 * element. Does NOT preserve order. O(1) complexity.
 * Returns NULL if index is out of bounds.
 * The returned pointer is NOT freed, even if owns_element is true;
 * ownership transfers to the caller.
 */
void *vec_swap_remove(vec_t *vec, size_t index);

/* ---- Capacity ---- */

/**
 * Return the number of elements currently in the vec.
 */
size_t vec_len(const vec_t *vec);

/**
 * Return the current capacity (number of allocated slots).
 */
size_t vec_cap(const vec_t *vec);

/**
 * Return true if the vec contains no elements.
 */
bool vec_is_empty(const vec_t *vec);

/**
 * Reserve enough capacity for at least `additional` more elements.
 * May reallocate the internal array. Panics on out-of-memory.
 * No-op if `vec` is NULL or the existing capacity is sufficient.
 */
void vec_reserve(vec_t *vec, allocator_t *allocator, size_t additional);

/**
 * Shrink the internal array capacity to fit the current length.
 * May reallocate to a smaller block. Panics on out-of-memory.
 * No-op if `vec` is NULL or already fits.
 */
void vec_shrink_to_fit(vec_t *vec, allocator_t *allocator);

/**
 * Resize the vec to `new_len` elements. If growing, new slots are filled
 * with NULL (the vec grows length without touching existing elements).
 * Shrinking beyond the current length is a no-op.
 * Panics on out-of-memory. No-op if `vec` is NULL.
 */
void vec_resize(vec_t *vec, allocator_t *allocator, size_t new_len);

/* ---- Ownership query ---- */

/**
 * Return true if the vec owns its elements (will free them on vec_free
 * and clone them on allocator_clone).
 */
bool vec_owns_element(const vec_t *vec);

#ifdef __cplusplus
}
#endif
#endif
