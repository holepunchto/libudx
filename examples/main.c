#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "../src/ucp.h"
#include "../src/fifo.h"
#include "../src/cirbuf.h"

static char *buf;
static size_t buf_len = 1300;
static int rt = 1234560;
static size_t rcvd = 0;
static size_t sent = 0;

static ucp_send_t req[16];

static void
on_message (ucp_t *self, char *buf, ssize_t nread, const struct sockaddr_in *from) {
  buf[nread] = 0;
  printf("got message: \"%s\" from %i:%i\n", buf, from->sin_addr, from->sin_port);
}

static void
on_read (ucp_stream_t *stream, char *buf, size_t read) {
  rcvd += read;
  // if (recv == sent) {
    // printf("on read %zu, total recv=%zu total sent=%zu\n", read, rcvd, sent);
  // }

  if (rt == 0) {
    printf("total recv=%zu total sent=%zu\n", rcvd, sent);
    exit(0);
  }

  ucp_stream_send_state(stream);
}

static void
on_write (ucp_stream_t *stream, ucp_write_t *req, int status) {
  // printf("on write\n");

  if (--rt > 0) {
    sent += buf_len;
    ucp_stream_write(stream, req, buf, buf_len);
  }

  if ((rt % 50000) == 0) {
    printf("total recv=%zu total sent=%zu\n", rcvd, sent);
  }
}

int
main () {
  buf = calloc(buf_len, 1);

  srand(time(0));

  uv_loop_t* loop = malloc(sizeof(uv_loop_t));
  uv_loop_init(loop);

  struct sockaddr_in addr;

  printf("booting... %zu\n", sizeof(struct sockaddr));

  ucp_t client;
  ucp_t server;

  // client is on 7654
  uv_ip4_addr("0.0.0.0", 7654, &addr);
  ucp_init(&client, loop);
  ucp_bind(&client, (const struct sockaddr *) &addr);

  // server is on 10000
  uv_ip4_addr("0.0.0.0", 10000, &addr);
  ucp_init(&server, loop);
  ucp_bind(&server, (const struct sockaddr *) &addr);

  // ucp_set_callback(&client, UCP_ON_MESSAGE, on_message);
  // ucp_send(&client, &(req[0]), buf, buf_len, (const struct sockaddr *) &addr);
  // ucp_send(&client, &(req[1]), buf, 5, (const struct sockaddr *) &addr);
  // ucp_send(&client, &(req[2]), buf, 5, (const struct sockaddr *) &addr);
  // ucp_send(&client, &(req[3]), buf, 5, (const struct sockaddr *) &addr);
  // ucp_send(&client, &(req[4]), buf, 5, (const struct sockaddr *) &addr);
  // ucp_send(&client, &(req[5]), buf, 5, (const struct sockaddr *) &addr);

  ucp_stream_t client_sock;
  ucp_stream_init(&client, &client_sock);

  ucp_stream_t server_sock;
  ucp_stream_init(&server, &server_sock);

  printf("client stream id is: %u\n", client_sock.local_id);
  printf("server stream id is: %u\n", server_sock.local_id);

  uv_ip4_addr("127.0.0.1", 10000, &addr);
  ucp_stream_connect(&client_sock, server_sock.local_id, (const struct sockaddr *) &addr);

  uv_ip4_addr("127.0.0.1", 7654, &addr);
  ucp_stream_connect(&server_sock, client_sock.local_id, (const struct sockaddr *) &addr);

  ucp_stream_set_callback(&server_sock, UCP_ON_READ, on_read);
  ucp_stream_set_callback(&client_sock, UCP_ON_WRITE, on_write);

  for (int i = 0; i < 1000; i++) {
    ucp_write_t *req = (ucp_write_t *) malloc(sizeof(ucp_write_t));
    sent += buf_len;
    ucp_stream_write(&client_sock, req, buf, buf_len);
  }

  printf("running...\n");

  uv_run(loop, UV_RUN_DEFAULT);
  free(loop);

  return 0;
}
