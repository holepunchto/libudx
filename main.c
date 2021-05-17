#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/ucp.h"
#include "src/fifo.h"

static char *buf = "hello world";
static size_t buf_len = 11;

static ucp_send_t req[16];

static void
on_message (ucp_t *self, char *buf, ssize_t nread, const struct sockaddr_in *from) {
  buf[nread] = 0;
  printf("got message: \"%s\" from %i:%i\n", buf, from->sin_addr, from->sin_port);
}

int
main () {
  uv_loop_t* loop = malloc(sizeof(uv_loop_t));
  uv_loop_init(loop);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", 7654, &addr);

  printf("hello world\n");

  ucp_t self;

  ucp_init(&self, loop);
  ucp_bind(&self, (const struct sockaddr *) &addr);

  uv_ip4_addr("127.0.0.1", 10000, &addr);

  ucp_set_callback(&self, UCP_ON_MESSAGE, on_message);

  ucp_send(&self, &(req[0]), buf, buf_len, (const struct sockaddr *) &addr);
  ucp_send(&self, &(req[1]), buf, 5, (const struct sockaddr *) &addr);
  ucp_send(&self, &(req[2]), buf, 5, (const struct sockaddr *) &addr);
  ucp_send(&self, &(req[3]), buf, 5, (const struct sockaddr *) &addr);
  ucp_send(&self, &(req[4]), buf, 5, (const struct sockaddr *) &addr);
  ucp_send(&self, &(req[5]), buf, 5, (const struct sockaddr *) &addr);

  printf("running...\n");
  uv_run(loop, UV_RUN_DEFAULT);
  free(loop);

  return 0;
}
