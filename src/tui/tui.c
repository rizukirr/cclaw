#define TB_IMPL
#define TB_OPT_ATTR_W 32
#include "lib/termbox2.h"

#include "cclaw/response.h"
#include "core/internal.h"
#include "tui.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define INPUT_MAX 4096
#define LAST_SUBMIT_MAX 4096
#define STATUS_MSG_MAX 256
#define LOG_CAP 256
#define LOG_LINE_MAX 512

#define PAD_X 2
#define PAD_Y 1

typedef struct {
  char lines[LOG_CAP][LOG_LINE_MAX];
  uintattr_t colors[LOG_CAP];
  size_t head;
  size_t count;
} ChatLog;

typedef struct {
  int content_left, content_right, content_top, content_bottom;
  int log_top, log_rows;
  int sep_yt, sep_tb;
  int input_start_y;
  int status_y;
  int draw_w;
} TUILayout;

typedef struct {
  char input[INPUT_MAX];
  char pending_prompt[LAST_SUBMIT_MAX];
  char status_msg[STATUS_MSG_MAX];
  int pending_send;
  int use_stream;
  size_t scroll_offset;
  size_t input_len;
  size_t cursor;
  size_t preferred_col;
  ChatLog log;
} TUIState;

static void log_push(ChatLog *log, const char *text, uintattr_t color) {
  size_t idx = (log->head + log->count) % LOG_CAP;
  if (log->count == LOG_CAP) {
    log->head = (log->head + 1) % LOG_CAP;
    idx = (log->head + log->count - 1) % LOG_CAP;
  } else {
    log->count++;
  }

  const char *src = text ? text : "";
  size_t src_len = strlen(src);
  if (src_len >= LOG_LINE_MAX)
    src_len = LOG_LINE_MAX - 1;
  memcpy(log->lines[idx], src, src_len);
  log->lines[idx][src_len] = '\0';
  log->colors[idx] = color;
}

static int json_slice_eq_lit(JsonSlice s, const char *lit) {
  size_t n = strlen(lit);
  return s.data && s.len == n && memcmp(s.data, lit, n) == 0;
}

static JsonSlice extract_output_text(Response response) {
  for (size_t i = 0; i < MAX_OUTPUT; i++) {
    Output output = response.output[i];
    if (json_slice_eq_lit(output.type, "message")) {
      for (size_t j = 0; j < MAX_CONTENT; j++) {
        Content content = output.content[j];
        if (json_slice_eq_lit(content.type, "output_text")) {
          return content.text;
        }
      }
    }
  }
  return (JsonSlice){NULL, 0};
}

static TUIState tui_state_init(void) {
  TUIState state = {0};
  state.use_stream = 1;
  snprintf(state.status_msg, sizeof(state.status_msg),
           "status: ready (stream:on)");

  return state;
}

static size_t line_start(const char *s, size_t pos) {
  while (pos > 0 && s[pos - 1] != '\n')
    pos--;
  return pos;
}

static size_t line_end(const char *s, size_t len, size_t pos) {
  while (pos < len && s[pos] != '\n')
    pos++;
  return pos;
}

static size_t cursor_col(const TUIState *st) {
  return st->cursor - line_start(st->input, st->cursor);
}

static void input_insert_char(TUIState *st, char c) {
  if (st->input_len + 1 >= sizeof(st->input))
    return;

  memmove(&st->input[st->cursor + 1], &st->input[st->cursor],
          st->input_len - st->cursor + 1);

  st->input[st->cursor] = c;
  st->cursor++;
  st->input_len++;
  st->preferred_col = cursor_col(st);
}

static void input_backspace(TUIState *st) {
  if (st->cursor == 0)
    return;

  memmove(&st->input[st->cursor - 1], &st->input[st->cursor],
          st->input_len - st->cursor + 1);
  st->cursor--;
  st->input_len--;
  st->preferred_col = cursor_col(st);
}

static void cursor_left(TUIState *st) {
  if (st->cursor > 0)
    st->cursor--;

  st->preferred_col = cursor_col(st);
}

static void cursor_right(TUIState *st) {
  if (st->cursor < st->input_len)
    st->cursor++;

  st->preferred_col = cursor_col(st);
}

static void cursor_up(TUIState *st) {
  size_t cur_start = line_start(st->input, st->cursor);
  if (cur_start == 0)
    return;

  size_t prev_end = cur_start - 1;
  size_t prev_start = line_start(st->input, prev_end);
  size_t prev_len = prev_end - prev_start;
  size_t col = st->preferred_col > prev_len ? prev_len : st->preferred_col;
  st->cursor = prev_start + col;
}

static void cursor_down(TUIState *st) {
  size_t cur_end = line_end(st->input, st->input_len, st->cursor);
  if (cur_end >= st->input_len)
    return;
  size_t next_start = cur_end + 1;
  size_t next_end = line_end(st->input, st->input_len, next_start);
  size_t next_len = next_end - next_start;
  size_t col = st->preferred_col > next_len ? next_len : st->preferred_col;
  st->cursor = next_start + col;
}

static void input_cursor_xy(const TUIState *st, const TUILayout *ly, int *x,
                            int *y) {
  size_t row = 0, col = 0;
  for (size_t i = 0; i < st->cursor && i < st->input_len; i++) {
    if (st->input[i] == '\n') {
      row++;
      col = 0;
    } else {
      col++;
    }
  }

  *x = ly->content_left + (int)col;
  *y = ly->input_start_y + (int)row;
}

static TUILayout tui_layout_init(TUIState *st) {
  TUILayout ly = {0};
  int w = tb_width();
  int h = tb_height();

  ly.content_left = PAD_X;
  ly.content_top = PAD_Y;
  ly.content_right = w - PAD_X;
  ly.content_bottom = h - PAD_Y;

  ly.log_top = ly.content_top + 4;
  int base_input_y = ly.content_bottom - 3;
  ly.status_y = ly.content_bottom - 1;
  ly.draw_w = ly.content_right - ly.content_left;

  if (ly.draw_w < 1)
    ly.draw_w = 1;

  int input_lines = 1;
  for (size_t i = 0; i < st->input_len; i++) {
    if (st->input[i] == '\n')
      input_lines++;
  }

  ly.input_start_y = base_input_y - (input_lines - 1);
  ly.sep_yt = ly.input_start_y - 1;
  ly.sep_tb = ly.content_bottom - 2;
  ly.log_rows = ly.sep_yt - ly.log_top;

  return ly;
}

static void tui_draw(TUIState *st, TUILayout *ly, bool cursor_visible) {

  int cx = ly->content_left, cy = ly->input_start_y;
  input_cursor_xy(st, ly, &cx, &cy);

  if (cursor_visible && cx < ly->content_right && cy < ly->sep_tb) {
    tb_set_cursor(cx, cy);
  } else {
    tb_hide_cursor();
  }

  tb_printf(ly->content_left, ly->content_top, TB_WHITE, TB_DEFAULT,
            "cclaw tui");
  tb_printf(ly->content_left, ly->content_top + 1, TB_CYAN, TB_DEFAULT,
            "type /exit to quit");
  tb_printf(ly->content_left, ly->content_top + 2, TB_CYAN, TB_DEFAULT,
            "commands: /stream, /stream on, /stream off");
  tb_printf(ly->content_left, ly->content_top + 3, TB_YELLOW, TB_DEFAULT,
            "messages: %zu", st->log.count);

  for (int r = 0; r < ly->log_rows; r++) {
    int y = ly->log_top + r;
    tb_printf(ly->content_left, y, TB_DEFAULT, TB_DEFAULT, "%-*s", ly->draw_w,
              "");
  }

  if (st->log.count > 0 && ly->log_rows > 0) {
    size_t visible = st->log.count < (size_t)ly->log_rows
                         ? st->log.count
                         : (size_t)ly->log_rows;

    size_t max_offset =
        (st->log.count > visible) ? (st->log.count - visible) : 0;
    if (st->scroll_offset > max_offset)
      st->scroll_offset = max_offset;

    size_t start =
        (st->log.head + st->log.count - visible - st->scroll_offset) % LOG_CAP;
    int first_y = ly->sep_yt - (int)visible;
    for (size_t i = 0; i < visible; i++) {
      size_t idx = (start + i) % LOG_CAP;
      int y = first_y + (int)i;

      tb_printf(ly->content_left, y, st->log.colors[idx], TB_DEFAULT, "%s",
                st->log.lines[idx]);
    }
  }

  for (int x = 0; x < ly->draw_w; x++) {
    tb_set_cell(ly->content_left + x, ly->sep_yt, '-', TB_CYAN, TB_DEFAULT);
    tb_set_cell(ly->content_left + x, ly->sep_tb, '-', TB_CYAN, TB_DEFAULT);
  }

  if (st->input_len > 0) {
    int line_y = ly->input_start_y;
    size_t start = 0;
    for (size_t i = 0; i <= st->input_len; i++) {
      if (i == st->input_len || st->input[i] == '\n') {
        size_t chunk = i - start;
        tb_printf(ly->content_left, line_y, TB_WHITE, TB_DEFAULT, "%.*s",
                  (int)chunk, &st->input[start]);
        line_y++;
        start = i + 1;
      }
    }
  } else {
    tb_printf(ly->content_left, ly->input_start_y, TB_CYAN, TB_DEFAULT,
              "Type Something");
  }
  tb_printf(ly->content_right - strlen(st->status_msg), ly->status_y, TB_CYAN,
            TB_DEFAULT, "%s", st->status_msg);
}

static int tui_stream_on_chunk(const char *chunk, size_t len, void *userdata) {
  TUIState *st = (TUIState *)userdata;
  if (!st || st->log.count == 0)
    return -1;

  size_t idx = (st->log.head + st->log.count - 1) % LOG_CAP;
  size_t cur = strlen(st->log.lines[idx]);

  if (cur >= LOG_LINE_MAX - 1)
    return 0;

  size_t room = (LOG_LINE_MAX - 1) - cur;
  size_t n = len < room ? len : room;

  memcpy(st->log.lines[idx] + cur, chunk, n);
  st->log.lines[idx][cur + n] = '\0';

  tb_clear();
  TUILayout ly = tui_layout_init(st);
  tui_draw(st, &ly, true);
  tb_present();

  return 0;
}

static void tui_process_chat(CClaw *ctx, TUIState *st) {
  if (st->use_stream) {
    log_push(&st->log, "", TB_DEFAULT);
    int rc =
        cclaw_chat_stream(ctx, st->pending_prompt, tui_stream_on_chunk, st);
    if (rc != 0) {
      size_t idx = (st->log.head + st->log.count - 1) % LOG_CAP;
      snprintf(st->log.lines[idx], LOG_LINE_MAX, "(stream error: %d)", rc);
      snprintf(st->status_msg, sizeof(st->status_msg),
               "status: error (stream:on)");
    } else {
      size_t idx = (st->log.head + st->log.count - 1) % LOG_CAP;
      if (st->log.lines[idx][0] == '\0')
        snprintf(st->log.lines[idx], LOG_LINE_MAX, "(empty response)");
      snprintf(st->status_msg, sizeof(st->status_msg),
               "status: ready (stream:on)");
    }
  } else {
    CClawString res = cclaw_chat(ctx, st->pending_prompt);
    char line[LOG_LINE_MAX];

    if (res.str && res.len > 0) {
      ArenaCheckpoint checkpoint = arena_checkpoint(ctx->arena);
      Response parsed = cclaw_response_from_json(res);
      JsonSlice text = extract_output_text(parsed);

      if (text.data && text.len > 0)
        snprintf(line, sizeof(line), "%.*s", (int)text.len, text.data);
      else
        snprintf(line, sizeof(line), "%.*s", (int)res.len, res.str);

      log_push(&st->log, line, TB_DEFAULT);
      arena_restore(ctx->arena, checkpoint);
      snprintf(st->status_msg, sizeof(st->status_msg),
               "status: ready (stream:off)");
    } else {
      log_push(&st->log, "(no response)", TB_DEFAULT);
      snprintf(st->status_msg, sizeof(st->status_msg),
               "status: error (stream:off)");
    }
  }
  st->pending_send = 0;
}

typedef enum { TUI_EVENT_OK, TUI_EVENT_BREAK, TUI_EVENT_CONTINUE } TUIEvent;

static inline TUIEvent tui_handle_command(TUIState *st, const char *cmd) {
  if (strcmp(cmd, "/exit") == 0)
    return TUI_EVENT_BREAK;

  if (strcmp(cmd, "/stream on") == 0) {
    st->use_stream = 1;
    snprintf(st->status_msg, sizeof(st->status_msg),
             "status: ready (stream:on)");
    return TUI_EVENT_CONTINUE;
  }

  if (strcmp(cmd, "/stream off") == 0) {
    st->use_stream = 0;
    snprintf(st->status_msg, sizeof(st->status_msg),
             "status: ready (stream:off)");
    return TUI_EVENT_CONTINUE;
  }

  if (strcmp(cmd, "/stream") == 0) {
    log_push(&st->log, st->use_stream ? "stream:on" : "stream:off", TB_CYAN);
    return TUI_EVENT_CONTINUE;
  }

  return TUI_EVENT_OK;
}

static TUIEvent tui_handle_event(TUIState *st, struct tb_event *ev) {
  if (ev->key == TB_KEY_CTRL_Q)
    return TUI_EVENT_BREAK;

  if (ev->key == TB_KEY_PGUP) {
    st->scroll_offset++;
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_PGDN) {
    if (st->scroll_offset > 0)
      st->scroll_offset--;
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ARROW_UP) {
    cursor_up(st);
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ARROW_DOWN) {
    cursor_down(st);
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ARROW_LEFT) {
    cursor_left(st);
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ARROW_RIGHT) {
    cursor_right(st);
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    input_backspace(st);
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_CTRL_N) {
    input_insert_char(st, '\n');
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ENTER && (ev->mod & TB_MOD_ALT)) {
    input_insert_char(st, '\n');
    return TUI_EVENT_CONTINUE;
  }

  if (ev->key == TB_KEY_ENTER) {
    if (st->input_len == 0)
      return TUI_EVENT_CONTINUE;

    TUIEvent cmd = tui_handle_command(st, st->input);
    if (cmd != TUI_EVENT_OK) {
      st->input_len = 0;
      st->input[0] = '\0';
      st->cursor = 0;
      st->preferred_col = 0;
      return cmd;
    }

    memcpy(st->pending_prompt, st->input, st->input_len + 1);
    log_push(&st->log, st->pending_prompt, TB_MAGENTA);

    st->pending_send = 1;
    snprintf(st->status_msg, sizeof(st->status_msg), "status: thinking...");

    st->scroll_offset = 0;
    st->input_len = 0;
    st->input[0] = '\0';
    st->cursor = 0;
    st->preferred_col = 0;
    return TUI_EVENT_CONTINUE;
  }

  if (ev->ch >= 32 && ev->ch <= 126) {
    input_insert_char(st, (char)ev->ch);
  }

  return TUI_EVENT_OK;
}

int tui_run(CClaw *ctx) {
  int tb_rc = tb_init();
  if (tb_rc != TB_OK)
    return 1;

  tb_set_input_mode(TB_INPUT_ALT);

  TUIState st = tui_state_init();
  bool running = true;
  bool cursor_visible = true;
  int blink_ticks = 0;

  while (running) {
    tb_clear();
    TUILayout ly = tui_layout_init(&st);
    tui_draw(&st, &ly, cursor_visible);
    tb_present();

    // Chat
    if (st.pending_send) {
      tui_process_chat(ctx, &st);
      continue;
    }

    // Key events
    struct tb_event ev;
    int ev_ret = tb_peek_event(&ev, 120);
    if (ev_ret == TB_ERR_NO_EVENT) {
      blink_ticks++;
      if (blink_ticks >= 4) {
        blink_ticks = 0;
        cursor_visible = !cursor_visible;
      }
      continue;
    }
    if (ev_ret < 0)
      continue;

    cursor_visible = true;
    blink_ticks = 0;
    if (ev.type != TB_EVENT_KEY)
      continue;

    TUIEvent te = tui_handle_event(&st, &ev);
    switch (te) {
    case TUI_EVENT_BREAK:
      running = false;
      continue;
    case TUI_EVENT_CONTINUE:
    case TUI_EVENT_OK:
      continue;
    }
  }

  tb_shutdown();
  return 0;
}
