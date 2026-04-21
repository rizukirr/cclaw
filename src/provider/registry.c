#include "cclaw/provider.h"
#include "lib/libenv.h"
#include "openai.h"
#include <string.h>

CClawProvider *cclaw_provider_from_name(const char *name,
                                        const CClawProviderConfig *cfg) {
  if (!name || !cfg)
    return NULL;
  if (strcmp(name, "openai") == 0)
    return cclaw_provider_openai(cfg);
  return NULL;
}

CClawProvider *cclaw_provider_from_env(void) {
  char *name = libenv_get("CCLAW_PROVIDER");
  if (!name)
    name = "openai";

  if (strcmp(name, "openai") == 0) {
    CClawProviderConfig cfg = {
        .api_key = libenv_get("OPENAI_API_KEY"),
        .model = libenv_get("OPENAI_MODEL"),
        .base_url = NULL,
    };
    if (!cfg.api_key || !cfg.model)
      return NULL;
    return cclaw_provider_openai(&cfg);
  }
  return NULL;
}
