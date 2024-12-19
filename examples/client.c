#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <assert.h>
#include <time.h>

#include "../include/udx.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

/**
 * Don't merge!!
 * TODO: rename to jitter_client.c
 * $ ./jitter_client.c IP_ADDR JITTER_MS
 */
#define PLOT

#define LOG_INTERVAL 100

#define m2ns(ms) (ms * 1000000)
#define n2ms(ns) (ns / 1000000)
#define get_milliseconds(t) (uv_hrtime() / (t) 1000000)

#ifdef USE_DRAIN_THREAD
#define THREADS_ENABLED 1
#else
#define THREADS_ENABLED 0
#endif

FILE *plot_fd;

static uv_loop_t loop;
static udx_t udx;

static udx_socket_t sock;
static udx_socket_send_t req;

static udx_stream_t stream;
static struct sockaddr_in dest_addr;

static size_t bytes_recv = 0;
static size_t bytes_recv_round = 0;
static uint64_t started = 0;
static uint64_t round_start = 0;

static uint32_t client_id = 1;
static uint32_t server_id = 2;

static uv_timer_t timer;

static uv_timer_t jitter_timer;
static int jitter_interval = 0;  // max cpu block
static uint64_t jitter_time = 0;
static uint64_t packets_recv = 0;
static uint64_t packets_recv_round = 0;

static int round_i = 0;
static float saturation = 1; // 0..1

static void
on_report (uv_timer_t *handle) {
  double_t now = get_milliseconds(double);
  double_t delta = now - round_start;
  round_start = now;

  uint64_t prgm_delta =  now - started;

  double bps = 8 * (bytes_recv - bytes_recv_round) / (delta / 1000.);
  double avg_bps = 8 * bytes_recv / (prgm_delta / 1000.);

  uint64_t p = packets_recv - packets_recv_round;
  double pps = p / (delta / 1000.);

  int64_t k_drop = udx.packets_dropped_by_kernel; // A.K.A RXQ Overflow
  int64_t t_drop = 0;
  double q_load = 0;
  uint64_t n_packets_buffered = 0;
  uint64_t n_drains = 0;

#ifdef USE_DRAIN_THREAD
  t_drop = stream.socket->packets_dropped_by_worker || udx.packets_dropped_by_worker;
  q_load = udx__drainer_read_load(&udx, &n_packets_buffered, &n_drains);
#endif

  printf("%02i> received %0.2f Mbit/s (avg %0.3f), packets %zu (pps %0.1f, drop:%zi), jitter %zums (%0.0f%%) TrxQ-load: %0.2f%%\n",
      round_i, bps / pow(1024, 2), avg_bps / pow(1024, 2), p, pps, k_drop, n2ms(jitter_time), saturation * 100, q_load * 100);

  // if (prgm_delta > 5000) __builtin_debugtrap();

#ifdef PLOT
  fprintf(plot_fd, "%i %f %f %zu %f %zu %zi %zi %f %zu %zu %f %i %i %f %f\n",
      round_i, bps, avg_bps, p, pps, n2ms(jitter_time), k_drop, t_drop, q_load, n_packets_buffered, n_drains, prgm_delta / 1000., stream.seq, jitter_interval, saturation, delta);
#endif

  jitter_time = 0;
  bytes_recv_round = bytes_recv;
  packets_recv_round = packets_recv;

  round_i += 1;
  if (round_i > (60000 / LOG_INTERVAL)) exit(0); // unclean exit after 1 min

  // increase jitter time every 10 rounds
  if (jitter_interval && !(round_i % 30)) {
    saturation = !saturation;
  }
}

void uv_sleep_nano(uint64_t nsec);

/**
 * Simulating slow JS;
 * "Jitter" describes a function that saturates
 * the loop with blocking calls:
 *
 * function jitter (ms) {
 *   const start = Date.now()
 *   while (Date.now() - start < ms) {} // block loop
 *
 *   // release / let udx drain & flush
 *   setTimeout(() => jitter(ms), 0)
 * }
 *
 * jitter(5)
 */
static void
on_jitter (uv_timer_t *handle) {
  uint64_t start = uv_hrtime();
  // uint64_t prgm_delta =  n2ms(start) - started; // TODO: sweep saturation using prgm_delta

  // TODO: remodel simload,
  // target = block_ms, next = wait_ms
  // -- or --
  // target = (cpu * block_ms) / divisors, next = ((1-cpu) * wait_ms) / divisor

  uint64_t target = m2ns(jitter_interval * saturation);

  if (target) {
    uv_sleep_nano(target); // block main-loop
  }

  // try to align next run onto jitter_interval
  uint64_t next = MAX(jitter_interval - n2ms(target), 1);
  uv_timer_start(&jitter_timer, on_jitter, next, 0);

  jitter_time += uv_hrtime() - start;
}

static void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (started == 0) {
    started = round_start = get_milliseconds(double);
    uv_timer_init(&loop, &timer);
    uv_timer_start(&timer, on_report, LOG_INTERVAL, LOG_INTERVAL);

#ifdef PLOT
    // Warn opening file an doing blocking writes causes jitters.
    char target[INET_ADDRSTRLEN] = {0};

    inet_ntop(AF_INET, &(dest_addr.sin_addr), target, INET_ADDRSTRLEN);

    char datestr[32] = {0};
    char datestr_human[32] = {0};
    { // want to know when data was recorded
      time_t tstamp = time(NULL);
      struct tm *ltime = localtime(&tstamp);
      strftime(datestr, sizeof(datestr), "%y%m%d_%H%M", ltime);
      strftime(datestr_human, sizeof(datestr), "%y-%m-%d %H:%M:%S", ltime);
    }

    char fname[1024] = {0};
    sprintf(fname, "logs/run%s_dst%s-load%i_%s.txt",
        datestr, target, jitter_interval, THREADS_ENABLED ? "thread" : "nothread");

    plot_fd = fopen(fname, "w");
    if (plot_fd == NULL) {
      printf("Failed creating log file: %s\n", fname);
      perror("Error");
      exit(1);
    }
    printf("dumping data into %s\n", fname);

    fprintf(plot_fd, "# remote: %s, date: %s\n", target, datestr_human);
    fprintf(plot_fd, "# thread enabled: %i, jitter interval: %i ms, initial rwnd %i, log interval: %i\n", THREADS_ENABLED, jitter_interval, handle->recv_rwnd, LOG_INTERVAL);
    fprintf(plot_fd, "# recv_mbps avg_recv_mbps packets packets_per_second load_ms kernel_drop thread_drop thread_que_load n_packets_buffered n_drains clock stream_seq jitter_interval saturation delta\n");
#endif

    if (jitter_interval > 0) {
      printf("enabling jitter timer %i\n", jitter_interval);

      uv_timer_init(&loop, &jitter_timer);
      uv_timer_start(&jitter_timer, on_jitter, 4000, 0);
    }
  }

  if (read_len < 0) {
    printf("received %zu bytes in %" PRIu64 " ms\n", bytes_recv, get_milliseconds(uint64_t) - started);
    printf("stream is done!\n");
    exit(0);
  }

  bytes_recv += read_len;
  packets_recv++;
}

static void
on_send (udx_socket_send_t *r, int status) {
  udx_stream_init(&udx, &stream, client_id, NULL, NULL);
  udx_stream_connect(&stream, &sock, server_id, (struct sockaddr *) &dest_addr);
  udx_stream_read_start(&stream, on_read);
}

int
main (int argc, char **argv) {
  if (argc < 2) return 1;

  uv_ip4_addr(argv[1], 18081, &dest_addr);

  if (argc > 2) {
    jitter_interval = atoi(argv[2]);
  }

  uv_loop_init(&loop);

  udx_init(&loop, &udx, NULL);

  udx_socket_init(&udx, &sock, NULL);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", 18082, &addr);

  udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);

  client_id = (uint32_t) getpid();
  server_id = client_id + 1;

  uint32_t ids[2] = {client_id, server_id};

  uv_buf_t buf = uv_buf_init((char *) ids, 8);
  udx_socket_send(&req, &sock, &buf, 1, (struct sockaddr *) &dest_addr, on_send);

  uv_run(&loop, UV_RUN_DEFAULT);
  return 0;
}

void uv_sleep_nano(uint64_t nsec) { // lifted from uv/core.c
  struct timespec timeout;
  int rc;

  timeout.tv_sec = nsec / 1000000000;
  timeout.tv_nsec = (nsec % 1000000000);

  do
    rc = nanosleep(&timeout, &timeout);
  while (rc == -1 && errno == EINTR);

  assert(rc == 0);
}
