#include "http.h"
#include "cclaw/error.h"
#include "core/internal.h"
#include <curl/curl.h>
#include <curl/easy.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_AUTH_PREFIX "Authorization: Bearer "
#define HEADER_CONTENT_TYPE_JSON "Content-Type: application/json"
#define HTTP_RESPONSE_STACK_CAP (128 * 1024)

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  int overflow;
} HttpWriteCtx;

typedef struct {
  CClawHttpStreamCallback on_chunk;
  void *userdata;
  int failed;
} HttpStreamCtx;

static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb,
                              void *userdata) {
  HttpStreamCtx *ctx = (HttpStreamCtx *)userdata;

  if (size != 0 && nmemb > SIZE_MAX / size) {
    ctx->failed = 1;
    return 0;
  }

  size_t chunk = size * nmemb;
  if (chunk == 0)
    return 0;

  if (ctx->on_chunk((const char *)ptr, chunk, ctx->userdata) != 0) {
    ctx->failed = 1;
    return 0;
  }

  return chunk;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  HttpWriteCtx *ctx = (HttpWriteCtx *)userdata;

  if (size != 0 && nmemb > SIZE_MAX / size) {
    ctx->overflow = 1;
    return 0;
  }

  size_t chunk = size * nmemb;
  if (ctx->len > ctx->cap || chunk > ctx->cap - ctx->len) {
    ctx->overflow = 1;
    return 0;
  }

  memcpy(ctx->buf + ctx->len, ptr, chunk);
  ctx->len += chunk;
  return chunk;
}

CClawString cclaw_http_post(CClaw *ctx, const char *url, const char *api_key,
                            const char *body) {
  CClawString result = {NULL, 0};
  char response_stack[HTTP_RESPONSE_STACK_CAP];
  HttpWriteCtx write_ctx = {
      .buf = response_stack,
      .len = 0,
      .cap = sizeof(response_stack) - 1,
      .overflow = 0,
  };

  if (!api_key) {
    cclaw_strerror(CCLAW_INVALID, "missing API key");
    return result;
  }

  CURL *curl = curl_easy_init();
  if (!curl)
    return result;

  /* Build auth header */
  size_t prefix_len = strlen(HEADER_AUTH_PREFIX);
  size_t key_len = strlen(api_key);
  char auth_header[prefix_len + key_len + 1];
  memcpy(auth_header, HEADER_AUTH_PREFIX, prefix_len);
  memcpy(auth_header + prefix_len, api_key, key_len);
  auth_header[prefix_len + key_len] = '\0';

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, HEADER_CONTENT_TYPE_JSON);
  headers = curl_slist_append(headers, auth_header);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cclaw/0.0.1");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || write_ctx.overflow) {
    if (write_ctx.overflow)
      cclaw_strerror(CCLAW_ERR_OOM, "response exceeds %zu bytes",
                     write_ctx.cap);

    cclaw_strerror(CCLAW_ERR_NETWORK, "cclaw: curl failed: %s",
                   curl_easy_strerror(res));
    return result;
  }

  result.str =
      arena_alloc(ctx->arena, write_ctx.len + 1, ARENA_ALIGNOF(result.str));
  if (!result.str)
    return (CClawString){NULL, 0};

  memcpy(result.str, write_ctx.buf, write_ctx.len);
  result.str[write_ctx.len] = '\0';
  result.len = write_ctx.len;

  return result;
}

int cclaw_http_post_stream(const char *url, const char *api_key,
                           const char *body, CClawHttpStreamCallback on_chunk,
                           void *userdata) {
  if (!api_key || !on_chunk) {
    cclaw_strerror(CCLAW_INVALID, "cclaw: missing API key or stream callback");
    return CCLAW_INVALID;
  }

  CURL *curl = curl_easy_init();
  if (!curl)
    return CCLAW_ERR_NETWORK;

  size_t prefix_len = strlen(HEADER_AUTH_PREFIX);
  size_t key_len = strlen(api_key);
  char auth_header[prefix_len + key_len + 1];
  memcpy(auth_header, HEADER_AUTH_PREFIX, prefix_len);
  memcpy(auth_header + prefix_len, api_key, key_len);
  auth_header[prefix_len + key_len] = '\0';

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, HEADER_CONTENT_TYPE_JSON);
  headers = curl_slist_append(headers, auth_header);

  HttpStreamCtx ctx = {
      .on_chunk = on_chunk,
      .userdata = userdata,
      .failed = 0,
  };

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cclaw/0.0.1");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || ctx.failed) {
    if (res != CURLE_OK)
      cclaw_strerror(CCLAW_ERR_NETWORK, "cclaw: curl failed: %s",
                     curl_easy_strerror(res));
    return CCLAW_ERR_NETWORK;
  }

  return 0;
}
