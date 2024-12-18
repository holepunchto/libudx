#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <assert.h>

#include "../include/udx.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

/**
 * Don't merge!!
 * I've destroyed the simplicity of the original
 * server/client example while doing throughput tests.
 * If retained then should be moved/renamed otherwise will restore later.
 */
#define PLOT
#define LOG_INTERVAL 300

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

static uint64_t jitter_interval = 0;  // max cpu block
static uv_timer_t jitter_timer;
static uint64_t jitter_time = 0;
static uint64_t packets_recv = 0;
static uint64_t packets_recv_round = 0;

static int round_i = 0;
#define saturation 1

static uint64_t
get_milliseconds () {
  return uv_hrtime() / 1000000;
}

static void
on_uv_interval (uv_timer_t *handle) {
  uint64_t now = get_milliseconds();
  uint64_t delta = now - round_start;
  round_start = now;

  uint64_t prgm_delta =  now - started;

  float bps = 8 * (bytes_recv - bytes_recv_round) / (delta / 1000.);
  float avg_bps = 8 * bytes_recv / (prgm_delta / 1000.);

  uint64_t p = packets_recv - packets_recv_round;
  float pps = p / (delta / 1000.);

  // stupid.... this converts the integer into a bool, does not retain integer value! hence k-drop = 1
  // int64_t k_drop = sock.packets_dropped_by_kernel || udx.packets_dropped_by_kernel;
  int64_t k_drop = udx.packets_dropped_by_kernel;
  int64_t t_drop = 0;
  double q_load = 0;
  uint64_t n_packets_buffered = 0;
  uint64_t n_drains = 0;

#ifdef USE_DRAIN_THREAD
  t_drop = stream.socket->packets_dropped_by_worker || udx.packets_dropped_by_worker;
  q_load = udx__drainer_read_load(&udx, &n_packets_buffered, &n_drains);
#endif

  printf("%02i> received %0.2f Mbit/s (avg %0.3f), packets %zu (pps %01f),  jitter %zums, [Kd%zi] thread-load: %0.2f%%\n",
      round_i, bps / pow(1024, 2), avg_bps / pow(1024, 2), p, pps, jitter_time, k_drop, q_load * 100);

#ifdef PLOT
  fprintf(plot_fd, "%i %f %f %zu %f %zu %zi %zi %f %zu %zu %zu\n",
      round_i, bps, avg_bps, p, pps, jitter_time, k_drop, t_drop, q_load, n_packets_buffered, n_drains, now);
#endif

  jitter_time = 0;
  bytes_recv_round = bytes_recv;
  packets_recv_round = packets_recv;

  round_i += 1;
  if (round_i > (60000 / LOG_INTERVAL)) exit(0); // unclean exit after 1 min
}

static void
on_uv_interval_jitter (uv_timer_t *handle) {
  uint64_t start = uv_hrtime();
  uint64_t ns;
  // int i = 0;
  do {
    // sqrt(++i);
    uv_sleep(1); // blocking main-loop seems to be enough
    ns = uv_hrtime() - start;
  } while (ns < jitter_interval * saturation * 1000000);
  jitter_time += ns / 1000000;
}

static void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (started == 0) {
    started = round_start = get_milliseconds();
    uv_timer_init(&loop, &timer);
    uv_timer_start(&timer, on_uv_interval, LOG_INTERVAL, LOG_INTERVAL);

#ifdef PLOT
    char target[INET_ADDRSTRLEN] = {0};

    inet_ntop(AF_INET, &(dest_addr.sin_addr), target, INET_ADDRSTRLEN);
   
    char datestr[32] = {0};
    { // want to know when data was recorded
      time_t tstamp = time(NULL);
      struct tm *ltime = localtime(&tstamp);
      strftime(datestr, sizeof(datestr), "%y%m%d-%H%M%S", ltime);
    }

    char fname[1024] = {0};
    sprintf(fname, "logs/dest%s-load%zu_%s_logfreq%i_run%s.txt",
        target, jitter_interval, THREADS_ENABLED ? "thread" : "nothread", LOG_INTERVAL, datestr);

    plot_fd = fopen(fname, "w");
    if (plot_fd == NULL) {
      printf("Failed creating log file: %s\n", fname);
      perror("Error");
      exit(1);
    }
    printf("dumping data into %s\n", fname);
    fprintf(plot_fd, "# thread enabled: %i, jitter interval: %zu ms, initial rwnd %i, log interval: %i\n", THREADS_ENABLED, jitter_interval, handle->recv_rwnd, LOG_INTERVAL);
    fprintf(plot_fd, "# recv_mbps avg_recv_mbps packets packets_per_second load_ms kernel_drop thread_drop thread_que_load n_packets_buffered n_drains clock\n");
#endif

    if (jitter_interval > 0) {
      printf("enabling jitter timer %zu\n", jitter_interval);

      uv_timer_init(&loop, &jitter_timer);
      uv_timer_start(&jitter_timer, on_uv_interval_jitter, 4000, jitter_interval);
    }
  }

  if (read_len < 0) {
    printf("received %zu bytes in %" PRIu64 " ms\n", bytes_recv, get_milliseconds() - started);
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
