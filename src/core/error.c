#include "cclaw/error.h"
#include "cclaw/str.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_ERROR_LEN 1024

static char error_buff[MAX_ERROR_LEN];
static CClawStatus error_status;
static CClawString error_content;

void cclaw_strerror(CClawStatus status, const char *message, ...) {
  va_list args;
  va_start(args, message);
  int written = vsnprintf(error_buff, sizeof(error_buff), message, args);
  va_end(args);

  size_t len = 0;
  if (written > 0) {
    len = (size_t)written;
    if (len >= MAX_ERROR_LEN)
      len = MAX_ERROR_LEN - 1;
  }

  error_status = status;
  error_content.str = error_buff;
  error_content.len = len;
}
