#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "../src/udx.h"
#include "../src/fifo.h"
#include "../src/cirbuf.h"
#include "../src/utils.h"

#define PARALLEL_WRITES 3000

static udx_t client;
static udx_stream_t client_sock;
static udx_send_t sreq;
static uv_timer_t timer;
static uint32_t sbuf;

static size_t sent = 0;
static size_t sent_prev = 0;
static int rt = 10000000;
static udx_write_t * pending_reqs[PARALLEL_WRITES];
static int pending_writes = 0;
static uint64_t start_time = 0;
static size_t send_buf_len = UDX_MAX_DATA_SIZE;
static char *send_buf;

static void
on_uv_interval (uv_timer_t *req) {
  int bw = 8 * sent / ((udx_get_microseconds() - start_time) / 1000 / 1000) / 1000;
  int top = bw / 100;
  int btm = top % 10;

  int bw_d = 8 * (sent - sent_prev) / 1000;
  int top_d = bw_d / 100;
  int btm_d = top_d % 10;

  top /= 10;
  top_d /= 10;

  sent_prev = sent;

  printf("lseq=%u, pwr=%u, rt=%u, frt=%zu, racks=%u, sacks=%zu, pkts_s=%zu, pkts_w=%u, pkts_inf=%u, byt_inf=%zu, cwnd=%zu rto=%u rtt=%u Mbps=%i,%i (%i,%i)\n",
    client_sock.stats_last_seq,
    pending_writes,
    rt,
    client_sock.stats_fast_rt,
    client_sock.remote_acked,
    client_sock.stats_sacks,
    client_sock.stats_pkts_sent,
    client_sock.pkts_waiting,
    client_sock.pkts_inflight,
    client_sock.inflight,
    client_sock.cwnd,
    client_sock.rto,
    client_sock.srtt,
    top,
    btm,
    top_d,
    btm_d
  );

  udx_stream_check_timeouts(&client_sock);
}

static void
on_message (udx_t *self, char *buf, ssize_t nread, const struct sockaddr_in *from) {
  if (nread < 4) return;

  udx_set_callback(&client, UDX_ON_MESSAGE, NULL);

  uint32_t id = *((uint32_t *) buf);
  printf("remote socket id: %u\n", id);

  send_buf = (char *) calloc(send_buf_len, 1);

  udx_stream_connect(&client_sock, id, (const struct sockaddr *) from);

  for (int i = 0; i < PARALLEL_WRITES; i++) {
    udx_write_t *req = (udx_write_t *) malloc(sizeof(udx_write_t));
    sent += send_buf_len;
    udx_stream_write(&client_sock, req, send_buf, send_buf_len);
  }

  uv_timer_start(&timer, on_uv_interval, 1000, 1000);
}

static void
on_write (udx_stream_t *stream, udx_write_t *req, int status, int unordered) {
  // printf("on write\n");

  if (unordered) {
    pending_reqs[pending_writes++] = req;
    return;
  }

  if (--rt > 0) {
    // if (pending_writes) printf("ordered write... %i\n", pending_writes);
    sent += send_buf_len;
    udx_stream_write(stream, req, send_buf, send_buf_len);
    while (pending_writes > 0) {
      udx_stream_write(stream, pending_reqs[--pending_writes], send_buf, send_buf_len);
    }
  }

  // printf("total sent=%zu, rt=%i\n", sent, rt);

  if (rt == 0) {
    printf("total sent=%zu, rt=%i\n", sent, rt);
    exit(0);
  }
}

int
main (int argc, char **argv) {
  srand(time(0));

  start_time = udx_get_microseconds();

  uv_loop_t* loop = malloc(sizeof(uv_loop_t));
  uv_loop_init(loop);
  uv_timer_init(loop, &timer);

  struct sockaddr_in addr;

  udx_init(&client, loop);

  int b = 2 * 1024 * 1024;
  udx_send_buffer_size(&client, &b);
  udx_recv_buffer_size(&client, &b);

  uv_ip4_addr("0.0.0.0", 10102, &addr);
  udx_bind(&client, (const struct sockaddr *) &addr);

  udx_set_callback(&client, UDX_ON_MESSAGE, on_message);

  int id;

  udx_stream_init(&client, &client_sock, &id);

  printf("local socket id: %u\n", client_sock.local_id);

  sbuf = client_sock.local_id;

  uv_ip4_addr(argc == 1 ? "127.0.0.1" : argv[1], 10101, &addr);
  udx_send(&client, &sreq, (char *) &sbuf, 4, (const struct sockaddr *) &addr);

  udx_stream_set_callback(&client_sock, UDX_ON_ACK, on_write);

  // printf("server stream id is: %u\n", server_sock.local_id);

  // udx_stream_connect(&server_sock, client_sock.local_id, (const struct sockaddr *) &addr);

  // udx_stream_set_callback(&server_sock, UDX_ON_READ, on_read);

  // for (int i = 0; i < 1000; i++) {
  //   udx_write_t *req = (udx_write_t *) malloc(sizeof(udx_write_t));
  //   sent += buf_len;
  //   udx_stream_write(&client_sock, req, buf, buf_len);
  // }

  // printf("running...\n");

  uv_run(loop, UV_RUN_DEFAULT);
  free(loop);

  return 0;
}
