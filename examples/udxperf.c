/* includes code from iperf2:
 *
 * ---------------------------------------------------------------
 * Copyright (c) 1999,2000,2001,2002,2003
 * The Board of Trustees of the University of Illinois
 * All Rights Reserved.
 *---------------------------------------------------------------
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software (Iperf) and associated
 * documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 *
 * Redistributions of source code must retain the above
 * copyright notice, this list of conditions and
 * the following disclaimers.
 *
 *
 * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimers in the documentation and/or other materials
 * provided with the distribution.
 *
 *
 * Neither the names of the University of Illinois, NCSA,
 * nor the names of its contributors may be used to endorse
 * or promote products derived from this Software without
 * specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ________________________________________________________________
 * National Laboratory for Applied Network Research
 * National Center for Supercomputing Applications
 * University of Illinois at Urbana-Champaign
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________
 *
 * some functions stdio.c
 * by Mark Gates <mgates@nlanr.net>
 * and Ajay Tirumalla <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * input and output numbers, converting with kilo, mega, giga
 * ------------------------------------------------------------------- */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <time.h>

#include "../include/udx.h"
// options

static bool is_server;
static bool is_client;
static bool extra_wanted;
char *addr_string;
static struct sockaddr_in laddr;
static struct sockaddr_in raddr;
static uint16_t port = 5001;

static int time_ms;
static int interval_ms;
static int num_bytes_to_transmit; // 0 if time is set (default)

// end options

static uv_loop_t loop;
static udx_t udx;

static udx_socket_t sock;

static udx_socket_send_t handshake_req;

static uv_timer_t client_complete_timer;

static bool simulate_load = false;

typedef struct {
  udx_stream_t stream;
  uint64_t start_time;
  uint64_t end_time;
  bool ended;
  uint64_t time_last_report;
  uint64_t bytes_last_report;
  uv_timer_t report_timer;
  uv_timer_t load_timer;
} udxperf_client_t;

uint32_t client_id = 1;
uint32_t server_id;

uint64_t bytes_queued;
uint64_t bytes_sent;
uint64_t bytes_acked;

bool active;

unsigned char chunk_bytes[0x10000];

// byte printing and conversions
// from iperf2

// e.g.
// char buf[20];
// byte_snprintf(buf, sizeof(buf), 10000, 'A');

/* -------------------------------------------------------------------
 * constants for byte_printf
 * ------------------------------------------------------------------- */

/* used as indices into kConversion[], kLabel_Byte[], and kLabel_bit[] */
enum {
  kConv_Unit,
  kConv_Kilo,
  kConv_Mega,
  kConv_Giga,
  kConv_Tera,
  kConv_Peta
};

/* factor to multiply the number by */
const double kConversion[] =
  {
    1.0,                                   /* unit */
    1.0 / 1024,                            /* kilo */
    1.0 / 1024 / 1024,                     /* mega */
    1.0 / 1024 / 1024 / 1024,              /* giga */
    1.0 / 1024 / 1024 / 1024 / 1024,       /* tera */
    1.0 / 1024 / 1024 / 1024 / 1024 / 1024 /* peta */
};

/* factor to multiply the number by for bits*/
const double kConversionForBits[] =
  {
    1.0,                                   /* unit */
    1.0 / 1000,                            /* kilo */
    1.0 / 1000 / 1000,                     /* mega */
    1.0 / 1000 / 1000 / 1000,              /* giga */
    1.0 / 1000 / 1000 / 1000 / 1000,       /* tera */
    1.0 / 1000 / 1000 / 1000 / 1000 / 1000 /* peta */
};

/* labels for Byte formats [KMG] */
const char *kLabel_Byte[] =
  {
    "Byte",
    "KByte",
    "MByte",
    "GByte",
    "TByte",
    "PByte"
};

/* labels for bit formats [kmg] */
const char *kLabel_bit[] =
  {
    "bit",
    "Kbit",
    "Mbit",
    "Gbit",
    "Tbit",
    "Pbit"
};

/* -------------------------------------------------------------------
 * byte_snprintf
 *
 * Given a number in bytes and a format, converts the number and
 * prints it out with a bits or bytes label.
 *   B, K, M, G, A, P, T for Byte, Kbyte, Mbyte, Gbyte, Tbyte, Pbyte adaptive byte
 *   b, k, m, g, a, p, t for bit,  Kbit,  Mbit,  Gbit, Tbit, Pbit, adaptive bit
 * adaptive picks the "best" one based on the number.
 * outString should be at least 11 chars long
 * (4 digits + space + 5 chars max + null)
 * ------------------------------------------------------------------- */

void
byte_snprintf (char *buf, int buf_len, double num, int fmtchar) {
  int conv = 0;
  const char *suffix;
  const char *format;
  if (!isupper(fmtchar)) {
    num *= 8;
  }

  switch (toupper(fmtchar)) {
  case 'B':
    conv = kConv_Unit;
    break;
  case 'K':
    conv = kConv_Kilo;
    break;
  case 'M':
    conv = kConv_Mega;
    break;
  case 'G':
    conv = kConv_Giga;
    break;
  case 'T':
    conv = kConv_Tera;
    break;
  case 'P':
    conv = kConv_Peta;
    break;

  default:
  case 'A': {
    double d = num;
    conv = kConv_Unit;

    if (isupper(fmtchar)) {
      while (d >= 1024.0 && conv < kConv_Peta) {
        d /= 1024.0;
        conv++;
      }
    } else {
      while (d >= 1000.0 && conv < kConv_Peta) {
        d /= 1000.0;
        conv++;
      }
    }
    break;
  }
  }

  if (!isupper(fmtchar)) {
    num *= kConversionForBits[conv];
    suffix = kLabel_bit[conv];
  } else {
    num *= kConversion[conv];
    suffix = kLabel_Byte[conv];
  }

  /* print such that we always fit in 4 places */
  if (num < 9.995) {        /* 9.995 would be rounded to 10.0 */
    format = "%4.2f %s";    /* #.## */
  } else if (num < 99.95) { /* 99.95 would be rounded to 100 */
    format = "%4.1f %s";    /* ##.# */
  } else if (num < 999.5) { /* 999.5 would be rounded to 1000 */
    format = "%4.0f %s";    /*  ### */
  } else {                  /* 1000-1024 fits in 4 places
                             * If not using Adaptive sizes then
                             * this code will not control spaces*/
    format = "%4.0f %s";    /* #### */
  }
  snprintf(buf, buf_len, format, num, suffix);
}

static void
print_interval (udxperf_client_t *client, uint64_t bytes, uint64_t start, uint64_t end) {
  // [ 18] local 127.0.0.1 port 5001 connected with 127.0.0.1 port 58518 (icwnd/mss/irtt=320/32768/640174)
  // [ ID] Interval            Transfer     Bandwidth
  // [ 18] 0.0000-20.5079 sec  76.5 MBytes  31.3 Mbits/sec

  udx_stream_t *stream = &client->stream;

  double time_sec = (end - start) / 1000.0;
  double bits_per_sec = bytes * 8 / (double) ((end - start) / 1000.0);

  char bytes_buf[20];
  byte_snprintf(bytes_buf, sizeof bytes_buf, bytes, 'A');
  bytes_buf[19] = '\0';

  char bps_buf[20];
  byte_snprintf(bps_buf, sizeof bps_buf, bytes / time_sec, 'a');
  bps_buf[19] = '\0';

  int64_t k_drop = stream->socket->packets_dropped_by_kernel || udx.packets_dropped_by_kernel;
  int64_t t_drop = -1;
  double q_load = 0;

#ifdef USE_DRAIN_THREAD
  t_drop = stream->socket->packets_dropped_by_worker || udx.packets_dropped_by_worker;
  q_load = udx__drainer_read_load(&udx, NULL, NULL);
#endif

  printf("[%3d] %6.4f-%6.4f sec %s %s/sec \t(kD %zi, wD %zi) q_load: %f", stream->local_id, (start - client->start_time) / 1000.0, (end - client->start_time) / 1000.0, bytes_buf, bps_buf, k_drop, t_drop, q_load);

  if (is_client && extra_wanted) {
    printf(" cwnd=%d ssthresh=%d fast_recovery_count=%d rto_count=%d rtx_count=%d", stream->cwnd, stream->ssthresh, stream->fast_recovery_count, stream->rto_count, stream->retransmit_count);
  }
  printf("\n");
}

static void
server_on_destroy (udx_stream_t *stream, int status) {
  assert(status == 0);
}

static void
server_on_finalize (udx_stream_t *stream) {
  free(stream);
  exit(0);
}

static void
server_on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  udxperf_client_t *c = (udxperf_client_t *) stream;
  c->end_time = uv_now(&loop);

  if (read_len == UV_EOF) {
    print_interval(c, stream->bytes_rx, c->start_time, c->end_time);
    c->ended = true;
    if (interval_ms) {
      uv_timer_stop(&c->report_timer);
      uv_close((uv_handle_t *) &c->report_timer, NULL);
    }

    if (simulate_load) {
      uv_timer_stop(&c->load_timer);
      uv_close((uv_handle_t *) &c->load_timer, NULL);
    }
  }

  return;
}

static void
produce_heat (uv_timer_t *timer) {
  for (int i = 0; i < 10000000LLU; i++) sqrt(i); // steals ~46ms
}

static void
server_report_interval (uv_timer_t *timer) {
  udxperf_client_t *c = timer->data;
  udx_stream_t *stream = &c->stream;

  uint64_t now = uv_now(&loop);

  if (c->time_last_report) {
    print_interval(c, stream->bytes_rx - c->bytes_last_report, c->time_last_report, now);
  }

  c->time_last_report = now;
  c->bytes_last_report = stream->bytes_rx;
}

static void
server_handshake (udx_socket_t *sock, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (read_len != 4) {
    printf("bad handshake!\n");
    return;
  }

  server_id++;

  raddr = *(struct sockaddr_in *) from;

  client_id = *(uint32_t *) buf->base;

  uv_buf_t rbuf = uv_buf_init((char *) &server_id, 4);
  udx_socket_send(&handshake_req, sock, &rbuf, 1, from, NULL);

  udxperf_client_t *c = calloc(1, sizeof(udxperf_client_t));

  udx_stream_t *stream = &c->stream;

  udx_stream_init(&udx, stream, server_id, server_on_destroy, server_on_finalize);
  udx_stream_connect(stream, sock, client_id, (struct sockaddr *) from);

  udx_stream_read_start(stream, server_on_read);

  char lstring[40];
  uv_inet_ntop(AF_INET, &laddr.sin_addr, lstring, sizeof lstring);
  char rstring[40];
  uv_inet_ntop(AF_INET, &((struct sockaddr_in *) from)->sin_addr, rstring, sizeof rstring);

  uint16_t rport = htons(((struct sockaddr_in *) from)->sin_port);

  c->start_time = uv_now(&loop);

  if (interval_ms) {
    uv_timer_init(&loop, &c->report_timer);
    c->report_timer.data = c;
    c->time_last_report = uv_now(&loop);
    uv_timer_start(&c->report_timer, server_report_interval, interval_ms, interval_ms);
  }

  if (simulate_load) {
    printf("loop stress enabled\n");
    uv_timer_init(&loop, &c->load_timer);
    c->report_timer.data = c;
    uv_timer_start(&c->load_timer, produce_heat, 0, 1);
  }

  printf("[%3d] %s:%d connected with %s:%d\n", server_id, lstring, port, rstring, rport);
}

static void
server () {

  uv_ip4_addr("0.0.0.0", port, &laddr);
  int rc = udx_socket_bind(&sock, (struct sockaddr *) &laddr, 0);

  if (rc) {
    printf("bind: %s\n", uv_strerror(rc));
    exit(0);
  }

  udx_socket_recv_start(&sock, server_handshake);

  printf("Server listening on UDP port %d\n", port);
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  bytes_acked += req->bytes_acked;
  free(req);
}

static void
pump_writes (udx_stream_t *stream) {
  uv_buf_t chunk = uv_buf_init((char *) chunk_bytes, sizeof(chunk_bytes));

  while (1) {
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
    bytes_queued += chunk.len;
    int wm = udx_stream_write(req, stream, &chunk, 1, on_ack);
    assert(wm >= 0 && "udx_stream_write");
    if (wm == 0) break;
    /* if (wm == UV_EPIPE) {
      // assuming caused by premature close
      printf("pump_write aborted, stream closed\n");
      break;
    } */ // memleakd
    udx_stream_write_resume(stream, pump_writes);
  }
}

static void
write_end_cb (udx_stream_write_t *req, int status, int unordered) {
  printf("write end\n");
  free(req);
  exit(0);
}

static void
finish_timer_cb (uv_timer_t *timer) {
  udxperf_client_t *c = timer->data;
  c->end_time = uv_now(&loop);

  udx_stream_t *stream = &c->stream;

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  udx_stream_write_end(req, stream, NULL, 0, write_end_cb);

  print_interval(c, stream->bytes_tx, c->start_time, c->end_time);

  if (interval_ms) {
    uv_timer_stop(&c->report_timer);
    uv_close((uv_handle_t *) &c->report_timer, NULL);
  }
}

static void
client_report_interval (uv_timer_t *timer) {
  udxperf_client_t *c = timer->data;
  udx_stream_t *stream = &c->stream;

  uint64_t now = uv_now(&loop);

  if (c->time_last_report) {
    print_interval(c, stream->bytes_tx - c->bytes_last_report, c->time_last_report, now);
  }

  c->time_last_report = now;
  c->bytes_last_report = stream->bytes_tx;
}

static void
client_handshake_response (udx_socket_t *sock, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {

  udxperf_client_t *c = calloc(1, sizeof(udxperf_client_t));

  udx_stream_t *stream = &c->stream;

  if (read_len != 4) {
    printf("bad handshake response\n");
    free(c);
    return;
  }

  uint32_t *id = (uint32_t *) buf->base;

  server_id = *id;

  udx_socket_recv_stop(sock);

  assert(client_id == 1);

  udx_stream_init(&udx, stream, client_id, NULL, NULL);
  udx_stream_connect(stream, sock, server_id, (struct sockaddr *) &raddr);

  assert(time_ms && "timer must be set");

  c->start_time = uv_now(&loop);

  if (time_ms) {
    uv_timer_init(&loop, &client_complete_timer);
    client_complete_timer.data = c;
    uv_timer_start(&client_complete_timer, finish_timer_cb, time_ms, 0);
  }

  if (interval_ms) {
    uv_timer_init(&loop, &c->report_timer);
    c->report_timer.data = c;
    uv_timer_start(&c->report_timer, client_report_interval, interval_ms, interval_ms);
  }

  pump_writes(stream);
}

// send our clientid as pid, await a serverid
static void
client_handshake () {

  int rc = uv_ip4_addr(addr_string, port, &raddr);
  if (rc != 0) {
    printf("error parsing address '%s'\n", addr_string);
    exit(0);
  }

  // send our id

  uv_buf_t buf = uv_buf_init((char *) &client_id, 4);

  udx_socket_send(&handshake_req, &sock, &buf, 1, (struct sockaddr *) &raddr, NULL);
  udx_socket_recv_start(&sock, client_handshake_response);
  printf("Client connecting to %s:%d\n", addr_string, port);
}

static void
client () {
  // send data until
  struct sockaddr_in laddr;
  uv_ip4_addr("0.0.0.0", 0, &laddr);
  udx_socket_bind(&sock, (struct sockaddr *) &laddr, 0);

  client_handshake();
}

void
usage () {
  printf("udxperf [-s|-c host] [options]\n");
  exit(0);
}

typedef enum {
  SW_NONE,
  SW_C,
  SW_T,
  SW_I,
  SW_P,
  SW_L // simulate load on main thread
} switch_type_t;

int
main (int argc, char **argv) {
  switch_type_t sw = SW_NONE;

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && sw == SW_NONE) {
      switch (argv[i][1]) {
      case 'c':
        sw = SW_C;
        is_client = true;
        time_ms = 10000;
        break;
      case 't':
        sw = SW_T;
        break;
      case 'i':
        sw = SW_I;
        break;
      case 'p':
        sw = SW_P;
        break;
      case 's':
        is_server = true;
        time_ms = -1L;
        break;
      case 'x':
        extra_wanted = true;
        break;
      case 'l':
        sw = SW_L;
        simulate_load = true;
        break;
      default:
        printf("unrecognized switch '%s'\n", argv[i]);
        usage();
      }
    } else {
      switch (sw) {
      case SW_NONE:
        usage();
        break;
      case SW_C:
        sw = SW_NONE;
        addr_string = argv[i];
        break;
      case SW_T:
        sw = SW_NONE;
        time_ms = 1000 * atoi(argv[i]);
        if (time_ms == 0) {
          printf("invalid -t option: %s\n", argv[i]);
          return 0;
        }
        break;
      case SW_I:
        sw = SW_NONE;
        interval_ms = 1000 * atoi(argv[i]);
        if (interval_ms == 0) {
          printf("invalid -i option: %s\n", argv[i]);
          return 0;
        }
        break;
      case SW_P:
        sw = SW_NONE;
        port = atoi(argv[i]);
        if (port == 0) {
          printf("invalid -p option: %s\n", argv[i]);
          return 0;
        }
        break;
      case SW_L:
        sw = SW_NONE;
        break;
      }
    }
  }

  uv_loop_init(&loop);
  udx_init(&loop, &udx, NULL);
  udx_socket_init(&udx, &sock, NULL);

  if (is_server) {
    server();
  } else {
    client();
  }

  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}
