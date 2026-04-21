#include "cclaw/cclaw.h"
#include "tui/tui.h"
#include <stdio.h>

int main(void) {
  CClaw *ctx = cclaw_init();
  if (!ctx) {
    fprintf(stderr, "cclaw: init failed\n");
    return 1;
  }

  int rc = tui_run(ctx);
  cclaw_destroy(ctx);
  return rc;
}
