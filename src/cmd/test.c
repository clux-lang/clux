#include "cmd/test.h"
#include "cmd/package.h"
#include "driver/driver.h"

#include <stdio.h>
#include <stdlib.h>

int cmd_test(const cmd_args_t *args) {
  if (!args) return 1;
  const char *path = args->posc > 0 ? args->posargs[0] : NULL;
  if (path) return driver_test_file(path);

  char *entry = NULL;
  char *root = NULL;
  int rc = cmd_resolve_entry(NULL, &entry, &root);
  if (rc != 0) return rc;
  free(entry);

  char **sources = NULL;
  size_t count = 0;
  rc = cmd_collect_sources(root, &sources, &count);
  free(root);
  if (rc != 0) return rc;
  rc = driver_test_sources((const char *const *)sources, count);
  for (size_t i = 0; i < count; i++) free(sources[i]);
  free(sources);
  return rc;
}
