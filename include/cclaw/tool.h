#ifndef CCLAW_TOOL_H
#define CCLAW_TOOL_H

#include <stddef.h>

#define CCLAW_TOOL_RESULT_MAX 1024

typedef int (*CClawToolFn)(const char *args_json, size_t args_len, char *out,
                           size_t out_cap, void *user);

typedef struct {
  const char *name;
  const char *description;
  const char *params_schema;
  CClawToolFn fn;
  void *user;
} CClawTool;

#endif // CCLAW_TOOL_H
