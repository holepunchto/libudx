#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_lookup_t req;

void
on_lookup (udx_lookup_t *req, int status, const struct sockaddr *addr, int addr_len) {
  assert(addr == NULL);

  uv_stop(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_lookup(&loop, &req, "example.invalid.", 0, on_lookup);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}
