#ifndef CCLAW_NET_HTTP_H
#define CCLAW_NET_HTTP_H

#include "cclaw/cclaw.h"
#include "cclaw/str.h"

typedef int (*CClawHttpStreamCallback)(const char *chunk, size_t len,
                                       void *userdata);

CClawString cclaw_http_post(CClaw *ctx, const char *url, const char *api_key,
                            const char *body);

int cclaw_http_post_stream(const char *url, const char *api_key,
                           const char *body, CClawHttpStreamCallback on_chunk,
                           void *userdata);

#endif
