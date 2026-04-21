#include "cclaw/error.h"
#include <assert.h>

int main(void) {
  cclaw_strerror(CCLAW_ERR_OOM, "out of memory");
  return 0;
}
