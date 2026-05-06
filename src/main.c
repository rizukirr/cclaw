#include "cclaw/cclaw.h"
#include "lib/libjson.h"
#include "tui/repl.h"
#include <stdio.h>
#include <string.h>

static int tool_add(const char *args_json, size_t args_len, char *out,
                    size_t out_cap, void *user) {

  char buf[args_len + 1];
  size_t buf_len = str_skip_escaped((char *)args_json, args_len, buf);

  (void)user;
  JsonSlice root = json_from_parts(buf, buf_len);
  JsonSlice a = {0};
  JsonSlice b = {0};

  if (!json_get(root, "a", &a) || !json_get(root, "b", &b))
    return -1;

  int av = 0, bv = 0;

  if (sscanf(a.data, "%d", &av) != 1 || sscanf(b.data, "%d", &bv) != 1)
    return -1;

  return snprintf(out, out_cap, "%d", av + bv) < 0 ? -1 : 0;
}

int main(void) {
  CClaw *ctx = cclaw_init();
  if (!ctx) {
    fprintf(stderr, "cclaw: init failed\n");
    return 1;
  }

  CClawTool add = {
      .name = "add",
      .description = "add two integers a and b, return a+b",
      .params_schema = "{\"type\":\"object\","
                       "\"properties\":{"
                       "\"a\":{\"type\":\"integer\"},"
                       "\"b\":{\"type\":\"integer\"}},"
                       "\"required\":[\"a\",\"b\"]}",
      .fn = tool_add,
  };

  cclaw_tool_register(ctx, &add);

  int rc = repl_run(ctx);
  cclaw_destroy(ctx);
  return rc;
}
