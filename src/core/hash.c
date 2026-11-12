#include "core/hash.h"
uint32_t hash_sdb(const char *str) {
  uint32_t hash = 0;
  while (*str) {
    hash = (*str++) + (hash << 6) + (hash << 16) - hash;
  }
  return hash;
}