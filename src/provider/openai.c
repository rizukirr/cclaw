#include "openai.h"
#include "cclaw/cclaw.h"
#include "cclaw/error.h"
#include "cclaw/provider.h"
#include "cclaw/str.h"
#include "cclaw/types.h"
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

typedef struct {
  char line_buf[OPENAI_STREAM_LINE_MAX];
  char event_buf[OPENAI_STREAM_EVENT_MAX];
  char data_buf[OPENAI_STREAM_DATA_MAX];
  size_t line_len, event_len, data_len;
  int failed;
  CClawStreamCallback on_chunk;
  void *userdata;
} OpenAIStreamCtx;

typedef struct {
  char api_key[KEY_MAX];
  char model[MODEL_MAX];
} OpenAIImpl;

static int json_slice_eq_lit(JsonSlice s, const char *lit) {
  size_t n = strlen(lit);
  return s.data && s.len == n && memcmp(s.data, lit, n) == 0;
}

static int openai_stream_dispatch(OpenAIStreamCtx *st) {
  if (st->data_len == 0)
    return 0;

  st->data_buf[st->data_len] = '\0';
  JsonSlice root = json_from_cstr(st->data_buf);

  JsonSlice type = {0};
  if (json_get(root, "type", &type) &&
      json_slice_eq_lit(type, "response.output_text.delta")) {
    JsonSlice delta = {0};
    if (json_get(root, "delta", &delta) && delta.data && delta.len > 0) {
      if (st->on_chunk(delta.data, delta.len, st->userdata) != 0) {
        st->failed = 1;
        return -1;
      }
    }
  } else if (json_get(root, "type", &type) &&
             json_slice_eq_lit(type, "response.error")) {
    st->failed = 1;
    cclaw_strerror(CCLAW_ERR_NETWORK, "openai stream returned response.error");
    return -1;
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

static char *openai_build_body(OpenAIImpl *s, const char *prompt, char *buf,
                               size_t cap, int stream) {
  JsonWriter w;
  json_writer_init(&w, buf, cap);

  // clang-format off
  json_write_object_begin(&w);
    json_write_str_kv(&w, "model", s->model);
    if(stream)
      json_write_bool_kv(&w, "stream", true);
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

static int openai_parse_nonstream_error(CClawString res) {
  JsonSlice root = json_from_parts(res.str, res.len);
  JsonSlice err = {0};
  if (!json_get(root, "error", &err) || !err.data || err.len == 0 ||
      err.data[0] != '{') {
    return 0;
  }

  JsonSlice message = {0};
  if (!json_get(err, "message", &message) || !message.data || message.len == 0)
    return 0;

  JsonSlice code = {0};
  if (json_get(err, "code", &code) && code.data && code.len > 0) {
    cclaw_strerror(CCLAW_INVALID, "openai error (%.*s): %.*s", (int)code.len,
                   code.data, (int)message.len, message.data);
  } else {
    cclaw_strerror(CCLAW_INVALID, "openai error: %.*s", (int)message.len,
                   message.data);
  }

  return 1;
}

static int openai_chat_stream(CClaw *ctx, CClawProvider *self,
                              const char *prompt, CClawStreamCallback on_chunk,
                              void *userdata) {
  (void)ctx;
  OpenAIImpl *s = (OpenAIImpl *)self->impl;
  char buf[PROMPT_MAX_LEN];

  char *body = openai_build_body(s, prompt, buf, sizeof(buf), 1);
  if (!body) {
    cclaw_strerror(CCLAW_ERR_PARSE, "failed to build stream request body");
    return CCLAW_ERR_PARSE;
  }

  OpenAIStreamCtx st = {
      .on_chunk = on_chunk,
      .userdata = userdata,
      .line_len = 0,
      .event_len = 0,
      .data_len = 0,
      .failed = 0,
  };

  int rc = cclaw_http_post_stream(OPENAI_URL, s->api_key, body,
                                  openai_http_stream_cb, &st);
  if (rc != 0)
    return rc;

  if (st.line_len > 0) {
    if (openai_stream_consume_line(&st) != 0)
      return CCLAW_ERR_NETWORK;
    st.line_len = 0;
  }

  if (st.data_len > 0) {
    if (openai_stream_dispatch(&st) != 0)
      return CCLAW_ERR_NETWORK;
  }

  if (st.failed)
    return CCLAW_ERR_NETWORK;

  return 0;
}

static CClawString openai_chat(CClaw *ctx, CClawProvider *self,
                               const char *prompt) {
  OpenAIImpl *s = (OpenAIImpl *)self->impl;
  char buf[PROMPT_MAX_LEN];

  char *body = openai_build_body(s, prompt, buf, sizeof(buf), false);
  if (!body) {
    cclaw_strerror(CCLAW_ERR_PARSE, "failed to build request body");
    return (CClawString){NULL, 0};
  }

  CClawString res = cclaw_http_post(ctx, OPENAI_URL, s->api_key, body);
  if (res.len == 0) {
    cclaw_strerror(CCLAW_ERR_NETWORK, "failed to reach openai");
    return res;
  }

  if (openai_parse_nonstream_error(res))
    return (CClawString){NULL, 0};

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
