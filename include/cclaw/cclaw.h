#ifndef CCLAW_H
#define CCLAW_H

#include "str.h"

typedef struct CClaw CClaw;

#define DEFAULT_ARENA_SIZE 4096

typedef int (*CClawStreamCallback)(const char *chunk, size_t len,
                                   void *userdata);

CClaw *cclaw_init(void);
CClawString cclaw_chat(CClaw *ctx, const char *prompt);
int cclaw_chat_stream(CClaw *ctx, const char *prompt,
                      CClawStreamCallback on_chunk, void *userdata);
void cclaw_destroy(CClaw *ctx);

#endif
