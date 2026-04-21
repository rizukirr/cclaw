#ifndef CCLAW_TYPES_H
#define CCLAW_TYPES_H

#include <stddef.h>

typedef enum {
  CCLAW_OK = 0,

  // Network
  CCLAW_ERR_NETWORK, // libcurl failed, DNS, connection refused
  CCLAW_ERR_TIMEOUT, // request timed out

  // Provider
  CCLAW_ERR_AUTH,            // 401/403 — bad API key
  CCLAW_ERR_RATE_LIMIT,      // 429 — too many requests
  CCLAW_ERR_SERVER,          // 500/502/503 — provider down
  CCLAW_ERR_CTX_EXCEEDED,    // token limit exceeded
  CCLAW_ERR_MODEL_NOT_FOUND, // model string not recognized by provider

  // Parse
  CCLAW_ERR_PARSE, // response JSON malformed or unexpected shape

  // Tool
  CCLAW_ERR_TOOL,           // tool fn returned error
  CCLAW_ERR_TOOL_NOT_FOUND, // LLM called a tool name not in registry

  // Agent
  CCLAW_ERR_MAX_ITER, // ReAct loop hit iteration limit

  // System
  CCLAW_ERR_OOM,         // arena/malloc failed
  CCLAW_ERR_UNSUPPORTED, // provider doesn't support requested capability

  CCLAW_INVALID = -1,
} CClawStatus;

#endif
