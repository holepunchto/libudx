#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;
udx_lookup_t req;

void
on_lookup (udx_lookup_t *req, int status, const struct sockaddr *addr, int addr_len) {
  assert(status == 0);
  assert(addr->sa_family == AF_INET6);
  assert(addr_len == sizeof(struct sockaddr_in6));

  char ip[INET_ADDRSTRLEN];
  uv_ip6_name((struct sockaddr_in6 *) addr, ip, INET6_ADDRSTRLEN);

  assert(strcmp(ip, "::1") == 0);

  uv_stop(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);
  udx_init(&loop, &udx);

  e = udx_lookup(&udx, &req, "localhost", UDX_LOOKUP_FAMILY_IPV6, on_lookup);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}
