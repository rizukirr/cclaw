#include "openai.h"
#include "cclaw/cclaw.h"
#include "cclaw/error.h"
#include "cclaw/provider.h"
#include "cclaw/response.h"
#include "cclaw/str.h"
#include "cclaw/tool.h"
#include "cclaw/types.h"
#include "core/internal.h"
#include "lib/libjson.h"
#include "net/http.h"
#include <stdlib.h>
#include <string.h>

#define OPENAI_URL "https://api.openai.com/v1/responses"
#define PROMPT_MAX_LEN 10000
#define MODEL_MAX 256
#define KEY_MAX 256
#define OPENAI_STREAM_LINE_MAX 4096
#define OPENAI_STREAM_DATA_MAX (16 * 1024)
#define OPENAI_STREAM_EVENT_MAX 128
#define OPENAI_STREAM_RESPONSE_ID_MAX 128
#define OPENAI_STREAM_CALL_ID_MAX 128
#define OPENAI_STREAM_ARGS_MAX 1024
#define OPENAI_STREAM_FUNCTION_NAME_MAX 128

typedef enum {
  TOOL_CALL_IDLE,
  TOOL_CALL_ACTIVE,
  TOOL_CALL_DONE,
  TOOL_CALL_READY
} StreamToolState;

typedef struct {
  char response_id[OPENAI_STREAM_RESPONSE_ID_MAX];
  char call_id[OPENAI_STREAM_CALL_ID_MAX];
  char function_name[OPENAI_STREAM_FUNCTION_NAME_MAX];
  char args[OPENAI_STREAM_ARGS_MAX];
  char tool_result[PROMPT_MAX_LEN];
  StreamToolState st;
} OpenAIStreamToolCtx;

typedef struct {
  char line_buf[OPENAI_STREAM_LINE_MAX];
  char event_buf[OPENAI_STREAM_EVENT_MAX];
  char data_buf[OPENAI_STREAM_DATA_MAX];
  size_t line_len, event_len, data_len;
  int failed;
  void *userdata;
  CClawStreamCallback on_chunk;
  OpenAIStreamToolCtx tool_ctx;
  CClaw *ctx;
} OpenAIStreamCtx;

typedef struct {
  char api_key[KEY_MAX];
  char model[MODEL_MAX];
} OpenAIImpl;

static int json_slice_eq_lit(JsonSlice s, const char *lit) {
  size_t n = strlen(lit);
  return s.data && s.len == n && memcmp(s.data, lit, n) == 0;
}

static int openai_dispatch_tool_call(CClaw *ctx, const Output *src, char *dest,
                                     size_t dest_len) {
  if (!ctx || !src || !src->name.data) {
    cclaw_strerror(CCLAW_INVALID, "tool_result: invalid input");
    return CCLAW_INVALID;
  }

  for (size_t i = 0; i < ctx->tool_count; i++) {
    CClawTool *tool = &ctx->tools[i];

    if (src->name.len == strlen(tool->name) &&
        memcmp(src->name.data, tool->name, src->name.len) == 0) {
      char result[CCLAW_TOOL_RESULT_MAX];

      int rc = tool->fn(src->arguments.data, src->arguments.len, result,
                        sizeof(result), tool->user);

      if (rc != 0) {
        cclaw_strerror(CCLAW_ERR_PARSE, "tool_result: name=%s error=%d",
                       tool->name, rc);
        return rc;
      }

      snprintf(dest, dest_len, "%s", result);
      return 0;
    }
  }

  cclaw_strerror(CCLAW_INVALID, "tool_result: unknow tool name=%.*s",
                 (int)src->name.len, src->name.data);
  return CCLAW_INVALID;
}

static int openai_stream_tool_result(OpenAIStreamCtx *st, Output *out) {
  if (!st || !out)
    return CCLAW_INVALID;

  out->name.data = st->tool_ctx.function_name;
  out->name.len = strlen(st->tool_ctx.function_name);
  out->call_id.data = st->tool_ctx.call_id;
  out->call_id.len = strlen(st->tool_ctx.call_id);
  out->type.data = "function_call_output";
  out->type.len = strlen("function_call_output");
  out->arguments.data = st->tool_ctx.args;
  out->arguments.len = strlen(st->tool_ctx.args);
  return 0;
}

static int openai_stream_dispatch(OpenAIStreamCtx *st) {
  if (st->data_len == 0)
    return 0;

  st->data_buf[st->data_len] = '\0';
  JsonSlice root = json_from_cstr(st->data_buf);

  JsonSlice type = {0}, call_id = {0}, function_name = {0};

  int gettype = json_get(root, "type", &type);

  if (gettype && (json_slice_eq_lit(type, "response.created") ||
                  json_slice_eq_lit(type, "response.in_progress"))) {
    JsonSlice response = {0};
    JsonSlice response_id = {0};
    if (json_get(root, "response", &response) &&
        json_get(response, "id", &response_id) && response_id.data &&
        response_id.len > 0) {
      snprintf(st->tool_ctx.response_id, sizeof(st->tool_ctx.response_id),
               "%.*s", (int)response_id.len, response_id.data);
    }
  }

  if (gettype && json_slice_eq_lit(type, "response.output_text.delta")) {
    JsonSlice delta = {0};
    if (json_get(root, "delta", &delta) && delta.data && delta.len > 0) {
      if (st->on_chunk(delta.data, delta.len, st->userdata) != 0) {
        st->failed = 1;
        return -1;
      }
    }
  } else if (gettype && json_slice_eq_lit(type, "response.error")) {
    st->failed = 1;
    cclaw_strerror(CCLAW_ERR_NETWORK, "openai stream returned response.error");
    return -1;
  } else if (gettype && json_slice_eq_lit(type, "response.output_item.added")) {

    JsonSlice item = {0};
    if (json_get(root, "item", &item)) {
      JsonSlice inner_type = {0};
      if (json_get(item, "type", &inner_type) &&
          json_slice_eq_lit(inner_type, "function_call")) {

        if (json_get(item, "call_id", &call_id)) {
          snprintf(st->tool_ctx.call_id, sizeof(st->tool_ctx.call_id), "%.*s",
                   (int)call_id.len, call_id.data);
        }

        if (json_get(item, "name", &function_name)) {
          snprintf(st->tool_ctx.function_name,
                   sizeof(st->tool_ctx.function_name), "%.*s",
                   (int)function_name.len, function_name.data);
        }

        st->tool_ctx.st = TOOL_CALL_ACTIVE;
      }
    }
  }

  if (st->tool_ctx.st == TOOL_CALL_ACTIVE) {
    if (gettype &&
        json_slice_eq_lit(type, "response.function_call_arguments.done")) {
      JsonSlice arguments = {0};
      if (json_get(root, "arguments", &arguments)) {
        snprintf(st->tool_ctx.args, sizeof(st->tool_ctx.args), "%.*s",
                 (int)arguments.len, arguments.data);
        st->tool_ctx.st = TOOL_CALL_READY;
      }
    }
  }

  if (st->tool_ctx.st == TOOL_CALL_READY) {
    Output tmp = {0};
    if (openai_stream_tool_result(st, &tmp) != 0) {
      return -1;
    }

    int rc = openai_dispatch_tool_call(st->ctx, &tmp, st->tool_ctx.tool_result,
                                       sizeof(st->tool_ctx.tool_result));
    if (rc != 0)
      return rc;

    st->tool_ctx.st = TOOL_CALL_DONE;
  }

  st->event_len = 0;
  st->data_len = 0;
  st->event_buf[0] = '\0';
  st->data_buf[0] = '\0';
  return 0;
}

static int openai_stream_consume_line(OpenAIStreamCtx *st) {
  st->line_buf[st->line_len] = '\0';

  if (st->line_len == 0)
    return openai_stream_dispatch(st);

  if (strncmp(st->line_buf, "event:", 6) == 0) {
    const char *p = st->line_buf + 6;
    while (*p == ' ')
      p++;

    size_t n = strlen(p);
    if (n >= sizeof(st->event_buf))
      n = sizeof(st->event_buf) - 1;

    memcpy(st->event_buf, p, n);
    st->event_buf[n] = '\0';
    st->event_len = n;
    return 0;
  }

  if (strncmp(st->line_buf, "data:", 5) == 0) {
    const char *p = st->line_buf + 5;
    while (*p == ' ')
      p++;

    size_t n = strlen(p);
    if (st->data_len + n + 1 >= sizeof(st->data_buf)) {
      st->failed = 1;
      cclaw_strerror(CCLAW_ERR_OOM, "failed to allocate data buffer");
      return CCLAW_ERR_OOM;
    }

    if (st->data_len > 0)
      st->data_buf[st->data_len++] = '\n';

    memcpy(st->data_buf + st->data_len, p, n);
    st->data_len += n;
    st->data_buf[st->data_len] = '\0';
  }

  return 0;
}

static char *openai_build_body(CClaw *ctx, OpenAIImpl *s, const char *prompt,
                               char *buf, size_t cap, int stream) {
  JsonWriter w;
  json_writer_init(&w, buf, cap);

  // clang-format off
  json_write_object_begin(&w);
    json_write_str_kv(&w, "model", s->model);
    if(stream)
      json_write_bool_kv(&w, "stream", true);

    if(ctx && ctx->tool_count > 0){
      json_write_array_begin_k(&w, "tools");
      for(size_t i = 0; i < ctx->tool_count; i++){
        const CClawTool *t = &ctx->tools[i];
        json_write_object_begin(&w);
          json_write_str_kv(&w, "type", "function");
          json_write_str_kv(&w, "name", t->name);
          json_write_str_kv(&w, "description", t->description ? t->description: "");
          json_write_raw_kv(&w, "parameters", t->params_schema ? t->params_schema : "{}",
              strlen(t->params_schema ? t->params_schema : "{}"));
        json_write_object_end(&w);
      }
      json_write_array_end(&w);
    }

    json_write_array_begin_k(&w, "input");
      // TODO: must be moved system prompt into config
      json_write_object_begin(&w);
        json_write_str_kv(&w, "role", "developer");
        json_write_str_kv(&w, "content", "You MUST only answer with less than 100 characters.");
      json_write_object_end(&w);
      json_write_object_begin(&w);
        json_write_str_kv(&w, "role", "user");
        json_write_str_kv(&w, "content", prompt);
      json_write_object_end(&w);
    json_write_array_end(&w);
  json_write_object_end(&w);
  // clang-format on

  if (!json_writer_ok(&w))
    return NULL;
  return (char *)json_writer_output(&w);
}

static char *openai_build_tool_result_body(OpenAIImpl *s, const char *call_id,
                                           const char *tool_result, char *buf,
                                           size_t cap, char *chat_id_buf,
                                           size_t char_id_len) {
  JsonWriter w;
  json_writer_init(&w, buf, cap);

  // clang-format off
  json_write_object_begin(&w);
    json_write_str_kv(&w, "model", s->model);
    json_write_str_kv(&w, "previous_response_id", chat_id_buf);
    json_write_array_begin_k(&w, "input");
      json_write_object_begin(&w);
        json_write_str_kv(&w, "type", "function_call_output");
        json_write_str_kv(&w, "call_id", call_id);
        json_write_str_kv(&w, "output", tool_result);
      json_write_object_end(&w);
    json_write_array_end(&w);
  json_write_object_end(&w);
  // clang-format on

  return json_writer_ok(&w) ? (char *)json_writer_output(&w) : NULL;
}

static int openai_http_stream_cb(const char *chunk, size_t len,
                                 void *userdata) {
  OpenAIStreamCtx *st = (OpenAIStreamCtx *)userdata;
  for (size_t i = 0; i < len; i++) {
    char ch = chunk[i];
    if (ch == '\r')
      continue;
    if (ch == '\n') {
      if (openai_stream_consume_line(st) != 0)
        return -1;
      st->line_len = 0;
      continue;
    }
    if (st->line_len + 1 >= sizeof(st->line_buf)) {
      st->failed = 1;
      cclaw_strerror(CCLAW_ERR_OOM, "openai stream line too large");
      return -1;
    }
    st->line_buf[st->line_len++] = ch;
  }
  return 0;
}

static char *openai_build_stream_tool_result_body(
    OpenAIImpl *s, const OpenAIStreamToolCtx tool_ctx, char *buf, size_t cap) {
  JsonWriter w;
  json_writer_init(&w, buf, cap);

  // clang-format off
  json_write_object_begin(&w);
    json_write_str_kv(&w, "model", s->model);
    json_write_bool_kv(&w, "stream", true);
    json_write_str_kv(&w, "previous_response_id", tool_ctx.response_id);
    json_write_array_begin_k(&w, "input");
      json_write_object_begin(&w);
        json_write_str_kv(&w, "type", "function_call_output");
        json_write_str_kv(&w, "call_id", tool_ctx.call_id);
        json_write_str_kv(&w, "output", tool_ctx.tool_result);
      json_write_object_end(&w);
    json_write_array_end(&w);
  json_write_object_end(&w);
  // clang-format on

  return json_writer_ok(&w) ? (char *)json_writer_output(&w) : NULL;
}

static int openai_http_post_stream(OpenAIImpl *s, OpenAIStreamCtx *st,
                                   char *json) {
  if (!s || !json)
    return CCLAW_INVALID;

  int res = cclaw_http_post_stream(OPENAI_URL, s->api_key, json,
                                   openai_http_stream_cb, st);
  if (res != 0)
    return res;

  if (st->line_len > 0) {
    if (openai_stream_consume_line(st) != 0)
      return CCLAW_ERR_NETWORK;
    st->line_len = 0;
  }

  if (st->data_len > 0) {
    if (openai_stream_dispatch(st) != 0)
      return CCLAW_ERR_NETWORK;
  }

  if (st->failed)
    return CCLAW_ERR_NETWORK;

  return 0;
}

static int openai_chat_stream(CClaw *ctx, CClawProvider *self,
                              const char *prompt, CClawStreamCallback on_chunk,
                              void *userdata) {
  OpenAIImpl *s = (OpenAIImpl *)self->impl;
  int rc;
  char buf[PROMPT_MAX_LEN];
  char tool_buf[PROMPT_MAX_LEN];

  char *body = openai_build_body(ctx, s, prompt, buf, sizeof(buf), 1);
  if (!body) {
    cclaw_strerror(CCLAW_ERR_PARSE, "failed to build stream request body");
    return CCLAW_ERR_PARSE;
  }

  OpenAIStreamCtx st = {
      .on_chunk = on_chunk,
      .userdata = userdata,
      .ctx = ctx,
      .line_len = 0,
      .event_len = 0,
      .data_len = 0,
      .failed = 0,
  };

  rc = openai_http_post_stream(s, &st, body);
  if (rc != 0)
    return rc;

  // reset per-stream text/event buffers before seconds stream
  st.line_len = 0;
  st.event_len = 0;
  st.data_len = 0;
  st.line_buf[0] = '\0';
  st.event_buf[0] = '\0';
  st.data_buf[0] = '\0';
  st.failed = 0;

  // Initiate tool call
  if (st.tool_ctx.st == TOOL_CALL_DONE) {

    char *tool_result_json = openai_build_stream_tool_result_body(
        s, st.tool_ctx, tool_buf, sizeof(tool_buf));

    if (!tool_result_json)
      return CCLAW_ERR_PARSE;

    rc = openai_http_post_stream(s, &st, tool_result_json);
    if (rc != 0)
      return rc;

    st.tool_ctx.st = TOOL_CALL_IDLE;
  }

  return 0;
}

static CClawString openai_chat(CClaw *ctx, CClawProvider *self,
                               const char *prompt) {
  OpenAIImpl *s = (OpenAIImpl *)self->impl;
  char buf[PROMPT_MAX_LEN];

  char *body = openai_build_body(ctx, s, prompt, buf, sizeof(buf), false);
  if (!body) {
    cclaw_strerror(CCLAW_ERR_PARSE, "failed to build request body");
    return (CClawString){NULL, 0};
  }

  CClawString res = cclaw_http_post(ctx, OPENAI_URL, s->api_key, body);
  if (res.len == 0) {
    cclaw_strerror(CCLAW_ERR_NETWORK, "failed to reach openai");
    return res;
  }

  char tool_result[CCLAW_TOOL_RESULT_MAX];
  char tool_buf[PROMPT_MAX_LEN];
  char call_id_buf[128];
  char chat_id_buf[128];

  if (!cclaw_is_response_error(ctx, res)) {
    Response parsed = cclaw_response_from_json(res);
    snprintf(chat_id_buf, sizeof(chat_id_buf), "%.*s", (int)parsed.id.len,
             parsed.id.data);

    for (size_t i = 0; i < MAX_OUTPUT && parsed.output[i].type.data; i++) {
      Output *out = &parsed.output[i];
      if (json_slice_eq_lit(out->type, "function_call")) {
        int rc = openai_dispatch_tool_call(ctx, out, tool_result,
                                           sizeof(tool_result));
        if (rc == 0) {
          snprintf(call_id_buf, sizeof(call_id_buf), "%.*s",
                   (int)out->call_id.len, out->call_id.data);

          char *tool_result_body = openai_build_tool_result_body(
              s, call_id_buf, tool_result, tool_buf, sizeof(tool_buf),
              chat_id_buf, strlen(chat_id_buf));

          if (!tool_result_body)
            return (CClawString){NULL, 0};

          CClawString final_res =
              cclaw_http_post(ctx, OPENAI_URL, s->api_key, tool_result_body);
          if (final_res.len > 0)
            return final_res;
        }
      }
    }
  }

  return res;
}

static void openai_free(CClawProvider *self) {
  if (!self)
    return;
  if (self->impl) {
    memset(self->impl, 0, sizeof(OpenAIImpl));
    free(self->impl);
  }
  free(self);
}

CClawProvider *cclaw_provider_openai(const CClawProviderConfig *cfg) {
  if (!cfg || !cfg->api_key || !cfg->model)
    return NULL;

  size_t kn = strlen(cfg->api_key);
  size_t mn = strlen(cfg->model);
  if (kn >= KEY_MAX || mn >= MODEL_MAX)
    return NULL;

  CClawProvider *p = calloc(1, sizeof(CClawProvider));
  if (!p)
    return NULL;

  OpenAIImpl *s = calloc(1, sizeof(OpenAIImpl));
  if (!s) {
    free(p);
    return NULL;
  }

  memcpy(s->api_key, cfg->api_key, kn + 1);
  memcpy(s->model, cfg->model, mn + 1);

  p->name = "openai";
  p->chat = openai_chat;
  p->free = openai_free;
  p->impl = s;
  p->chat_stream = openai_chat_stream;
  return p;
}
