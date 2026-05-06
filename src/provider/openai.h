#ifndef CCLAW_PROVIDER_OPENAI_H
#define CCLAW_PROVIDER_OPENAI_H

#include "cclaw/provider.h"

/**
 * @brief Construct an OpenAI-backed CClawProvider.
 *
 * Allocates a CClawProvider whose vtable targets the OpenAI Responses API
 * (https://api.openai.com/v1/responses). Both blocking (`chat`) and streaming
 * (`chat_stream`) entry points are wired, and tool-call dispatch is handled
 * automatically against the tools registered on the owning CClaw context.
 *
 * @param cfg  Provider configuration. Must be non-NULL. `cfg->api_key` and
 *             `cfg->model` must be non-NULL C strings shorter than KEY_MAX
 *             (256) and MODEL_MAX (256) bytes respectively. `cfg->base_url`
 *             is currently ignored.
 *
 * @return  Newly allocated provider on success, or NULL if @p cfg is invalid,
 *          a required field is missing, a length limit is exceeded, or
 *          allocation fails. Ownership transfers to the caller, who must
 *          release the provider via its `free` vtable entry (typically
 *          invoked through the owning CClaw context).
 *
 * @note    The returned provider copies the api_key and model into its
 *          internal storage; the caller may free or reuse @p cfg immediately
 *          after this function returns.
 */
CClawProvider *cclaw_provider_openai(const CClawProviderConfig *cfg);

#endif
