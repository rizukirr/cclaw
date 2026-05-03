#ifndef CCLAW_STR_H
#define CCLAW_STR_H

#include <stddef.h>
#include <string.h>

typedef struct {
  char *str;
  size_t len;
} CClawString;

static inline size_t str_skip_escaped(char *src, size_t src_len, char *out) {
  size_t w = 0;
  if (src_len >= 2) {
    for (size_t r = 0; r < src_len; r++) {
      if (src[r] == '\\')
        continue;

      out[w++] = src[r];
    }
    out[w] = '\0';
  }

  return w;
}

static inline CClawString cclaw_str(const char *str) {
  CClawString s = {.str = (char *)str, .len = strlen(str)};
  return s;
}

#endif // CCLAW_STR_H
