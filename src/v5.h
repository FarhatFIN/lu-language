#ifndef LU_V5_H
#define LU_V5_H

#include <stddef.h>

/* True when the source carries the Lu/Strict marker on its first
   meaningful line. */
int v5_is_strict(const char *src);

/* Transpile a Lu/Strict (v5) program into plain Lu (v4) text.
   Returns a malloc'd buffer on success, NULL on failure with a
   human-readable diagnostic in err[]. */
char *v5_transpile(const char *path, const char *src, char *err, size_t errcap);

#endif /* LU_V5_H */
