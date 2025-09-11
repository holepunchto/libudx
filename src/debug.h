#ifndef UDX_DEBUG_H
#define UDX_DEBUG_H

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

#include "queue.h"
#include <inttypes.h>

#ifdef DEBUG_STATS
#include "../include/udx.h"
#include <uv.h>

static uint64_t debug_start = 0;

static inline void
debug_print_cwnd_stats (udx_stream_t *stream) {
  if (!debug_start) debug_start = uv_hrtime() / 1000000;
  printf("%llu %u %u %u\n", (uv_hrtime() / 1000000) - debug_start, stream->cwnd, stream->cwnd_cnt, stream->srtt);
}
#else
static inline void
debug_print_cwnd_stats (udx_stream_t *stream) {
  (void) stream; // silence 'unused-parameter' warning
}
#endif

#ifdef UDX_DEBUG_THROUGHPUT

static inline void
open_throughput_file (udx_stream_t *stream) {
  if (!stream->throughput_fd) {
    char filename[100];
    snprintf(filename, sizeof(filename), "stream.%u.dat", stream->local_id);
    stream->throughput_fd = fopen(filename, "w");
  }
}

// prints a throughput record. tp = throughput
// <timestamp> tp <seq> <acked>
static inline void
debug_throughput (udx_stream_t *stream) {
  open_throughput_file(stream);

  fprintf(stream->throughput_fd, "%" PRIu64 " tp %u %u\n", uv_now(stream->udx->loop), stream->seq, stream->remote_acked);
}

static inline void
debug_throughput_init (udx_stream_t *stream) {
  stream->throughput_fd = NULL;
}

#include <stdarg.h>
// prints with format
// <timestamp> <formatted_string>
static inline void
debug_throughput_printf (udx_stream_t *stream, char *fmt, ...) {
  open_throughput_file(stream);

  va_list ap;
  va_start(ap, fmt);
  fprintf(stream->throughput_fd, "%" PRIu64 " ", uv_now(stream->udx->loop));
  vfprintf(stream->throughput_fd, fmt, ap);
  fprintf(stream->throughput_fd, "\n");
  va_end(ap);
}
#else
static inline void
debug_throughput (udx_stream_t *stream) {
  (void) stream;
}

static inline void
debug_throughput_printf (udx_stream_t *stream, char *fmt, ...) {
  (void) stream;
  (void) fmt;
}

static inline void
debug_throughput_init (udx_stream_t *stream) {
  (void) stream;
}

#endif

#define debug_printf(...) \
  do { \
    if (DEBUG) fprintf(stderr, __VA_ARGS__); \
  } while (0)

/*
static void
debug_print_outgoing (udx_stream_t *stream) {
  if (DEBUG) {
    for (uint32_t s = stream->remote_acked; s != stream->seq; s++) {
      udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&stream->outgoing, s);
      if (pkt == NULL) {
        debug_printf("-");
        continue;
      }

      if (pkt->lost) {
        debug_printf("R");
      } else {
        debug_printf("I");
      }
    }
    debug_printf("\n");

    uint64_t now = uv_hrtime() / 1000000;
    udx_queue_node_t *q;
    if (stream->inflight_queue.len > 0) {
      debug_printf("inflight q =");
      udx__queue_foreach(q, &stream->inflight_queue.node) {
        // for (udx_queue_node_t *q = stream->inflight_queue.node.next; q != &stream->inflight_queue.node; q = q->next) {
        udx_packet_t *pkt = udx__queue_data(q, udx_packet_t, queue);
        assert(!pkt->lost);
        debug_printf("%u %lums ", pkt->seq, now - pkt->time_sent);
      }
      debug_printf("\n");
    }

    if (stream->retransmit_queue.len > 0) {

      debug_printf("retransmit q =");
      udx__queue_foreach(q, &stream->retransmit_queue.node) {
        udx_packet_t *pkt = udx__queue_data(q, udx_packet_t, queue);
        assert(pkt->lost);
        debug_printf("%u %lums ", pkt->seq, now - pkt->time_sent);
      }
      debug_printf("\n");
    }
  }
}
*/

#endif // UDX_DEBUG_H
