#ifndef CCLAW_PROVIDER_H
#define CCLAW_PROVIDER_H

#include "cclaw/cclaw.h"
#include "str.h"

typedef struct CClawProvider CClawProvider;

typedef struct {
  const char *api_key;
  const char *model;
  const char *base_url;
} CClawProviderConfig;

struct CClawProvider {
  const char *name;
  CClawString (*chat)(CClaw *ctx, CClawProvider *self, const char *prompt);
  int (*chat_stream)(CClaw *ctx, CClawProvider *self, const char *prompt,
                     CClawStreamCallback on_chunk, void *userdata);
  void (*free)(CClawProvider *self);
  void *impl;
};

CClawProvider *cclaw_provider_from_name(const char *name,
                                        const CClawProviderConfig *cfg);
CClawProvider *cclaw_provider_from_env(void);

#endif
