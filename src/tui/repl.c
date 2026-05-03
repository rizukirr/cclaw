#include "repl.h"

#include "cclaw/response.h"
#include "core/internal.h"
#include "event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPL_INPUT_MAX 256
#define REPL_OUTPU_MAX 256

typedef struct {
  char input[REPL_INPUT_MAX];
  char output[REPL_INPUT_MAX];
  bool stream;
} ReplState;

static ReplState repl_state_init(void) {
  ReplState st = {
      .stream = true,
  };

  return st;
}

static int repl_stream_on_chunk(const char *chunk, size_t len, void *userdata) {
  if (!chunk || len == 0 || !userdata)
    return -1;

  ReplState *st = (ReplState *)userdata;
  memcpy(st->output + strlen(st->output), chunk, len);
  return 0;
}

static CClawEvent repl_handle_command(ReplState *st, const char *cmd) {
  if (strcmp(cmd, "/stream on") == 0) {
    st->stream = true;
    return CCLAW_EVENT_CONTINUE;
  }

  if (strcmp(cmd, "/stream off") == 0) {
    st->stream = false;
    return CCLAW_EVENT_CONTINUE;
  }

  if (strcmp(cmd, "/exit") == 0 || strcmp(cmd, "/quit") == 0) {
    return CCLAW_EVENT_BREAK;
  }

  return CCLAW_EVENT_OK;
}

static int json_slice_eq_lit(JsonSlice s, const char *lit) {
  size_t n = strlen(lit);
  return s.data && s.len == n && memcmp(s.data, lit, n) == 0;
}

static JsonSlice repl_extract_output_text(Response response) {
  for (size_t i = 0; i < MAX_OUTPUT; i++) {
    Output output = response.output[i];
    if (json_slice_eq_lit(output.type, "message")) {
      for (size_t j = 0; j < MAX_CONTENT; j++) {
        Content content = output.content[j];
        if (json_slice_eq_lit(content.type, "output_text"))
          return content.text;
      }
    }
  }
  return (JsonSlice){NULL, 0};
}

int repl_run(CClaw *ctx) {
  if (!ctx)
    return 1;

  ReplState st = {0};

  puts("cclaw raw repl — type /exit or /quit to close");
  while (true) {
    fputs("> ", stdout);
    fflush(stdout);

    st = repl_state_init();

    if (!fgets(st.input, sizeof(st.input), stdin))
      break;

    size_t len = strlen(st.input);
    if (len > 0 && st.input[len - 1] == '\n')
      st.input[len - 1] = '\0';

    CClawEvent ev = repl_handle_command(&st, st.input);
    if (ev == CCLAW_EVENT_BREAK)
      break;
    if (ev == CCLAW_EVENT_CONTINUE)
      continue;

    if (st.input[0] == '\0')
      continue;

    if (st.stream) {
      int err = cclaw_chat_stream(ctx, st.input, repl_stream_on_chunk, &st);
      if (err != 0) {
        fprintf(stderr, "error stream\n");
        continue;
      }

      fprintf(stderr, "response: %s\n", st.output);
      continue;
    }

    CClawString res = cclaw_chat(ctx, st.input);
    if (!res.str || res.len == 0) {
      puts("(no response)");
      continue;
    }

    fprintf(stderr, "response: %.*s\n", (int)res.len, res.str);

    Response parsed = {0};
    if (cclaw_is_response_error(ctx, res)) {
      parsed = cclaw_response_error_from_json(res);
      fprintf(stderr, "error: %.*s\n", (int)parsed.error.message.len,
              parsed.error.message.data);
    } else {
      parsed = cclaw_response_from_json(res);
      JsonSlice text = repl_extract_output_text(parsed);

      if (text.data && text.len > 0)
        fprintf(stderr, "%.*s\n", (int)text.len, text.data);
      else
        fprintf(stderr, "%.*s\n", (int)res.len, res.str);
    }
  }

  return 0;
}
