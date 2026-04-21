#ifndef CCLAW_RESPONSE_H
#define CCLAW_RESPONSE_H

#include "cclaw/str.h"
#include "lib/libjson.h"

#define MAX_CONTENT 20
#define MAX_OUTPUT 20

typedef struct {
  JsonSlice payer;
} Billing;

typedef struct {
  JsonSlice type;
  JsonSlice annotations;
  JsonSlice logprobs;
  JsonSlice text;
} Content;

typedef struct {
  JsonSlice id;
  JsonSlice type;
  JsonSlice summary;
  JsonSlice role;
  Content content[MAX_CONTENT];
} Output;

typedef struct {
  JsonSlice effort;
  JsonSlice summary;
} Reasoning;

typedef struct {
  JsonSlice innput_tokens;
  JsonSlice output_tokens;
  JsonSlice total_tokens;
} Usage;

typedef struct {
  JsonSlice id;
  JsonSlice object;
  JsonSlice created_at;
  JsonSlice status;
  JsonSlice background;
  JsonSlice completed_at;
  JsonSlice error;
  JsonSlice frequency_penalty;
  JsonSlice incomplete_details;
  JsonSlice instructions;
  JsonSlice max_output_tokens;
  JsonSlice max_tool_calls;
  JsonSlice model;
  JsonSlice parallel_tool_calls;
  JsonSlice presence_penalty;
  JsonSlice previous_response_id;
  JsonSlice prompt_cache_key;
  JsonSlice prompt_cache_retention;
  JsonSlice safety_identifier;
  JsonSlice service_tier;
  JsonSlice store;
  JsonSlice temperature;
  JsonSlice text;
  JsonSlice tool_choice;
  JsonSlice tools;
  JsonSlice top_logprobs;
  JsonSlice top_p;
  JsonSlice truncation;
  JsonSlice user;
  JsonSlice metadata;
  Billing billing;
  Output output[MAX_OUTPUT];
  Reasoning reasoning;
  Usage usage;
} Response;

Response cclaw_response_from_json(const CClawString json);
#endif // CCLAW_RESPONSE_H
