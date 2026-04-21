#ifndef CCLAW_INTERNAL_H
#define CCLAW_INTERNAL_H

#include "cclaw/provider.h"
#include "lib/arena.h"

struct CClaw {
  Arena *arena;
  CClawProvider *provider;
};

#endif // CCLAW_INTERNAL_H
