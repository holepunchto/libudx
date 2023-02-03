#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "../include/udx.h"

#include "cirbuf.h"
#include "debug.h"
#include "endian.h"
#include "fifo.h"
#include "io.h"

#define UDX_STREAM_ALL_DESTROYED (UDX_STREAM_DESTROYED | UDX_STREAM_DESTROYED_REMOTE)
#define UDX_STREAM_ALL_ENDED     (UDX_STREAM_ENDED | UDX_STREAM_ENDED_REMOTE)
#define UDX_STREAM_DEAD          (UDX_STREAM_ALL_DESTROYED | UDX_STREAM_DESTROYING | UDX_STREAM_CLOSED)

#define UDX_STREAM_SHOULD_READ (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD)
#define UDX_STREAM_READ        0

#define UDX_STREAM_SHOULD_END (UDX_STREAM_ENDING | UDX_STREAM_ENDED | UDX_STREAM_DEAD)
#define UDX_STREAM_END        UDX_STREAM_ENDING

#define UDX_STREAM_SHOULD_END_REMOTE (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD | UDX_STREAM_ENDING_REMOTE)
#define UDX_STREAM_END_REMOTE        UDX_STREAM_ENDING_REMOTE

#define UDX_PACKET_CALLBACK     (UDX_PACKET_STREAM_SEND | UDX_PACKET_STREAM_DESTROY | UDX_PACKET_SEND)
#define UDX_PACKET_FREE_ON_SEND (UDX_PACKET_STREAM_STATE | UDX_PACKET_STREAM_DESTROY)

#define UDX_HEADER_DATA_OR_END (UDX_HEADER_DATA | UDX_HEADER_END)

#define UDX_DEFAULT_TTL         64
#define UDX_DEFAULT_BUFFER_SIZE 212992

#define UDX_MAX_TRANSMITS 6

#define UDX_SLOW_RETRANSMIT 1
#define UDX_FAST_RETRANSMIT 2

#define UDX_CONG_C           400  // C=0.4 (inverse) in scaled 1000
#define UDX_CONG_C_SCALE     1e12 // ms/s ** 3 * c-scale
#define UDX_CONG_BETA        731  // b=0.3, BETA = 1-b, scaled 1024
#define UDX_CONG_BETA_UNIT   1024
#define UDX_CONG_BETA_SCALE  (8 * (UDX_CONG_BETA_UNIT + UDX_CONG_BETA) / 3 / (UDX_CONG_BETA_UNIT - UDX_CONG_BETA)) // 3B/(2-B) scaled 8
#define UDX_CONG_CUBE_FACTOR UDX_CONG_C_SCALE / UDX_CONG_C
#define UDX_CONG_INIT_CWND   3
#define UDX_CONG_MAX_CWND    65536

typedef struct {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int type;

  uv_buf_t buf;
} udx_pending_read_t;

static uint64_t
get_microseconds () {
  return uv_hrtime() / 1000;
}

static uint64_t
get_milliseconds () {
  return get_microseconds() / 1000;
}

static inline uint32_t
cubic_root (uint64_t a) {
  return (uint32_t) cbrt(a);
}

static uint32_t
max_uint32 (uint32_t a, uint32_t b) {
  return a < b ? b : a;
}

static int32_t
seq_diff (uint32_t a, uint32_t b) {
  return a - b;
}

static int
seq_compare (uint32_t a, uint32_t b) {
  int32_t d = seq_diff(a, b);
  return d < 0 ? -1 : d > 0 ? 1
                            : 0;
}

static uint32_t
seq_max (uint32_t a, uint32_t b) {
  return seq_compare(a, b) < 0 ? b : a;
}

static inline bool
is_addr_v4_mapped (const struct sockaddr *addr) {
  return addr->sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&(((struct sockaddr_in6 *) addr)->sin6_addr));
}

static inline void
addr_to_v4 (struct sockaddr_in6 *addr) {
  struct sockaddr_in in;
  memset(&in, 0, sizeof(in));

  in.sin_family = AF_INET;
  in.sin_port = addr->sin6_port;
#ifdef SIN6_LEN
  in.sin_len = sizeof(struct sockaddr_in);
#endif

  // Copy the IPv4 address from the last 4 bytes of the IPv6 address.
  memcpy(&(in.sin_addr), &(addr->sin6_addr.s6_addr[12]), 4);

  memcpy(addr, &in, sizeof(in));
}

static inline void
addr_to_v6 (struct sockaddr_in *addr) {
  struct sockaddr_in6 in;
  memset(&in, 0, sizeof(in));

  in.sin6_family = AF_INET6;
  in.sin6_port = addr->sin_port;
#ifdef SIN6_LEN
  in.sin6_len = sizeof(struct sockaddr_in6);
#endif

  in.sin6_addr.s6_addr[10] = 0xff;
  in.sin6_addr.s6_addr[11] = 0xff;

  // Copy the IPv4 address to the last 4 bytes of the IPv6 address.
  memcpy(&(in.sin6_addr.s6_addr[12]), &(addr->sin_addr), 4);

  memcpy(addr, &in, sizeof(in));
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events);

static void
ref_inc (udx_t *udx) {
  udx->refs++;

  if (udx->streams != NULL) return;

  udx->streams_len = 0;
  udx->streams_max_len = 16;
  udx->streams = malloc(udx->streams_max_len * sizeof(udx_stream_t *));

  udx__cirbuf_init(&(udx->streams_by_id), 16);
}

static void
ref_dec (udx_t *udx) {
  udx->refs--;

  if (udx->refs || udx->streams == NULL) return;

  free(udx->streams);
  udx->streams = NULL;
  udx->streams_max_len = 0;

  udx__cirbuf_destroy(&(udx->streams_by_id));
}

static void
trigger_socket_close (udx_socket_t *socket) {
  if (--socket->pending_closes) return;

  udx__fifo_destroy(&(socket->send_queue));

  if (socket->on_close != NULL) {
    socket->on_close(socket);
  }

  ref_dec(socket->udx);
}

static void
on_uv_close (uv_handle_t *handle) {
  trigger_socket_close((udx_socket_t *) handle->data);
}

static void
on_uv_interval (uv_timer_t *handle) {
  udx_check_timeouts((udx_t *) handle->data);
}

static int
udx_start_timer (udx_t *udx) {
  uv_timer_t *timer = &(udx->timer);

  memset(timer, 0, sizeof(uv_timer_t));

  int err = uv_timer_init(udx->loop, timer);
  assert(err == 0);

  err = uv_timer_start(timer, on_uv_interval, UDX_CLOCK_GRANULARITY_MS, UDX_CLOCK_GRANULARITY_MS);
  assert(err == 0);

  timer->data = udx;

  return err;
}

static void
on_udx_timer_close (uv_handle_t *handle) {
  udx_t *udx = (udx_t *) handle->data;
  udx_socket_t *socket = udx->timer_closed_by;

  if (udx->sockets > 0) { // re-open
    udx->timer_closed_by = NULL;
    udx_start_timer(udx);
  }

  trigger_socket_close(socket);
}

static void
close_handles (udx_socket_t *handle) {
  if (handle->status & UDX_SOCKET_CLOSING_HANDLES) return;
  handle->status |= UDX_SOCKET_CLOSING_HANDLES;

  if (handle->status & UDX_SOCKET_BOUND) {
    handle->pending_closes++;
    uv_poll_stop(&(handle->io_poll));
    uv_close((uv_handle_t *) &(handle->io_poll), on_uv_close);
  }

  handle->pending_closes += 2; // one below and one in trigger_socket_close
  uv_close((uv_handle_t *) &(handle->socket), on_uv_close);

  udx_t *udx = handle->udx;

  udx->sockets--;

  if (udx->sockets > 0 || udx->timer_closed_by) {
    trigger_socket_close(handle);
    return;
  }

  udx->timer_closed_by = handle;

  uv_timer_stop(&(udx->timer));
  uv_close((uv_handle_t *) &(udx->timer), on_udx_timer_close);
}

static int
update_poll (udx_socket_t *socket) {
  int events = UV_READABLE;

  if (socket->send_queue.len > 0) {
    events |= UV_WRITABLE;
  }

  if (events == socket->events) return 0;

  socket->events = events;
  return uv_poll_start(&(socket->io_poll), events, on_uv_poll);
}

// cubic congestion as per the paper https://www.cs.princeton.edu/courses/archive/fall16/cos561/papers/Cubic08.pdf

static void
increase_cwnd (udx_stream_t *stream, uint32_t cnt, uint32_t acked) {
  // smooth out applying the window increase using the counters...

  if (stream->cwnd_cnt >= cnt) {
    stream->cwnd_cnt = 0;
    stream->cwnd++;
  }

  stream->cwnd_cnt += acked;

  if (stream->cwnd_cnt >= cnt) {
    uint32_t delta = stream->cwnd_cnt / cnt;
    stream->cwnd_cnt -= delta * cnt;
    stream->cwnd += delta;
  }

  // clamp it
  if (stream->cwnd > UDX_CONG_MAX_CWND) {
    stream->cwnd = UDX_CONG_MAX_CWND;
  }
}

static void
reduce_cwnd (udx_stream_t *stream, int reset) {
  udx_cong_t *c = &(stream->cong);

  if (reset) {
    memset(c, 0, sizeof(udx_cong_t));
  } else {
    c->start_time = 0;
    c->last_max_cwnd = stream->cwnd < c->last_max_cwnd
                         ? (stream->cwnd * (UDX_CONG_BETA_UNIT + UDX_CONG_BETA)) / (2 * UDX_CONG_BETA_UNIT)
                         : stream->cwnd;
  }

  uint32_t upd = (stream->cwnd * UDX_CONG_BETA) / UDX_CONG_BETA_UNIT;

  stream->cwnd = stream->ssthresh = upd < 2 ? 2 : upd;
  stream->cwnd_cnt = 0; // TODO: dbl check that we should reset this

  debug_print_cwnd_stats(stream);
}

static void
update_congestion (udx_cong_t *c, uint32_t cwnd, uint32_t acked, uint64_t time) {
  c->ack_cnt += acked;

  // sanity check that didn't just enter this
  if (c->last_cwnd == cwnd && (time - c->last_time) <= 3) return;

  uint64_t delta;

  // make sure we don't over run this
  if (!c->start_time || time != c->last_time) {
    c->last_cwnd = cwnd;
    c->last_time = time;

    // we just entered this, init all state
    if (c->start_time == 0) {
      c->start_time = time;
      c->ack_cnt = acked;
      c->tcp_cwnd = cwnd;

      if (c->last_max_cwnd <= cwnd) {
        c->K = 0;
        c->origin_point = cwnd;
      } else {
        c->K = cubic_root(UDX_CONG_CUBE_FACTOR * (c->last_max_cwnd - cwnd));
        c->origin_point = c->last_max_cwnd;
      }
    }

    // time since epoch + delay
    uint32_t t = time - c->start_time + c->delay_min;

    // |t- K|
    uint64_t d = (t < c->K) ? (c->K - t) : (t - c->K);

    // C * (t - K)^3
    delta = UDX_CONG_C * d * d * d / UDX_CONG_C_SCALE;

    uint32_t target = t < c->K
                        ? c->origin_point - delta
                        : c->origin_point + delta;

    // the higher cnt, the slower it applies...
    c->cnt = target > cwnd
               ? cwnd / (target - cwnd)
               : 100 * cwnd; // ie very slowly
    ;

    // when we have no estimate of current bw make sure to not be too conservative
    if (c->last_cwnd == 0 && c->cnt > 20) {
      c->cnt = 20;
    }
  }

  // check tcp friendly mode

  delta = (UDX_CONG_BETA_SCALE * cwnd) >> 3;

  while (c->ack_cnt > delta) {
    c->ack_cnt -= delta;
    c->tcp_cwnd++;
  }

  if (c->tcp_cwnd > cwnd) {
    delta = c->tcp_cwnd - cwnd;
    uint32_t max_cnt = cwnd / delta;
    if (c->cnt > max_cnt) c->cnt = max_cnt;
  }

  // one update per 2 acks...
  if (c->cnt < 2) c->cnt = 2;
}

static void
unqueue_first_transmits (udx_stream_t *stream) {
  if (stream->seq_flushed == stream->remote_acked) return;

  for (uint32_t seq = stream->seq_flushed - 1; seq != stream->remote_acked; seq--) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&(stream->outgoing), seq);

    if (pkt == NULL || pkt->transmits != 0 || pkt->status != UDX_PACKET_SENDING) break;

    pkt->status = UDX_PACKET_WAITING;

    stream->pkts_waiting++;
    stream->pkts_inflight--;
    stream->inflight -= pkt->size;
    stream->seq_flushed--;

    udx__fifo_remove(&(stream->socket->send_queue), pkt, pkt->fifo_gc);
  }
}

static void
clear_incoming_packets (udx_stream_t *stream) {
  uint32_t seq = stream->ack;
  udx_cirbuf_t *inc = &(stream->incoming);

  while (stream->pkts_buffered) {
    udx_pending_read_t *pkt = (udx_pending_read_t *) udx__cirbuf_remove(inc, seq++);
    if (pkt == NULL) continue;

    stream->pkts_buffered--;
    free(pkt);
  }
}

static void
clear_outgoing_packets (udx_stream_t *stream) {
  udx_fifo_t *q = &(stream->socket->send_queue);

  // We should make sure all existing packets do not send, and notify the user that they failed
  for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(&(stream->outgoing), seq);

    if (pkt == NULL) continue;

    // Make sure to remove it from the fifo, if it was added
    if (pkt->status == UDX_PACKET_SENDING) {
      udx__fifo_remove(q, pkt, pkt->fifo_gc);
    }

    udx_stream_write_t *w = (udx_stream_write_t *) pkt->ctx;

    if (--(w->packets) == 0) {
      if (w->on_ack != NULL) {
        w->on_ack(w, UV_ECANCELED, 0);
      }
    }

    free(pkt);
  }

  // also clear pending unordered packets, and the destroy packet if waiting
  udx_fifo_t *u = &(stream->unordered);

  while (u->len > 0) {
    udx_packet_t *pkt = udx__fifo_shift(u);
    if (pkt == NULL) continue;

    udx__fifo_remove(q, pkt, pkt->fifo_gc);

    if (pkt->type == UDX_PACKET_STREAM_SEND) {
      udx_stream_send_t *req = pkt->ctx;

      if (req->on_send != NULL) {
        req->on_send(req, UV_ECANCELED);
      }
    }

    if (pkt->type & UDX_PACKET_FREE_ON_SEND) {
      free(pkt);
    }
  }
}

static void
init_stream_packet (udx_packet_t *pkt, int type, udx_stream_t *stream, const uv_buf_t *buf) {
  uint8_t *b = (uint8_t *) &(pkt->header);

  // 8 bit magic byte + 8 bit version + 8 bit type + 8 bit extensions
  *(b++) = UDX_MAGIC_BYTE;
  *(b++) = UDX_VERSION;
  *(b++) = (uint8_t) type;
  *(b++) = 0; // data offset

  uint32_t *i = (uint32_t *) b;

  // 32 bit (le) remote id
  *(i++) = udx__swap_uint32_if_be(stream->remote_id);
  // 32 bit (le) recv window
  *(i++) = 0xffffffff; // hardcode max recv window
  // 32 bit (le) seq
  *(i++) = udx__swap_uint32_if_be(stream->seq);
  // 32 bit (le) ack
  *(i++) = udx__swap_uint32_if_be(stream->ack);

  pkt->seq = stream->seq;
  pkt->is_retransmit = 0;
  pkt->transmits = 0;
  pkt->size = (uint16_t) (UDX_HEADER_SIZE + buf->len);
  pkt->dest = stream->remote_addr;
  pkt->dest_len = stream->remote_addr_len;

  pkt->bufs_len = 2;

  pkt->bufs[0] = uv_buf_init((char *) &(pkt->header), UDX_HEADER_SIZE);
  pkt->bufs[1] = *buf;
}

static int
send_state_packet (udx_stream_t *stream) {
  if ((stream->status & UDX_STREAM_CONNECTED) == 0) return 0;

  stream->deferred_ack = 0;

  uint32_t *sacks = NULL;
  uint32_t start = 0;
  uint32_t end = 0;

  udx_packet_t *pkt = NULL;

  void *payload = NULL;
  size_t payload_len = 0;

  int ooo = stream->out_of_order;

  // 65536 is just a sanity check here in terms of how much max work we wanna do, could prob be smarter
  // only valid if ooo is very large
  for (uint32_t i = 0; i < 65536 && ooo > 0 && payload_len < 400; i++) {
    uint32_t seq = stream->ack + 1 + i;
    if (udx__cirbuf_get(&(stream->incoming), seq) == NULL) continue;

    ooo--;

    if (sacks == NULL) {
      pkt = malloc(sizeof(udx_packet_t) + 1024);
      payload = (((char *) pkt) + sizeof(udx_packet_t));
      sacks = (uint32_t *) payload;
      start = seq;
      end = seq + 1;
    } else if (seq == end) {
      end++;
    } else {
      *(sacks++) = udx__swap_uint32_if_be(start);
      *(sacks++) = udx__swap_uint32_if_be(end);
      start = seq;
      end = seq + 1;
      payload_len += 8;
    }
  }

  if (start != end) {
    *(sacks++) = udx__swap_uint32_if_be(start);
    *(sacks++) = udx__swap_uint32_if_be(end);
    payload_len += 8;
  }

  if (pkt == NULL) pkt = malloc(sizeof(udx_packet_t));

  uv_buf_t buf = uv_buf_init(payload, payload_len);

  init_stream_packet(pkt, payload ? UDX_HEADER_SACK : 0, stream, &buf);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_STATE;
  pkt->ttl = 0;

  udx__fifo_push(&(stream->socket->send_queue), pkt);
  return update_poll(stream->socket);
}

static int
send_data_packet (udx_stream_t *stream, udx_packet_t *pkt) {
  if (stream->inflight + pkt->size > stream->cwnd * UDX_MSS) {
    return 0;
  }

  assert(pkt->status == UDX_PACKET_WAITING);

  pkt->status = UDX_PACKET_SENDING;

  stream->pkts_waiting--;
  stream->pkts_inflight++;
  stream->inflight += pkt->size;

  if (pkt->is_retransmit) {
    pkt->is_retransmit = 0;
    stream->retransmits_waiting--;
    if (pkt->transmits == 1) stream->retransmitting++;
  } else if (seq_compare(stream->seq_flushed, pkt->seq) <= 0) {
    stream->seq_flushed = pkt->seq + 1;
  }

  pkt->fifo_gc = udx__fifo_push(&(stream->socket->send_queue), pkt);

  int err = update_poll(stream->socket);
  return err < 0 ? err : 1;
}

static int
flush_waiting_packets (udx_stream_t *stream) {
  const uint32_t was_waiting = stream->pkts_waiting;
  uint32_t seq = stream->retransmits_waiting ? stream->remote_acked : (stream->seq - stream->pkts_waiting);

  int sent = 0;

  while (seq != stream->seq && stream->pkts_waiting > 0) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&(stream->outgoing), seq++);

    if (pkt == NULL || pkt->status != UDX_PACKET_WAITING) continue;

    sent = send_data_packet(stream, pkt);
    if (sent <= 0) break;
  }

  // TODO: retransmits are counted in pkts_waiting, but we (prob) should not count them
  // towards to drain loop - investigate that.
  if (was_waiting > 0 && stream->pkts_waiting == 0 && stream->on_drain != NULL) {
    stream->on_drain(stream);
  }

  if (sent < 0) return sent;
  return 0;
}

static int
close_maybe (udx_stream_t *stream, int err) {
  // if BOTH closed or ANY destroyed.
  if ((stream->status & UDX_STREAM_ALL_ENDED) != UDX_STREAM_ALL_ENDED && !(stream->status & UDX_STREAM_ALL_DESTROYED)) return 0;
  // if we already destroyed, bail.
  if (stream->status & UDX_STREAM_CLOSED) return 0;

  stream->status |= UDX_STREAM_CLOSED;

  udx_t *udx = stream->udx;

  // Remove from the set, by array[i] = array.pop()
  udx_stream_t *other = udx->streams[--(udx->streams_len)];
  udx->streams[stream->set_id] = other;
  other->set_id = stream->set_id;

  udx__cirbuf_remove(&(udx->streams_by_id), stream->local_id);
  clear_outgoing_packets(stream);
  clear_incoming_packets(stream);

  // TODO: move the instance to a TIME_WAIT state, so we can handle retransmits

  if (stream->status & UDX_STREAM_READING) {
    udx_stream_read_stop(stream);
  }

  udx_stream_t *relay = stream->relay_to;

  if (relay) {
    udx__cirbuf_remove(&(relay->relaying_streams), stream->local_id);
  }

  udx_cirbuf_t relaying = stream->relaying_streams;

  for (uint32_t i = 0; i < relaying.size; i++) {
    udx_stream_t *stream = (udx_stream_t *) relaying.values[i];

    if (stream) stream->relay_to = NULL;
  }

  udx__cirbuf_destroy(&(stream->relaying_streams));
  udx__cirbuf_destroy(&(stream->incoming));
  udx__cirbuf_destroy(&(stream->outgoing));
  udx__fifo_destroy(&(stream->unordered));

  if (stream->on_close != NULL) {
    stream->on_close(stream, err);
  }

  ref_dec(udx);

  return 1;
}

// rack recovery implemented using https://datatracker.ietf.org/doc/rfc8985/

static inline bool
rack_sent_after (uint64_t t1, uint32_t seq1, uint64_t t2, uint32_t seq2) {
  return t1 > t2 || (t1 == t2 && seq_compare(seq2, seq1) < 0);
}

static inline uint32_t
rack_update_reo_wnd (udx_stream_t *stream) {
  // TODO: add the DSACK logic also (skipped for now as we didnt impl and only recommended...)

  if (!stream->reordering_seen) {
    if (stream->recovery) return 0;
    if (stream->sacks >= 3) return 0;
  }

  uint32_t r = stream->rack_rtt_min / 4;
  return r < stream->srtt ? r : stream->srtt;
}

static void
rack_detect_loss (udx_stream_t *stream) {
  uint64_t timeout = 0;
  uint32_t reo_wnd = rack_update_reo_wnd(stream);
  uint64_t now = 0;

  int resending = 0;

  for (uint32_t seq = stream->remote_acked; seq != stream->seq_flushed; seq++) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&(stream->outgoing), seq);

    if (pkt == NULL || pkt->transmits == 0 || pkt->status != UDX_PACKET_INFLIGHT) continue;

    if (!rack_sent_after(stream->rack_time_sent, stream->rack_next_seq, pkt->time_sent, pkt->seq + 1)) {
      // if no retransmitting packets this is oldest package and hence no need to skip anything else
      if (stream->retransmitting) continue;
      else break;
    }

    if (!now) now = get_milliseconds();

    int64_t remaining = pkt->time_sent + stream->rack_rtt + reo_wnd - now;

    if (remaining <= 0) {
      pkt->status = UDX_PACKET_WAITING;
      pkt->is_retransmit = UDX_FAST_RETRANSMIT;

      stream->inflight -= pkt->size;
      stream->pkts_waiting++;
      stream->pkts_inflight--;
      stream->retransmits_waiting++;

      resending++;
    } else if ((uint64_t) remaining > timeout) {
      timeout = remaining;
    }
  }

  if (resending) {
    if (stream->recovery == 0) {
      // easy win is to clear packets that are in the queue - they def wont help if sent.
      unqueue_first_transmits(stream);

      // recover until the full window is packa
      stream->recovery = seq_diff(stream->seq_flushed, stream->remote_acked);

      reduce_cwnd(stream, false);

      debug_printf("fast recovery: started, recovery=%u inflight=%zu cwnd=%u acked=%u, seq=%u srtt=%u\n", stream->recovery, stream->inflight, stream->cwnd, stream->remote_acked, stream->seq_flushed, stream->srtt);
    }

    flush_waiting_packets(stream);
  }

  stream->rack_timeout = timeout;
}

static void
ack_update (udx_stream_t *stream, uint32_t acked, bool is_limited) {
  uint64_t time = get_milliseconds();

  // also reset rto, since things are moving forward...
  stream->rto_timeout = time + stream->rto;

  udx_cong_t *c = &(stream->cong);

  // If we are application limited, just reset the epic and return...
  // The delay_min check here, was added due to massive latency increase (ie multiple seconds) due to router buffering
  // Perhaps research other approaches for this, but since delay_min is adjusted based on congestion this seems OK but
  // but surely better ways exists for this
  if (is_limited || stream->recovery || (c->delay_min > 0 && stream->srtt > c->delay_min * 4)) {
    c->start_time = 0;
    return;
  }

  if (c->delay_min == 0 || c->delay_min > stream->srtt) {
    c->delay_min = stream->srtt;
  }

  if (stream->cwnd < stream->ssthresh) {
    stream->cwnd += acked;
    if (stream->cwnd > stream->ssthresh) stream->cwnd = stream->ssthresh;
  } else {
    update_congestion(c, stream->cwnd, acked, time);
    increase_cwnd(stream, c->cnt, acked);
  }

  debug_print_cwnd_stats(stream);
}

static int
ack_packet (udx_stream_t *stream, uint32_t seq, int sack) {
  udx_cirbuf_t *out = &(stream->outgoing);
  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(out, seq);

  if (pkt == NULL) {
    if (!sack) stream->sacks--; // packet not here, was sacked before
    return 0;
  }

  if (sack) {
    stream->sacks++;
  }

  if (pkt->is_retransmit) {
    pkt->is_retransmit = 0;
    stream->retransmits_waiting--;
    stream->pkts_waiting--;
  } else if ((pkt->status == UDX_PACKET_INFLIGHT && pkt->transmits > 1) || (pkt->status == UDX_PACKET_SENDING && pkt->transmits > 0)) {
    stream->retransmitting--;
  }

  if (pkt->status == UDX_PACKET_INFLIGHT || pkt->status == UDX_PACKET_SENDING) {
    stream->pkts_inflight--;
    stream->inflight -= pkt->size;
  }

  const uint64_t time = get_milliseconds();
  const uint32_t rtt = (uint32_t) (time - pkt->time_sent);
  const uint32_t next = seq + 1;

  if (seq_compare(stream->rack_fack, next) < 0) {
    stream->rack_fack = next;
  } else if (seq_compare(next, stream->rack_fack) < 0 && pkt->transmits == 1) {
    stream->reordering_seen = true;
  }

  if (pkt->status == UDX_PACKET_INFLIGHT && pkt->transmits == 1) {
    if (stream->rack_rtt_min == 0 || stream->rack_rtt_min > rtt) {
      stream->rack_rtt_min = rtt;
    }

    // First round trip time sample
    if (stream->srtt == 0) {
      stream->srtt = rtt;
      stream->rttvar = rtt / 2;
    } else {
      const uint32_t delta = rtt < stream->srtt ? stream->srtt - rtt : rtt - stream->srtt;
      // RTTVAR <- (1 - beta) * RTTVAR + beta * |SRTT - R'| where beta is 1/4
      stream->rttvar = (3 * stream->rttvar + delta) / 4;

      // SRTT <- (1 - alpha) * SRTT + alpha * R' where alpha is 1/8
      stream->srtt = (7 * stream->srtt + rtt) / 8;
    }

    // RTO <- SRTT + max (G, K*RTTVAR) where K is 4 maxed with 1s
    stream->rto = max_uint32(stream->srtt + max_uint32(UDX_CLOCK_GRANULARITY_MS, 4 * stream->rttvar), 1000);
  }

  if (pkt->status == UDX_PACKET_INFLIGHT && (pkt->transmits == 1 || (rtt >= stream->rack_rtt_min && stream->rack_rtt_min > 0))) {
    stream->rack_rtt = rtt;

    if (rack_sent_after(pkt->time_sent, next, stream->rack_time_sent, stream->rack_next_seq)) {
      stream->rack_time_sent = pkt->time_sent;
      stream->rack_next_seq = next;
    }
  }

  // If this packet was queued for sending we need to remove it from the queue.
  if (pkt->status == UDX_PACKET_SENDING) {
    udx__fifo_remove(&(stream->socket->send_queue), pkt, pkt->fifo_gc);
  }

  udx_stream_write_t *w = (udx_stream_write_t *) pkt->ctx;

  free(pkt);

  if (--(w->packets) != 0) return 1;

  if (w->on_ack != NULL) {
    w->on_ack(w, 0, sack);
  }

  if (stream->status & UDX_STREAM_DEAD) return 2;

  // TODO: the end condition needs work here to be more "stateless"
  // ie if the remote has acked all our writes, then instead of waiting for retransmits, we should
  // clear those and mark as local ended NOW.
  if ((stream->status & UDX_STREAM_SHOULD_END) == UDX_STREAM_END && stream->pkts_waiting == 0 && stream->pkts_inflight == 0) {
    stream->status |= UDX_STREAM_ENDED;
    return 2;
  }

  return 1;
}

static uint32_t
process_sacks (udx_stream_t *stream, char *buf, size_t buf_len) {
  uint32_t n = 0;
  uint32_t *sacks = (uint32_t *) buf;

  for (size_t i = 0; i + 8 <= buf_len; i += 8) {
    uint32_t start = udx__swap_uint32_if_be(*(sacks++));
    uint32_t end = udx__swap_uint32_if_be(*(sacks++));
    int32_t len = seq_diff(end, start);

    for (int32_t j = 0; j < len; j++) {
      int a = ack_packet(stream, start + j, 1);
      if (a == 2) return 0; // ended
      if (a == 1) {
        n++;
      }
    }
  }

  return n;
}

static void
process_data_packet (udx_stream_t *stream, int type, uint32_t seq, char *data, ssize_t data_len) {
  if (seq == stream->ack && type == UDX_HEADER_DATA) {
    // Fast path - next in line, no need to memcpy it, stack allocate the struct and call on_read...
    stream->ack++;

    if (stream->on_read != NULL) {
      uv_buf_t buf = uv_buf_init(data, data_len);
      stream->on_read(stream, data_len, &buf);
    }
    return;
  }

  stream->out_of_order++;

  // Slow path, packet out of order.
  // Copy over incoming buffer as we do not own it (stack allocated upstream)
  char *ptr = malloc(sizeof(udx_pending_read_t) + data_len);

  udx_pending_read_t *pkt = (udx_pending_read_t *) ptr;
  char *cpy = ptr + sizeof(udx_pending_read_t);

  memcpy(cpy, data, data_len);

  pkt->type = type;
  pkt->seq = seq;
  pkt->buf.base = cpy;
  pkt->buf.len = data_len;

  stream->pkts_buffered++;
  udx__cirbuf_set(&(stream->incoming), (udx_cirbuf_val_t *) pkt);
}

static int
relay_packet (udx_stream_t *stream, char *buf, ssize_t buf_len, int type, uint32_t seq, uint32_t ack) {
  stream->seq = seq_max(stream->seq, seq);

  udx_stream_t *relay = stream->relay_to;

  if (relay->socket != NULL) {
    uint32_t *h = (uint32_t *) buf;
    h[1] = udx__swap_uint32_if_be(relay->remote_id);

    uv_buf_t b = uv_buf_init(buf, buf_len);

    int err = udx__sendmsg(relay->socket, &b, 1, (struct sockaddr *) &relay->remote_addr, relay->remote_addr_len);

    if (err == EAGAIN) {
      b.base += UDX_HEADER_SIZE;
      b.len -= UDX_HEADER_SIZE;

      udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

      init_stream_packet(pkt, type, relay, &b);

      h = (uint32_t *) &(pkt->header);
      h[3] = udx__swap_uint32_if_be(seq);
      h[4] = udx__swap_uint32_if_be(ack);

      pkt->status = UDX_PACKET_SENDING;
      pkt->type = UDX_PACKET_STREAM_RELAY;
      pkt->seq = seq;

      udx__fifo_push(&(relay->socket->send_queue), pkt);

      update_poll(relay->socket);
    }
  }

  if (type & UDX_HEADER_DESTROY) {
    stream->status |= UDX_STREAM_DESTROYED_REMOTE;
    close_maybe(stream, UV_ECONNRESET);
  }

  return 1;
}

static int
process_packet (udx_socket_t *socket, char *buf, ssize_t buf_len, struct sockaddr *addr) {
  if (buf_len < UDX_HEADER_SIZE) return 0;

  uint8_t *b = (uint8_t *) buf;

  if ((*(b++) != UDX_MAGIC_BYTE) || (*(b++) != UDX_VERSION)) return 0;

  int type = (int) *(b++);
  uint8_t data_offset = *(b++);

  uint32_t *i = (uint32_t *) b;

  uint32_t local_id = udx__swap_uint32_if_be(*(i++));
  /* recv_win */ udx__swap_uint32_if_be(*(i++));
  uint32_t seq = udx__swap_uint32_if_be(*(i++));
  uint32_t ack = udx__swap_uint32_if_be(*i);

  udx_stream_t *stream = (udx_stream_t *) udx__cirbuf_get(socket->streams_by_id, local_id);

  if (stream == NULL || stream->status & UDX_STREAM_DEAD) return 0;

  // We expect this to be a stream packet from now on
  if (!(stream->status & UDX_STREAM_CONNECTED) && stream->on_firewall != NULL) {
    if (is_addr_v4_mapped((struct sockaddr *) addr)) {
      addr_to_v4((struct sockaddr_in6 *) addr);
    }

    if (stream->on_firewall(stream, socket, addr)) return 1;
  }

  if (stream->relay_to) return relay_packet(stream, buf, buf_len, type, seq, ack);

  buf += UDX_HEADER_SIZE;
  buf_len -= UDX_HEADER_SIZE;

  size_t header_len = (data_offset > 0 && data_offset < buf_len) ? data_offset : buf_len;
  bool is_limited = stream->inflight + 2 * UDX_MSS < stream->cwnd * UDX_MSS;

  bool sacked = (type & UDX_HEADER_SACK) ? process_sacks(stream, buf, header_len) > 0 : false;

  // Done with header processing now.
  // For future compat, make sure we are now pointing at the actual data using the data_offset
  if (data_offset) {
    if (data_offset > buf_len) return 1;
    buf += data_offset;
    buf_len -= data_offset;
  }

  udx_cirbuf_t *inc = &(stream->incoming);

  // For all stream packets, ensure that they are causally newer (or same)
  if (seq_compare(stream->ack, seq) <= 0) {
    if (type & UDX_HEADER_DATA_OR_END && udx__cirbuf_get(inc, seq) == NULL && (stream->status & UDX_STREAM_SHOULD_READ) == UDX_STREAM_READ) {
      process_data_packet(stream, type, seq, buf, buf_len);
    }

    if (type & UDX_HEADER_END) {
      stream->status |= UDX_STREAM_ENDING_REMOTE;
      stream->remote_ended = seq;
    }

    if (type & UDX_HEADER_DESTROY) {
      stream->status |= UDX_STREAM_DESTROYED_REMOTE;
      close_maybe(stream, UV_ECONNRESET);
      return 1;
    }
  }

  if (type & UDX_HEADER_MESSAGE) {
    if (stream->on_recv != NULL) {
      uv_buf_t b = uv_buf_init(buf, buf_len);
      stream->on_recv(stream, buf_len, &b);
    }
  }

  // process the (out of order) read queue
  while ((stream->status & UDX_STREAM_SHOULD_READ) == UDX_STREAM_READ) {
    udx_pending_read_t *pkt = (udx_pending_read_t *) udx__cirbuf_remove(inc, stream->ack);
    if (pkt == NULL) break;

    stream->out_of_order--;
    stream->pkts_buffered--;
    stream->ack++;

    if ((pkt->type & UDX_HEADER_DATA) && stream->on_read != NULL) {
      stream->on_read(stream, pkt->buf.len, &(pkt->buf));
    }

    free(pkt);
  }

  // Check if the ack is oob.
  if (seq_compare(stream->seq, ack) < 0) {
    return 1;
  }

  int32_t len = seq_diff(ack, stream->remote_acked);

  if (len > 0) {
    ack_update(stream, len, is_limited);
    rack_detect_loss(stream);
  } else if (sacked) {
    rack_detect_loss(stream);
  }

  for (int32_t j = 0; j < len; j++) {
    if (stream->recovery > 0 && --(stream->recovery) == 0) {
      // The end of fast recovery, adjust according to the spec (unsure if we need this as we do not modify cwnd during recovery but oh well...)
      if (stream->ssthresh < stream->cwnd) stream->cwnd = stream->ssthresh;

      debug_printf("fast recovery: ended, inflight=%zu, cwnd=%u, acked=%u, seq=%u\n", stream->inflight, stream->cwnd, stream->remote_acked + 1, stream->seq_flushed);
    }

    int a = ack_packet(stream, stream->remote_acked++, 0);

    if (a == 0 || a == 1) continue;
    if (a == 2) { // it ended, so ack that and trigger close
      // TODO: make this work as well, if the ack packet is lost, ie
      // have some internal (capped) queue of "gracefully closed" streams (TIME_WAIT)
      send_state_packet(stream);
      close_maybe(stream, 0);
    }
    return 1;
  }

  // if data pkt, send an ack - use deferred acks as well...
  if (type & UDX_HEADER_DATA_OR_END) {
    // This needs some work, ie, read some modern specs...
    // if (stream->out_of_order) {
    //   send_state_packet(stream);
    // } else if (stream->deferred_ack == 0) {
    //   stream->deferred_ack = 1; // lets try with a clock single tick first
    // }
    send_state_packet(stream);
  }

  if ((stream->status & UDX_STREAM_SHOULD_END_REMOTE) == UDX_STREAM_END_REMOTE && seq_compare(stream->remote_ended, stream->ack) <= 0) {
    stream->status |= UDX_STREAM_ENDED_REMOTE;
    if (stream->on_read != NULL) {
      uv_buf_t b = uv_buf_init(NULL, 0);
      stream->on_read(stream, UV_EOF, &b);
    }
    if (close_maybe(stream, 0)) return 1;
  }

  if (stream->pkts_waiting > 0) {
    udx_stream_check_timeouts(stream);
  }

  return 1;
}

static void
remove_next (udx_fifo_t *f) {
  while (f->len > 0 && udx__fifo_shift(f) == NULL)
    ;
}

static void
trigger_send_callback (udx_socket_t *socket, udx_packet_t *pkt) {
  if (pkt->type == UDX_PACKET_SEND) {
    udx_socket_send_t *req = pkt->ctx;

    if (req->on_send != NULL) {
      req->on_send(req, 0);
    }
    return;
  }

  if (pkt->type == UDX_PACKET_STREAM_SEND) {
    udx_stream_send_t *req = pkt->ctx;

    udx_stream_t *stream = req->handle;
    remove_next(&(stream->unordered));

    if (req->on_send != NULL) {
      req->on_send(req, 0);
    }
    return;
  }

  if (pkt->type == UDX_PACKET_STREAM_DESTROY) {
    udx_stream_t *stream = pkt->ctx;
    remove_next(&(stream->unordered));

    stream->status |= UDX_STREAM_DESTROYED;
    close_maybe(stream, 0);
    return;
  }
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events) {
  udx_socket_t *socket = handle->data;
  ssize_t size;

  if (events & UV_READABLE) {
    struct sockaddr_storage addr;
    int addr_len = sizeof(addr);
    uv_buf_t buf;

    memset(&addr, 0, addr_len);

    char b[2048];
    buf.base = (char *) &b;
    buf.len = 2048;

    while ((size = udx__recvmsg(socket, &buf, (struct sockaddr *) &addr, addr_len)) >= 0) {
      if (!process_packet(socket, b, size, (struct sockaddr *) &addr) && socket->on_recv != NULL) {
        buf.len = size;

        if (is_addr_v4_mapped((struct sockaddr *) &addr)) {
          addr_to_v4((struct sockaddr_in6 *) &addr);
        }

        socket->on_recv(socket, size, &buf, (struct sockaddr *) &addr);
      }

      buf.len = 2048;
    }
  }

  while (events & UV_WRITABLE && socket->send_queue.len > 0) {
    udx_packet_t *pkt = (udx_packet_t *) udx__fifo_shift(&(socket->send_queue));
    if (pkt == NULL) continue;

    bool adjust_ttl = pkt->ttl > 0 && socket->ttl != pkt->ttl;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, pkt->ttl);

    if (socket->family == 6 && pkt->dest.ss_family == AF_INET) {
      addr_to_v6((struct sockaddr_in *) &(pkt->dest));
      pkt->dest_len = sizeof(struct sockaddr_in6);
    }

    size = udx__sendmsg(socket, pkt->bufs, pkt->bufs_len, (struct sockaddr *) &(pkt->dest), pkt->dest_len);

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, socket->ttl);

    if (size == UV_EAGAIN) {
      udx__fifo_undo(&(socket->send_queue));
      break;
    }

    assert(pkt->status == UDX_PACKET_SENDING);
    pkt->status = UDX_PACKET_INFLIGHT;
    pkt->transmits++;
    pkt->time_sent = uv_hrtime() / 1e6;

    int type = pkt->type;

    if (type & UDX_PACKET_CALLBACK) {
      trigger_send_callback(socket, pkt);
      // TODO: watch for re-entry here!
    }

    if (type & UDX_PACKET_FREE_ON_SEND) {
      free(pkt);
    }

    // queue another write, might be able to do this smarter...
    if (socket->send_queue.len > 0) continue;

    // if the socket is under closure, we need to trigger shutdown now since no important writes are pending
    if (socket->status & UDX_SOCKET_CLOSING) {
      close_handles(socket);
      return;
    }
  }

  // update the poll if the socket is still active.
  if (uv_is_active((uv_handle_t *) &socket->io_poll)) {
    update_poll(socket);
  }
}

int
udx_init (uv_loop_t *loop, udx_t *handle) {
  handle->refs = 0;
  handle->sockets = 0;
  handle->timer_closed_by = NULL;

  handle->streams_len = 0;
  handle->streams_max_len = 0;
  handle->streams = NULL;

  handle->loop = loop;

  return 0;
}

int
udx_check_timeouts (udx_t *handle) {
  for (uint32_t i = 0; i < handle->streams_len; i++) {
    int err = udx_stream_check_timeouts(handle->streams[i]);
    if (err < 0) return err;
    if (err == 1) i--; // stream was closed, the index again
  }
  return 0;
}

int
udx_socket_init (udx_t *udx, udx_socket_t *handle) {
  ref_inc(udx);

  handle->family = 0;
  handle->status = 0;
  handle->events = 0;
  handle->pending_closes = 0;
  handle->ttl = UDX_DEFAULT_TTL;

  handle->udx = udx;
  handle->streams_by_id = &(udx->streams_by_id);

  udx->sockets++;

  // If first open...
  if (udx->sockets == 1 && udx->timer_closed_by == NULL) {
    // Asserting all the errors here as it massively simplifies error handling.
    // In practice these will never fail.
    udx_start_timer(udx);
  }

  handle->on_recv = NULL;
  handle->on_close = NULL;

  udx__fifo_init(&(handle->send_queue), 16);

  uv_udp_t *socket = &(handle->socket);

  // Asserting all the errors here as it massively simplifies error handling.
  // In practice these will never fail.

  int err = uv_udp_init(udx->loop, socket);
  assert(err == 0);

  socket->data = handle;

  return err;
}

int
udx_socket_get_send_buffer_size (udx_socket_t *handle, int *value) {
  *value = 0;
  return uv_send_buffer_size((uv_handle_t *) &(handle->socket), value);
}

int
udx_socket_set_send_buffer_size (udx_socket_t *handle, int value) {
  if (value < 1) return UV_EINVAL;
  return uv_send_buffer_size((uv_handle_t *) &(handle->socket), &value);
}

int
udx_socket_get_recv_buffer_size (udx_socket_t *handle, int *value) {
  *value = 0;
  return uv_recv_buffer_size((uv_handle_t *) &(handle->socket), value);
}

int
udx_socket_set_recv_buffer_size (udx_socket_t *handle, int value) {
  if (value < 1) return UV_EINVAL;
  return uv_recv_buffer_size((uv_handle_t *) &(handle->socket), &value);
}

int
udx_socket_get_ttl (udx_socket_t *handle, int *ttl) {
  *ttl = handle->ttl;
  return 0;
}

int
udx_socket_set_ttl (udx_socket_t *handle, int ttl) {
  if (ttl < 1 || ttl > 255) return UV_EINVAL;
  handle->ttl = ttl;
  return uv_udp_set_ttl((uv_udp_t *) &(handle->socket), ttl);
}

int
udx_socket_bind (udx_socket_t *handle, const struct sockaddr *addr) {
  uv_udp_t *socket = &(handle->socket);
  uv_poll_t *poll = &(handle->io_poll);
  uv_os_fd_t fd;

  if (addr->sa_family == AF_INET) {
    handle->family = 4;
  } else if (addr->sa_family == AF_INET6) {
    handle->family = 6;
  } else {
    return UV_EINVAL;
  }

  // This might actually fail in practice, so
  int err = uv_udp_bind(socket, addr, 0);
  if (err) return err;

  // Asserting all the errors here as it massively simplifies error handling
  // and in practice non of these will fail, as all our handles are valid and alive.

  err = uv_udp_set_ttl(socket, handle->ttl);
  assert(err == 0);

  int send_buffer_size = UDX_DEFAULT_BUFFER_SIZE;
  err = uv_send_buffer_size((uv_handle_t *) socket, &send_buffer_size);
  assert(err == 0);

  int recv_buffer_size = UDX_DEFAULT_BUFFER_SIZE;
  err = uv_recv_buffer_size((uv_handle_t *) socket, &recv_buffer_size);
  assert(err == 0);

  err = uv_fileno((const uv_handle_t *) socket, &fd);
  assert(err == 0);

  err = uv_poll_init_socket(handle->udx->loop, poll, fd);
  assert(err == 0);

  handle->status |= UDX_SOCKET_BOUND;
  poll->data = handle;

  return update_poll(handle);
}

int
udx_socket_getsockname (udx_socket_t *handle, struct sockaddr *name, int *name_len) {
  return uv_udp_getsockname(&(handle->socket), name, name_len);
}

int
udx_socket_send (udx_socket_send_t *req, udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *dest, udx_socket_send_cb cb) {
  return udx_socket_send_ttl(req, handle, bufs, bufs_len, dest, 0, cb);
}

int
udx_socket_send_ttl (udx_socket_send_t *req, udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *dest, int ttl, udx_socket_send_cb cb) {
  if (ttl < 0 /* 0 is "default" */ || ttl > 255) return UV_EINVAL;

  assert(bufs_len == 1);

  req->handle = handle;
  req->on_send = cb;

  udx_packet_t *pkt = &(req->pkt);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_SEND;
  pkt->ttl = ttl;
  pkt->ctx = req;

  if (dest->sa_family == AF_INET) {
    pkt->dest_len = sizeof(struct sockaddr_in);
  } else if (dest->sa_family == AF_INET6) {
    pkt->dest_len = sizeof(struct sockaddr_in6);
  } else {
    return UV_EINVAL;
  }

  memcpy(&(pkt->dest), dest, pkt->dest_len);

  pkt->is_retransmit = 0;
  pkt->transmits = 0;

  pkt->bufs_len = 1;

  pkt->bufs[0] = bufs[0];

  pkt->fifo_gc = udx__fifo_push(&(handle->send_queue), pkt);

  return update_poll(handle);
}

int
udx_socket_recv_start (udx_socket_t *handle, udx_socket_recv_cb cb) {
  if (handle->status & UDX_SOCKET_RECEIVING) return UV_EALREADY;

  handle->on_recv = cb;
  handle->status |= UDX_SOCKET_RECEIVING;

  return update_poll(handle);
}

int
udx_socket_recv_stop (udx_socket_t *handle) {
  if ((handle->status & UDX_SOCKET_RECEIVING) == 0) return 0;

  handle->on_recv = NULL;
  handle->status ^= UDX_SOCKET_RECEIVING;

  return update_poll(handle);
}

int
udx_socket_close (udx_socket_t *handle, udx_socket_close_cb cb) {
  // if (handle->streams_len > 0) return UV_EBUSY;

  handle->status |= UDX_SOCKET_CLOSING;

  handle->on_close = cb;

  // allow stream packets to flush, but cancel anything else
  int queued = handle->send_queue.len;

  while (queued--) {
    udx_packet_t *pkt = udx__fifo_shift(&(handle->send_queue));
    if (pkt == NULL) break;

    if (pkt->type == UDX_PACKET_SEND) {
      udx_socket_send_t *req = pkt->ctx;

      if (req->on_send != NULL) {
        req->on_send(req, UV_ECANCELED);
      }

      continue;
    }

    // stream packet, allow them to flush, by requeueing them
    // flips the order but these are all state packets so whatevs
    udx__fifo_push(&(handle->send_queue), pkt);
  }

  if (handle->send_queue.len == 0) {
    close_handles(handle);
  }

  return 0;
}

int
udx_stream_init (udx_t *udx, udx_stream_t *handle, uint32_t local_id, udx_stream_close_cb close_cb) {
  ref_inc(udx);

  handle->local_id = local_id;
  handle->remote_id = 0;
  handle->set_id = 0;
  handle->status = 0;
  handle->out_of_order = 0;
  handle->recovery = 0;
  handle->socket = NULL;
  handle->relay_to = NULL;
  handle->udx = udx;

  handle->reordering_seen = false;
  handle->retransmitting = 0;

  handle->mtu = UDX_DEFAULT_MTU;

  handle->seq = 0;
  handle->ack = 0;
  handle->remote_acked = 0;

  handle->srtt = 0;
  handle->rttvar = 0;
  handle->rto = 1000;
  handle->rto_timeout = get_milliseconds() + handle->rto;
  handle->rack_timeout = 0;

  handle->rack_rtt_min = 0;
  handle->rack_rtt = 0;
  handle->rack_time_sent = 0;
  handle->rack_next_seq = 0;
  handle->rack_fack = 0;

  handle->deferred_ack = 0;

  handle->pkts_waiting = 0;
  handle->pkts_inflight = 0;
  handle->pkts_buffered = 0;
  handle->retransmits_waiting = 0;
  handle->seq_flushed = 0;

  handle->sacks = 0;
  handle->inflight = 0;
  handle->ssthresh = 255;
  handle->cwnd = UDX_CONG_INIT_CWND;
  handle->cwnd_cnt = 0;
  handle->rwnd = 0;

  handle->on_firewall = NULL;
  handle->on_read = NULL;
  handle->on_recv = NULL;
  handle->on_drain = NULL;
  handle->on_close = close_cb;

  // Clear congestion state
  memset(&(handle->cong), 0, sizeof(udx_cong_t));

  udx__cirbuf_init(&(handle->relaying_streams), 2);

  // Init stream write/read buffers
  udx__cirbuf_init(&(handle->outgoing), 16);
  udx__cirbuf_init(&(handle->incoming), 16);
  udx__fifo_init(&(handle->unordered), 1);

  handle->set_id = udx->streams_len++;

  if (udx->streams_len == udx->streams_max_len) {
    udx->streams_max_len *= 2;
    udx->streams = realloc(udx->streams, udx->streams_max_len * sizeof(udx_stream_t *));
  }

  udx->streams[handle->set_id] = handle;

  // Add the socket to the active set

  udx__cirbuf_set(&(udx->streams_by_id), (udx_cirbuf_val_t *) handle);

  return 0;
}

int
udx_stream_get_mtu (udx_stream_t *handle, uint16_t *mtu) {
  *mtu = handle->mtu;
  return 0;
}

int
udx_stream_set_mtu (udx_stream_t *handle, uint16_t mtu) {
  handle->mtu = mtu;
  return 0;
}

int
udx_stream_get_seq (udx_stream_t *handle, uint32_t *seq) {
  *seq = handle->seq;
  return 0;
}

int
udx_stream_set_seq (udx_stream_t *handle, uint32_t seq) {
  handle->seq = handle->seq_flushed = seq;
  return 0;
}

int
udx_stream_get_ack (udx_stream_t *handle, uint32_t *ack) {
  *ack = handle->ack;
  return 0;
}

int
udx_stream_set_ack (udx_stream_t *handle, uint32_t ack) {
  handle->ack = ack;
  return 0;
}

int
udx_stream_firewall (udx_stream_t *handle, udx_stream_firewall_cb cb) {
  handle->on_firewall = cb;
  return 0;
}

int
udx_stream_recv_start (udx_stream_t *handle, udx_stream_recv_cb cb) {
  if (handle->status & UDX_STREAM_RECEIVING) return UV_EALREADY;

  handle->on_recv = cb;
  handle->status |= UDX_STREAM_RECEIVING;

  return handle->socket == NULL ? 0 : update_poll(handle->socket);
}

int
udx_stream_recv_stop (udx_stream_t *handle) {
  if ((handle->status & UDX_STREAM_RECEIVING) == 0) return 0;

  handle->on_recv = NULL;
  handle->status ^= UDX_STREAM_RECEIVING;

  return handle->socket == NULL ? 0 : update_poll(handle->socket);
}

int
udx_stream_read_start (udx_stream_t *handle, udx_stream_read_cb cb) {
  if (handle->status & UDX_STREAM_READING) return UV_EALREADY;

  handle->on_read = cb;
  handle->status |= UDX_STREAM_READING;

  return handle->socket == NULL ? 0 : update_poll(handle->socket);
}

int
udx_stream_read_stop (udx_stream_t *handle) {
  if ((handle->status & UDX_STREAM_READING) == 0) return 0;

  handle->on_read = NULL;
  handle->status ^= UDX_STREAM_READING;

  return handle->socket == NULL ? 0 : update_poll(handle->socket);
}

static void
check_deferred_ack (udx_stream_t *handle) {
  if (handle->deferred_ack == 0) return;
  if (--(handle->deferred_ack) > 0) return;
  send_state_packet(handle);
}

int
udx_stream_check_timeouts (udx_stream_t *handle) {
  if ((handle->status & UDX_STREAM_CONNECTED) == 0) {
    return 0;
  }

  if (handle->remote_acked == handle->seq) {
    check_deferred_ack(handle);
    return 0;
  }

  const uint64_t now = handle->inflight ? get_milliseconds() : 0;

  if (handle->rack_timeout > 0 && now >= handle->rack_timeout) {
    rack_detect_loss(handle);
  }

  if (now > handle->rto_timeout) {
    // Bail out of fast recovery mode if we are in it
    handle->recovery = 0;

    // Make sure to clear all new packets that are in the queue
    unqueue_first_transmits(handle);

    // Reduce the congestion window (full reset)
    reduce_cwnd(handle, true);

    // Ensure it backs off until data is acked...
    handle->rto_timeout = now + 2 * handle->rto;

    // Consider all packet losts - seems to be the simple consensus across different stream impls
    // which we like cause it is nice and simple to implement.
    for (uint32_t seq = handle->remote_acked; seq != handle->seq_flushed; seq++) {
      udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&(handle->outgoing), seq);

      if (pkt == NULL || pkt->status != UDX_PACKET_INFLIGHT || pkt->is_retransmit) continue;

      if (pkt->transmits >= UDX_MAX_TRANSMITS) {
        handle->status |= UDX_STREAM_DESTROYED;
        close_maybe(handle, UV_ETIMEDOUT);
        return 1;
      }

      pkt->status = UDX_PACKET_WAITING;
      pkt->is_retransmit = UDX_SLOW_RETRANSMIT;

      handle->inflight -= pkt->size;
      handle->pkts_waiting++;
      handle->pkts_inflight--;
      handle->retransmits_waiting++;
    }

    debug_printf("timeout! pkt loss detected - inflight=%zu ssthresh=%u cwnd=%u acked=%u seq=%u rtt=%u\n", handle->inflight, handle->ssthresh, handle->cwnd, handle->remote_acked, handle->seq_flushed, handle->srtt);
  }

  check_deferred_ack(handle);

  int err = flush_waiting_packets(handle);
  return err < 0 ? err : 0;
}

int
udx_stream_connect (udx_stream_t *handle, udx_socket_t *socket, uint32_t remote_id, const struct sockaddr *remote_addr) {
  if (handle->status & UDX_STREAM_CONNECTED) {
    return UV_EISCONN;
  }

  handle->status |= UDX_STREAM_CONNECTED;

  handle->remote_id = remote_id;
  handle->socket = socket;

  if (remote_addr->sa_family == AF_INET) {
    handle->remote_addr_len = sizeof(struct sockaddr_in);
  } else if (remote_addr->sa_family == AF_INET6) {
    handle->remote_addr_len = sizeof(struct sockaddr_in6);
  } else {
    return UV_EINVAL;
  }

  memcpy(&(handle->remote_addr), remote_addr, handle->remote_addr_len);

  if (socket->family == 6 && handle->remote_addr.ss_family == AF_INET) {
    addr_to_v6((struct sockaddr_in *) &(handle->remote_addr));
    handle->remote_addr_len = sizeof(struct sockaddr_in6);
  }

  return update_poll(handle->socket);
}

int
udx_stream_relay_to (udx_stream_t *handle, udx_stream_t *destination) {
  if (handle->relay_to != NULL) return UV_EINVAL;

  handle->relay_to = destination;

  udx__cirbuf_set(&(destination->relaying_streams), (udx_cirbuf_val_t *) handle);

  return 0;
}

int
udx_stream_send (udx_stream_send_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_send_cb cb) {
  assert(bufs_len == 1);

  req->handle = handle;
  req->on_send = cb;

  udx_socket_t *socket = handle->socket;
  udx_packet_t *pkt = &(req->pkt);

  init_stream_packet(pkt, UDX_HEADER_MESSAGE, handle, &bufs[0]);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_SEND;
  pkt->ttl = 0;
  pkt->ctx = req;
  pkt->is_retransmit = 0;
  pkt->transmits = 0;

  pkt->fifo_gc = udx__fifo_push(&(socket->send_queue), pkt);
  udx__fifo_push(&(handle->unordered), pkt);

  return update_poll(socket);
}

int
udx_stream_write_resume (udx_stream_t *handle, udx_stream_drain_cb drain_cb) {
  handle->on_drain = drain_cb;
  return 0;
}

int
udx_stream_write (udx_stream_write_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb) {
  assert(bufs_len == 1);

  req->packets = 0;
  req->handle = handle;
  req->on_ack = ack_cb;

  // if this is the first inflight packet, we should "restart" rto timer
  if (handle->inflight == 0) {
    handle->rto_timeout = get_milliseconds() + handle->rto;
  }

  int err = 0;

  uv_buf_t buf = bufs[0];

  size_t mtu = handle->mtu - UDX_HEADER_SIZE;

  do {
    udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

    uv_buf_t buf_partial = uv_buf_init(buf.base, buf.len < mtu ? buf.len : mtu);

    init_stream_packet(pkt, UDX_HEADER_DATA, handle, &buf_partial);

    pkt->status = UDX_PACKET_WAITING;
    pkt->type = UDX_PACKET_STREAM_WRITE;
    pkt->ttl = 0;
    pkt->ctx = req;

    handle->seq++;
    req->packets++;

    buf.len -= buf_partial.len;
    buf.base += buf_partial.len;

    udx__cirbuf_set(&(handle->outgoing), (udx_cirbuf_val_t *) pkt);

    // If we are not the first packet in the queue, wait to send us until the queue is flushed...
    if (handle->pkts_waiting++ > 0) continue;
    err = send_data_packet(handle, pkt);
  } while (buf.len > 0 || err < 0);

  return err;
}

int
udx_stream_write_end (udx_stream_write_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb) {
  assert(bufs_len == 1);

  handle->status |= UDX_STREAM_ENDING;

  req->packets = 0;
  req->handle = handle;
  req->on_ack = ack_cb;

  int err = 0;

  uv_buf_t buf = bufs[0];

  size_t mtu = handle->mtu - UDX_HEADER_SIZE;

  do {
    udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

    uv_buf_t buf_partial = uv_buf_init(buf.base, buf.len < mtu ? buf.len : mtu);

    init_stream_packet(pkt, UDX_HEADER_END, handle, &buf_partial);

    pkt->status = UDX_PACKET_WAITING;
    pkt->type = UDX_PACKET_STREAM_WRITE;
    pkt->ttl = 0;
    pkt->ctx = req;

    handle->seq++;
    req->packets++;

    buf.len -= buf_partial.len;
    buf.base += buf_partial.len;

    udx__cirbuf_set(&(handle->outgoing), (udx_cirbuf_val_t *) pkt);

    // If we are not the first packet in the queue, wait to send us until the queue is flushed...
    if (handle->pkts_waiting++ > 0) continue;
    err = send_data_packet(handle, pkt);
  } while (buf.len > 0 || err < 0);

  return err;
}

int
udx_stream_destroy (udx_stream_t *handle) {
  if ((handle->status & UDX_STREAM_CONNECTED) == 0) {
    handle->status |= UDX_STREAM_DESTROYED;
    close_maybe(handle, 0);
    return 0;
  }

  handle->status |= UDX_STREAM_DESTROYING;

  // clear the outgoing packets immediately as we don't want anything to leave to the network
  // while the destroy packet is being flushed. we could also destroy incoming packets here, but
  // that creates some reentry trickiness incase this was called from on_read.
  clear_outgoing_packets(handle);

  if (handle->relay_to != NULL) {
    handle->status |= UDX_STREAM_DESTROYED;
    close_maybe(handle, 0);
    return 0;
  }

  udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

  uv_buf_t buf = uv_buf_init(NULL, 0);

  init_stream_packet(pkt, UDX_HEADER_DESTROY, handle, &buf);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_DESTROY;
  pkt->ttl = 0;
  pkt->ctx = handle;

  handle->seq++;

  udx__fifo_push(&(handle->socket->send_queue), pkt);
  udx__fifo_push(&(handle->unordered), pkt);

  int err = update_poll(handle->socket);
  return err < 0 ? err : 1;
}

static void
on_uv_getaddrinfo (uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
  udx_lookup_t *lookup = (udx_lookup_t *) req->data;

  if (status < 0) {
    lookup->on_lookup(lookup, status, NULL, 0);
  } else {
    lookup->on_lookup(lookup, status, res->ai_addr, res->ai_addrlen);
  }

  uv_freeaddrinfo(res);
}

int
udx_lookup (uv_loop_t *loop, udx_lookup_t *req, const char *host, unsigned int flags, udx_lookup_cb cb) {
  req->on_lookup = cb;
  req->req.data = req;

  memset(&req->hints, 0, sizeof(struct addrinfo));

  int family = AF_UNSPEC;

  if (flags & UDX_LOOKUP_FAMILY_IPV4) family = AF_INET;
  if (flags & UDX_LOOKUP_FAMILY_IPV6) family = AF_INET6;

  req->hints.ai_family = family;
  req->hints.ai_socktype = SOCK_STREAM;

  return uv_getaddrinfo(loop, &req->req, on_uv_getaddrinfo, host, NULL, &req->hints);
}

static int
cmp_interface (const void *a, const void *b) {
  const uv_interface_address_t *ia = a;
  const uv_interface_address_t *ib = b;

  int result;

  result = strcmp(ia->phys_addr, ib->phys_addr);
  if (result != 0) return result;

  result = memcmp(&ia->address, &ib->address, sizeof(ia->address));
  if (result != 0) return result;

  return 0;
}

static void
on_interface_event_interval (uv_timer_t *timer) {
  udx_interface_event_t *handle = (udx_interface_event_t *) timer->data;

  uv_interface_address_t *prev_addrs = handle->addrs;
  int prev_addrs_len = handle->addrs_len;
  bool prev_sorted = handle->sorted;

  int err = uv_interface_addresses(&(handle->addrs), &(handle->addrs_len));
  if (err < 0) {
    handle->on_event(handle, err);
    return;
  }

  handle->sorted = false;

  bool changed = handle->addrs_len != prev_addrs_len;

  for (int i = 0; !changed && i < handle->addrs_len; i++) {
    if (cmp_interface(&handle->addrs[i], &prev_addrs[i]) == 0) {
      continue;
    }

    if (handle->sorted) changed = true;
    else {
      qsort(handle->addrs, handle->addrs_len, sizeof(uv_interface_address_t), cmp_interface);

      if (!prev_sorted) {
        qsort(prev_addrs, prev_addrs_len, sizeof(uv_interface_address_t), cmp_interface);
      }

      handle->sorted = true;
      i = 0;
    }
  }

  if (changed) handle->on_event(handle, 0);
  else handle->sorted = prev_sorted;

  uv_free_interface_addresses(prev_addrs, prev_addrs_len);
}

static void
on_interface_event_close (uv_handle_t *handle) {
  udx_interface_event_t *event = (udx_interface_event_t *) handle->data;

  if (event->on_close != NULL) {
    event->on_close(event);
  }
}

int
udx_interface_event_init (uv_loop_t *loop, udx_interface_event_t *handle) {
  handle->loop = loop;
  handle->sorted = false;

  int err = uv_interface_addresses(&(handle->addrs), &(handle->addrs_len));
  if (err < 0) return err;

  err = uv_timer_init(handle->loop, &(handle->timer));
  if (err < 0) return err;

  handle->timer.data = handle;

  return 0;
}

int
udx_interface_event_start (udx_interface_event_t *handle, udx_interface_event_cb cb, uint64_t frequency) {
  handle->on_event = cb;

  int err = uv_timer_start(&(handle->timer), on_interface_event_interval, 0, frequency);
  return err < 0 ? err : 0;
}

int
udx_interface_event_stop (udx_interface_event_t *handle) {
  handle->on_event = NULL;

  int err = uv_timer_stop(&(handle->timer));
  return err < 0 ? err : 0;
}

int
udx_interface_event_close (udx_interface_event_t *handle, udx_interface_event_close_cb cb) {
  handle->on_event = NULL;
  handle->on_close = cb;

  uv_free_interface_addresses(handle->addrs, handle->addrs_len);

  int err = uv_timer_stop(&(handle->timer));
  if (err < 0) return err;

  uv_close((uv_handle_t *) &(handle->timer), on_interface_event_close);

  return 0;
}
