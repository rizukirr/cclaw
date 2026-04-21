#define ARENA_IMPLEMENTATION
#define LIBENV_IMPLEMENTATION
#include "cclaw/cclaw.h"
#include "cclaw/error.h"
#include "internal.h"
#include "lib/libenv.h"
#include <stdlib.h>

CClaw *cclaw_init(void) {
  CClaw *ctx = calloc(1, sizeof(CClaw));
  if (!ctx) {
    cclaw_strerror(CCLAW_ERR_OOM, "failed to allocate context");
    return NULL;
  }

  ctx->arena = arena_create(DEFAULT_ARENA_SIZE);
  if (!ctx->arena) {
    cclaw_strerror(CCLAW_ERR_OOM, "failed to allocate arena");
    free(ctx);
    return NULL;
  }

  if (libenv_load(".env") != 0) {
    cclaw_strerror(CCLAW_INVALID, "failed to load .env");
    arena_free(ctx->arena);
    free(ctx);
    return NULL;
  }

  ctx->provider = cclaw_provider_from_env();
  if (!ctx->provider) {
    cclaw_strerror(CCLAW_INVALID, "failed to init provider");
    arena_free(ctx->arena);
    free(ctx);
    return NULL;
  }

  return ctx;
}

CClawString cclaw_chat(CClaw *ctx, const char *prompt) {
  if (!ctx || !ctx->provider) {
    cclaw_strerror(CCLAW_INVALID, "ctx or provider is null");
    return (CClawString){NULL, 0};
  }
  return ctx->provider->chat(ctx, ctx->provider, prompt);
}

int cclaw_chat_stream(CClaw *ctx, const char *prompt,
                      CClawStreamCallback on_chunk, void *userdata) {
  if (!ctx || !ctx->provider || !on_chunk) {
    cclaw_strerror(CCLAW_INVALID, "ctx/provider/callback is null");
    return CCLAW_INVALID;
  }

  if (ctx->provider->chat_stream)
    return ctx->provider->chat_stream(ctx, ctx->provider, prompt, on_chunk,
                                      userdata);

  CClawString res = ctx->provider->chat(ctx, ctx->provider, prompt);
  if (!res.str || res.len == 0)
    return -1;

  return on_chunk(res.str, res.len, userdata);
}

void cclaw_destroy(CClaw *ctx) {
  if (!ctx)
    return;
  if (ctx->provider && ctx->provider->free)
    ctx->provider->free(ctx->provider);
  arena_free(ctx->arena);
  free(ctx);
}
