#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include <udx.h>

static char *buf;
static size_t buf_len = 1300;
static int rt = 100;
static size_t rcvd = 0;
static size_t sent = 0;

static udx_send_t req[16];

static void
on_message (udx_t *self, char *buf, ssize_t nread, const struct sockaddr_in *from) {
  buf[nread] = 0;
  printf("got message: \"%s\" from %i:%i\n", buf, from->sin_addr, from->sin_port);
}

static void
on_read (udx_stream_t *stream, char *buf, size_t read) {
  rcvd += read;

  printf("total recv=%zu, total sent=%zu, rt=%i\n", rcvd, sent, rt);

  if (rt == 0) {
    exit(0);
  }

  udx_stream_send_state(stream);
}

static void
on_write (udx_stream_t *stream, udx_write_t *req, int status) {
  // printf("on write\n");

  if (rt == 0) return;

  if (--rt > 0) {
    sent += buf_len;
    udx_stream_write(stream, req, buf, buf_len);
  }
}

int
main () {
  buf = calloc(buf_len, 1);

  printf("microtime is %llu\n", udx_get_microseconds());

  srand(time(0));

  printf("microtime is %llu\n", udx_get_microseconds());

  uv_loop_t* loop = malloc(sizeof(uv_loop_t));
  uv_loop_init(loop);

  struct sockaddr_in addr;

  printf("booting... %zu\n", sizeof(struct sockaddr));

  udx_t client;
  udx_t server;

  // client is on 7654
  uv_ip4_addr("0.0.0.0", 7654, &addr);
  udx_init(&client, loop);
  udx_bind(&client, (const struct sockaddr *) &addr);

  // server is on 10000
  uv_ip4_addr("0.0.0.0", 10000, &addr);
  udx_init(&server, loop);
  udx_bind(&server, (const struct sockaddr *) &addr);

  // udx_set_callback(&client, UDX_ON_MESSAGE, on_message);
  // udx_send(&client, &(req[0]), buf, buf_len, (const struct sockaddr *) &addr);
  // udx_send(&client, &(req[1]), buf, 5, (const struct sockaddr *) &addr);
  // udx_send(&client, &(req[2]), buf, 5, (const struct sockaddr *) &addr);
  // udx_send(&client, &(req[3]), buf, 5, (const struct sockaddr *) &addr);
  // udx_send(&client, &(req[4]), buf, 5, (const struct sockaddr *) &addr);
  // udx_send(&client, &(req[5]), buf, 5, (const struct sockaddr *) &addr);

  int id;

  udx_stream_t client_sock;
  udx_stream_init(&client, &client_sock, &id);

  udx_stream_t server_sock;
  udx_stream_init(&server, &server_sock, &id);

  printf("client stream id is: %u\n", client_sock.local_id);
  printf("server stream id is: %u\n", server_sock.local_id);

  uv_ip4_addr("127.0.0.1", 10000, &addr);
  udx_stream_connect(&client_sock, server_sock.local_id, (const struct sockaddr *) &addr);

  uv_ip4_addr("127.0.0.1", 7654, &addr);
  udx_stream_connect(&server_sock, client_sock.local_id, (const struct sockaddr *) &addr);

  udx_stream_set_callback(&server_sock, UDX_ON_READ, on_read);
  udx_stream_set_callback(&client_sock, UDX_ON_ACK, on_write);

  for (int i = 0; i < 1000; i++) {
    udx_write_t *req = (udx_write_t *) malloc(sizeof(udx_write_t));
    sent += buf_len;
    udx_stream_write(&client_sock, req, buf, buf_len);
  }

  printf("running...\n");

  uv_run(loop, UV_RUN_DEFAULT);
  free(loop);

  return 0;
}
