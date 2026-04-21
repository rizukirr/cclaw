#ifndef CCLAW_ERROR_H
#define CCLAW_ERROR_H

#include "types.h"

#if defined(__GNUC__) || defined(__clang__)
#define CCLAW_PRINTF_FMT(fmt_idx, arg_idx)                                   \
  __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define CCLAW_PRINTF_FMT(fmt_idx, arg_idx)
#endif

#define CCLAW_ERR(status, message) cclaw_strerror(status, message)

void cclaw_strerror(CClawStatus status, const char *message, ...)
    CCLAW_PRINTF_FMT(2, 3);

#endif
