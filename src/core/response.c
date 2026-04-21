#define LIBJSON_IMPLEMENTATION
#include "cclaw/response.h"

Response cclaw_response_from_json(const CClawString json) {
  char *str = json.str;
  str[json.len] = '\0';

  JsonSlice root = json_from_cstr(str);
  JsonSlice billing, output, outputItem, reasoning, usage, content, contentItem;
  JsonArrayIter outputs, contents;
  Response res = {0};

  json_get(root, "id", &res.id);
  json_get(root, "object", &res.object);
  json_get(root, "created_at", &res.created_at);
  json_get(root, "status", &res.status);
  json_get(root, "background", &res.background);
  json_get(root, "completed_at", &res.completed_at);
  json_get(root, "error", &res.error);
  json_get(root, "frequency_penalty", &res.frequency_penalty);
  json_get(root, "incomplete_details", &res.incomplete_details);
  json_get(root, "instructions", &res.instructions);
  json_get(root, "max_output_tokens", &res.max_output_tokens);
  json_get(root, "max_tool_calls", &res.max_tool_calls);
  json_get(root, "model", &res.model);
  json_get(root, "parallel_tool_calls", &res.parallel_tool_calls);
  json_get(root, "presence_penalty", &res.presence_penalty);
  json_get(root, "previous_response_id", &res.previous_response_id);
  json_get(root, "prompt_cache_key", &res.prompt_cache_key);
  json_get(root, "prompt_cache_retention", &res.prompt_cache_retention);
  json_get(root, "safety_identifier", &res.safety_identifier);
  json_get(root, "service_tier", &res.service_tier);
  json_get(root, "store", &res.store);
  json_get(root, "temperature", &res.temperature);
  json_get(root, "text", &res.text);
  json_get(root, "tool_choice", &res.tool_choice);
  json_get(root, "tools", &res.tools);
  json_get(root, "top_logprobs", &res.top_logprobs);
  json_get(root, "top_p", &res.top_p);
  json_get(root, "truncation", &res.truncation);
  json_get(root, "user", &res.user);
  json_get(root, "metadata", &res.metadata);

  json_get(root, "billing", &billing);
  json_get(billing, "payer", &res.billing.payer);

  if (json_get(root, "output", &output)) {
    json_array_iter_init(output, &outputs);

    size_t i = 0;
    while (json_array_iter_next(&outputs, &outputItem)) {
      json_get(outputItem, "id", &res.output[i].id);
      json_get(outputItem, "type", &res.output[i].type);
      json_get(outputItem, "summary", &res.output[i].summary);
      json_get(outputItem, "role", &res.output[i].role);
      if (json_get(outputItem, "content", &content)) {
        json_array_iter_init(content, &contents);
        size_t j = 0;
        while (json_array_iter_next(&contents, &contentItem)) {
          json_get(contentItem, "type", &res.output[i].content[j].type);
          json_get(contentItem, "annotations",
                   &res.output[i].content[j].annotations);
          json_get(contentItem, "logprobs", &res.output[i].content[j].logprobs);
          json_get(contentItem, "text", &res.output[i].content[j].text);
          j++;
        }
      }
      i++;
    }
  }

  json_get(root, "reasoning", &reasoning);
  json_get(reasoning, "effort", &res.reasoning.effort);
  json_get(reasoning, "summary", &res.reasoning.summary);

  json_get(root, "usage", &usage);
  json_get(usage, "input_tokens", &res.usage.innput_tokens);
  json_get(usage, "output_tokens", &res.usage.output_tokens);
  json_get(usage, "total_tokens", &res.usage.total_tokens);

  return res;
}
