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

/**
 * @brief Compare a JsonSlice against a NUL-terminated literal for byte
 * equality.
 *
 * Used throughout the OpenAI parser to dispatch on JSON `type` discriminator
 * strings (e.g. "function_call", "response.output_text.delta") without
 * allocating a temporary C string.
 *
 * @param s    JSON slice to compare. A slice with `s.data == NULL` always
 *             compares unequal.
 * @param lit  NUL-terminated literal to compare against. Must be non-NULL.
 *
 * @return  Non-zero if the slice's bytes match @p lit exactly (same length
 *          and same content), zero otherwise.
 */
static int json_slice_eq_lit(JsonSlice s, const char *lit) {
  size_t n = strlen(lit);
  return s.data && s.len == n && memcmp(s.data, lit, n) == 0;
}

/**
 * @brief Resolve a model-emitted tool call to a registered tool and run it.
 *
 * Looks up @p src->name in the CClaw tool registry, invokes the matched
 * tool's callback with the model-supplied JSON arguments, and copies the
 * tool's textual result into @p dest.
 *
 * @param ctx       CClaw context owning the tool registry. Must be non-NULL.
 * @param src       Parsed `function_call` Output describing the tool the
 *                  model wants to invoke. Must be non-NULL with a populated
 *                  `name` slice.
 * @param dest      Caller-provided buffer that receives the tool result as a
 *                  NUL-terminated string. Must be non-NULL.
 * @param dest_len  Capacity of @p dest in bytes, including the NUL.
 *
 * @return  0 on success; CCLAW_INVALID if inputs are malformed or the tool
 *          name is not registered; the tool callback's non-zero return code
 *          if the callback itself fails.
 *
 * @note    Errors are also reported via cclaw_strerror so the caller can
 *          surface a human-readable diagnostic.
 */
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

/**
 * @brief Project the streaming tool-call accumulator into an Output struct.
 *
 * Wraps the function name, call_id, and arguments accumulated across
 * streaming SSE events into an Output suitable for handing to
 * openai_dispatch_tool_call(). The Output's slices borrow from @p st and
 * remain valid only while @p st is live and unmodified.
 *
 * @param st   Streaming context whose `tool_ctx` holds the accumulated call.
 *             Must be non-NULL.
 * @param out  Destination Output to populate. Must be non-NULL.
 *
 * @return  0 on success, CCLAW_INVALID if either argument is NULL.
 */
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

/**
 * @brief Process one fully-buffered SSE `data:` payload from the OpenAI stream.
 *
 * Parses the JSON in `st->data_buf` and reacts to the event `type` field:
 *   - `response.created` / `response.in_progress`: capture the response id
 *     (needed later to chain a tool-result follow-up request).
 *   - `response.output_text.delta`: forward the text chunk to the user
 *     callback `st->on_chunk`.
 *   - `response.error`: mark the stream failed.
 *   - `response.output_item.added` (function_call): begin accumulating a
 *     tool call (capture call_id and function name).
 *   - `response.function_call_arguments.done`: finalize accumulated
 *     arguments and transition the tool state to READY.
 *
 * When the tool state reaches READY, the call is dispatched via
 * openai_dispatch_tool_call() and the state advances to DONE so the outer
 * stream driver can issue the follow-up `function_call_output` request.
 *
 * On exit, the event/data line buffers are reset to receive the next event.
 *
 * @param st  Streaming context. Must be non-NULL with `data_buf`/`data_len`
 *            populated.
 *
 * @return  0 on success or when the user callback signals continue; -1 if
 *          the user callback aborts the stream or a `response.error` is
 *          observed; non-zero error code from openai_dispatch_tool_call() if
 *          tool execution fails.
 */
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
  }

  if (gettype && json_slice_eq_lit(type, "response.error")) {
    st->failed = 1;
    cclaw_strerror(CCLAW_ERR_NETWORK, "openai stream returned response.error");
    return -1;
  }

  if (gettype && json_slice_eq_lit(type, "response.output_item.added")) {
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

/**
 * @brief Consume one complete SSE line from the line buffer.
 *
 * Implements the line-oriented half of the SSE framing protocol:
 *   - An empty line dispatches the accumulated event by calling
 *     openai_stream_dispatch().
 *   - A line beginning with `event:` captures the event name into
 *     `st->event_buf`.
 *   - A line beginning with `data:` appends the payload to `st->data_buf`,
 *     joining multiple `data:` lines with `\n` per the SSE spec.
 *   - All other lines (including comments beginning with `:`) are ignored.
 *
 * The line buffer itself is reset by the caller after this function returns.
 *
 * @param st  Streaming context with `line_buf`/`line_len` holding the line
 *            (NUL terminator written here). Must be non-NULL.
 *
 * @return  0 on success; CCLAW_ERR_OOM if the data buffer would overflow;
 *          forwards any non-zero return from openai_stream_dispatch().
 */
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

/**
 * @brief Serialize an OpenAI Responses API request body into @p buf.
 *
 * Builds the JSON document POSTed to /v1/responses for the initial turn:
 * model, optional `stream:true`, the tool schema array (when tools are
 * registered on @p ctx), the hard-coded developer message, and the user
 * prompt.
 *
 * @param ctx     CClaw context whose registered tools are advertised to the
 *                model. May be NULL or empty (no `tools` array is emitted).
 * @param s       OpenAI provider implementation (for `model`). Must be
 *                non-NULL.
 * @param prompt  User prompt text. Must be non-NULL.
 * @param buf     Caller-provided output buffer. Must be non-NULL.
 * @param cap     Capacity of @p buf in bytes.
 * @param stream  When non-zero, emits `"stream": true` so the server uses
 *                Server-Sent Events.
 *
 * @return  Pointer into @p buf containing the NUL-terminated JSON body on
 *          success, or NULL if the writer overflowed @p buf.
 *
 * @note    The returned pointer aliases @p buf and is valid only while @p
 *          buf remains live and unmodified by the caller.
 */
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

/**
 * @brief Serialize a follow-up `function_call_output` request body (blocking).
 *
 * Builds the JSON sent to /v1/responses to deliver a tool result back to the
 * model after a tool call resolved during the blocking openai_chat() path.
 * The request chains to the prior turn via `previous_response_id`.
 *
 * @param s             OpenAI provider implementation (for `model`). Must be
 *                      non-NULL.
 * @param call_id       The `call_id` echoed back from the model's
 *                      function_call. Must be a NUL-terminated C string.
 * @param tool_result   Textual output produced by the tool callback. Must be
 *                      a NUL-terminated C string.
 * @param buf           Caller-provided output buffer. Must be non-NULL.
 * @param cap           Capacity of @p buf in bytes.
 * @param chat_id_buf   The previous response id to chain against. Must be a
 *                      NUL-terminated C string.
 * @param char_id_len   Currently unused (reserved for future bounds checks).
 *
 * @return  Pointer into @p buf with the JSON body on success, NULL on
 *          writer overflow.
 */
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

/**
 * @brief HTTP transport callback that feeds raw bytes into the SSE line parser.
 *
 * Invoked by the HTTP layer for each chunk of response body received over
 * the streaming connection. Performs CRLF normalization (drops `\r`),
 * splits on `\n`, and forwards each completed line to
 * openai_stream_consume_line().
 *
 * @param chunk     Pointer to the received bytes. Need not be
 *                  NUL-terminated.
 * @param len       Number of valid bytes at @p chunk.
 * @param userdata  Opaque pointer; must point to an OpenAIStreamCtx.
 *
 * @return  0 to continue receiving, -1 to abort the HTTP stream (set when
 *          the line buffer overflows or a downstream callback fails).
 */
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

/**
 * @brief Serialize a follow-up `function_call_output` request body (streaming).
 *
 * Variant of openai_build_tool_result_body() used by the streaming path:
 * always sets `stream:true` and pulls call_id, tool_result, and the previous
 * response_id directly from the streaming tool accumulator.
 *
 * @param s         OpenAI provider implementation (for `model`). Must be
 *                  non-NULL.
 * @param tool_ctx  Snapshot of the streaming tool accumulator (passed by
 *                  value; the caller's copy is unaffected).
 * @param buf       Caller-provided output buffer. Must be non-NULL.
 * @param cap       Capacity of @p buf in bytes.
 *
 * @return  Pointer into @p buf with the JSON body on success, NULL on
 *          writer overflow.
 */
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

/**
 * @brief POST a JSON body to the OpenAI Responses endpoint and drive the SSE
 * loop.
 *
 * Hands the request body to the HTTP transport with
 * openai_http_stream_cb() as the chunk callback. After the HTTP call
 * returns, flushes any trailing partial line and any unterminated `data:`
 * payload through the parser so events that lack a final blank line are
 * still dispatched.
 *
 * @param s     OpenAI provider implementation (for `api_key`). Must be
 *              non-NULL.
 * @param st    Streaming context that accumulates parser state across this
 *              and any chained follow-up calls. Must be non-NULL.
 * @param json  NUL-terminated JSON request body. Must be non-NULL.
 *
 * @return  0 on a clean stream; CCLAW_INVALID if inputs are NULL;
 *          CCLAW_ERR_NETWORK if the HTTP transport, line dispatch, or final
 *          flush fails, or if the parser flagged `st->failed`.
 */
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

/**
 * @brief Streaming chat entry point installed on the provider vtable.
 *
 * Issues the initial streaming request and forwards text deltas to @p
 * on_chunk in real time. If the model responds with a tool call, the call
 * is dispatched against the CClaw tool registry and a second streaming
 * request is sent carrying the tool result, whose deltas continue to flow
 * through @p on_chunk.
 *
 * @param ctx       CClaw context (tool registry source). Must be non-NULL
 *                  if tools are expected.
 * @param self      The provider whose `impl` is an OpenAIImpl. Must be
 *                  non-NULL.
 * @param prompt    User prompt. Must be non-NULL and shorter than the JSON
 *                  body buffer can accommodate.
 * @param on_chunk  Callback invoked for each text delta. Returning non-zero
 *                  aborts the stream.
 * @param userdata  Opaque pointer forwarded verbatim to @p on_chunk.
 *
 * @return  0 on success; CCLAW_ERR_PARSE if either request body fails to
 *          serialize; otherwise the error code from
 *          openai_http_post_stream().
 */
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

/**
 * @brief Blocking chat entry point installed on the provider vtable.
 *
 * Sends a single non-streaming POST to the Responses API and inspects the
 * parsed reply. If the reply contains a `function_call`, the tool is run
 * via openai_dispatch_tool_call() and a second blocking request is sent
 * carrying the tool result; the second response replaces the first as the
 * returned value.
 *
 * @param ctx     CClaw context (tool registry + error reporting). Must be
 *                non-NULL.
 * @param self    The provider whose `impl` is an OpenAIImpl. Must be
 *                non-NULL.
 * @param prompt  User prompt. Must be non-NULL.
 *
 * @return  CClawString owning the raw HTTP response body on success
 *          (caller is responsible for freeing per CClawString conventions),
 *          or `{NULL, 0}` if request serialization fails or the network
 *          call returns no bytes. If a tool call is dispatched but the
 *          follow-up request returns no bytes, the original response is
 *          returned.
 */
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

/**
 * @brief Free entry point installed on the provider vtable.
 *
 * Zeroes the OpenAIImpl (so the API key does not linger in freed memory),
 * frees the impl, and frees the provider itself. Safe to call with NULL.
 *
 * @param self  Provider previously returned by cclaw_provider_openai(),
 *              or NULL.
 */
static void openai_free(CClawProvider *self) {
  if (!self)
    return;
  if (self->impl) {
    memset(self->impl, 0, sizeof(OpenAIImpl));
    free(self->impl);
  }
  free(self);
}

/**
 * @brief Construct an OpenAI-backed CClawProvider. See openai.h for the
 *        full contract.
 */
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
