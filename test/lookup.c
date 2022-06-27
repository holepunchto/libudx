#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_lookup_t req;

void
on_lookup (udx_lookup_t *req, int status, struct sockaddr *addr, int addr_len) {
  assert(addr->sa_family == AF_INET);

  char ip[INET_ADDRSTRLEN];
  uv_ip4_name((struct sockaddr_in *) addr, ip, INET_ADDRSTRLEN);

  assert(strcmp(ip, "127.0.0.1") == 0);

  uv_stop(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_lookup(&loop, &req, "localhost", UDX_LOOKUP_FAMILY_IPV4, on_lookup);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}
