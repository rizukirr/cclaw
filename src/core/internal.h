#ifndef CCLAW_INTERNAL_H
#define CCLAW_INTERNAL_H

#include "cclaw/provider.h"
#include "cclaw/tool.h"
#include "lib/arena.h"

#define CCLAW_MAX_TOOLS 16

struct CClaw {
  Arena *arena;
  CClawProvider *provider;
  CClawTool tools[CCLAW_MAX_TOOLS];
  size_t tool_count;
};

#endif // CCLAW_INTERNAL_H
