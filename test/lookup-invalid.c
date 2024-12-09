#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

udx_t udx;
uv_loop_t loop;
udx_lookup_t req;

void
on_lookup (udx_lookup_t *req, int status, const struct sockaddr *addr, int addr_len) {
  assert(status < 0);
  assert(addr == NULL);
  assert(addr_len == 0);

  uv_stop(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);
  udx_init(&loop, &udx);

  e = udx_lookup(&udx, &req, "example.invalid.", 0, on_lookup);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}
