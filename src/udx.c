#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "../include/udx.h"
#include "internal.h"

#include "cirbuf.h"
#include "debug.h"
#include "endian.h"
#include "io.h"
#include "link.h"
#include "queue.h"
#include "win_filter.h"

#define UDX_STREAM_ALL_ENDED (UDX_STREAM_ENDED | UDX_STREAM_ENDED_REMOTE)
#define UDX_STREAM_DEAD      (UDX_STREAM_DESTROYING | UDX_STREAM_CLOSED)

#define UDX_STREAM_SHOULD_READ (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD)
#define UDX_STREAM_READ        0

#define UDX_STREAM_SHOULD_END (UDX_STREAM_ENDING | UDX_STREAM_ENDED | UDX_STREAM_DEAD)
#define UDX_STREAM_END        UDX_STREAM_ENDING

#define UDX_STREAM_SHOULD_END_REMOTE (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD | UDX_STREAM_ENDING_REMOTE)
#define UDX_STREAM_END_REMOTE        UDX_STREAM_ENDING_REMOTE

#define UDX_HEADER_DATA_OR_END (UDX_HEADER_DATA | UDX_HEADER_END)

#define UDX_DEFAULT_TTL         64
#define UDX_INIT_PACING_RATE    25000 // 25MB/s, 200mbit. updated by bbr_init
#define UDX_DEFAULT_SNDBUF_SIZE 212992

#define UDX_MAX_RTO_TIMEOUTS 6

#define UDX_RTO_MAX_MS        30000
#define UDX_RTT_MAX_MS        30000
#define UDX_RTT_MIN_WINDOW_MS 300000            // 300 seconds, same as Linux default
#define UDX_DEFAULT_RWND_MAX  (4 * 1024 * 1024) // arbitrary, ~175 1500 mtu packets, @20ms latency = 416 mbits/sec

#define UDX_HIGH_WATERMARK 262144

#define UDX_TLP_MAX_ACK_DELAY 2

#define UDX_PROBE_TYPE_NONE 0 // can only be sent if cwnd permits
#define UDX_PROBE_TYPE_TLP  1 // packet is a tail-loss probe, can exceed cwnd, set tail probe flags
#define UDX_PROBE_TYPE_ZWP  2 // packet is a zero-window probe, can exceed rwnd

#define UDX_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void
arm_stream_timers (udx_stream_t *stream, bool sent_tlp);

typedef struct {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int type;

  uv_buf_t buf;
} udx_pending_read_t;

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

uint32_t
udx__max_payload (udx_stream_t *stream) {
  assert(stream->mtu > (AF_INET ? UDX_IPV4_HEADER_SIZE : UDX_IPV6_HEADER_SIZE));
  return stream->mtu - (stream->remote_addr.ss_family == AF_INET ? UDX_IPV4_HEADER_SIZE : UDX_IPV6_HEADER_SIZE);
}

static inline uint32_t
cwnd_in_bytes (udx_stream_t *stream) {
  return stream->cwnd * udx__max_payload(stream);
}

static inline uint32_t
send_window_in_bytes (udx_stream_t *stream) {
  return min_uint32(cwnd_in_bytes(stream), stream->send_rwnd);
}

// rounds down
static inline uint32_t
send_rwnd_in_packets (udx_stream_t *stream) {
  return stream->send_rwnd / udx__max_payload(stream);
}

static inline uint32_t
send_window_in_packets (udx_stream_t *stream) {
  return min_uint32(stream->cwnd, send_rwnd_in_packets(stream));
}

static void
ref_dec (udx_t *udx) {
  udx->refs--;

  if (udx->refs) return;

  if (udx->has_streams) {
    udx__cirbuf_destroy(&(udx->streams_by_id));
    udx->has_streams = false;
  }

  if (udx->on_idle != NULL) {
    udx->on_idle(udx);
  }
}

static void
on_uv_close (uv_handle_t *handle) {
  udx_socket_t *socket = (udx_socket_t *) handle;
  udx_t *udx = socket->udx;

  if (socket->on_close != NULL) {
    socket->on_close(socket);
  }

  ref_dec(udx);
}

static void
update_pacing_time (udx_stream_t *stream);

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

int
udx_stream_write_sizeof (int nwbufs) {
  assert(nwbufs > 0); // must have at least one nwbuf
  return sizeof(udx_stream_write_t) + sizeof(udx_stream_write_buf_t) * nwbufs;
}

static void
on_bytes_acked (udx_stream_write_buf_t *wbuf, size_t bytes, bool cancelled) {
  udx_stream_write_t *write = wbuf->write;
  udx_stream_t *stream = write->stream;

  // todo: remove this check? does it matter if we consider them 'not in flight' if we cancel them anyways?
  if (!cancelled) {
    assert(bytes <= wbuf->bytes_inflight);
    wbuf->bytes_inflight -= bytes;
  }
  wbuf->bytes_acked += bytes;
  assert(wbuf->bytes_acked <= wbuf->buf.len);

  write->bytes_acked += bytes;
  assert(write->bytes_acked <= write->size);

  assert(bytes <= stream->writes_queued_bytes);
  stream->writes_queued_bytes -= bytes;

  // if high watermark (262k+send window bytes queued for writing was hit)
  // stream->writes_queued_bytes > UDX_HIGH_WATERMARK + send_window_in_bytes(stream)
  // and we are now below high watermark

  if (stream->hit_high_watermark && stream->writes_queued_bytes < UDX_HIGH_WATERMARK + send_window_in_bytes(stream)) {
    stream->hit_high_watermark = false;
    if (stream->on_drain != NULL) stream->on_drain(stream);
  }
}

static void
deref_packet (udx_packet_t *pkt) {
  if (--pkt->ref_count == 0) {
    if (pkt->bufs != &pkt->buf_sml[0]) {
      free(pkt->bufs);
      free(pkt->wbufs);
    }
    free(pkt);
  }
}

static void
cancel_packet (udx_packet_t *pkt) {
  uv_buf_t *bufs = pkt->bufs;
  udx_stream_write_buf_t **wbufs = pkt->wbufs;

  for (int i = 0; i < pkt->nwbufs; i++) {
    size_t buf_len = bufs[i + 1].len;
    udx_stream_write_buf_t *wbuf = wbufs[i];
    on_bytes_acked(wbuf, buf_len, true);

    // todo: move into on_bytes_acked itself
    udx_stream_write_t *write = wbuf->write;

    if (write->bytes_acked == write->size && write->on_ack) {
      write->on_ack(write, UV_ECANCELED, 0);
    }
  }

  deref_packet(pkt);
}

static void
clear_outgoing_packets (udx_stream_t *stream) {
  // todo: skip the math, and just
  // 1. destroy all packets
  // 2. destroy all wbufs
  // 3. set write->bytes_acked = write->size and call ack(cancel) on all writes

  if (stream->pkt) {
    assert(stream->pkt->ref_count == 1);
    cancel_packet(stream->pkt);
  }

  // We should make sure all existing packets do not send, and notify the user that they failed
  for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(&(stream->outgoing), seq);

    if (pkt == NULL) continue;

    cancel_packet(pkt);
  }

  while (stream->write_queue.len > 0) {
    udx_stream_write_buf_t *wbuf = udx__queue_data(udx__queue_shift(&stream->write_queue), udx_stream_write_buf_t, queue);
    assert(wbuf != NULL);

    on_bytes_acked(wbuf, wbuf->buf.len - wbuf->bytes_acked, true);
    // todo: move into on_bytes_acked itself
    udx_stream_write_t *write = wbuf->write;
    if (write->bytes_acked == write->size && write->on_ack) {
      write->on_ack(write, UV_ECANCELED, 0);
    }
  }
}

// returns the rwnd to advertise to the sender
// to provide a rwnd value the user provides two values
// 1. a maximum buffer size. default is UDX_DEFAULT_RWND_MAX
// 2. a callback to return the number of bytes already in the buffer.
// the window is then set to the

static uint32_t
get_recv_rwnd (udx_stream_t *stream) {
  uint32_t bufsize = 0;
  if (stream->get_read_buffer_size) {
    bufsize = stream->get_read_buffer_size(stream);
  }
  if (stream->recv_rwnd_max > bufsize) {
    return stream->recv_rwnd_max - bufsize;
  } else {
    return 0;
  }
}

static void
udx_write_header (uint8_t header[20], udx_stream_t *stream, int type) {
  uint8_t *b = header;

  // 8 bit magic byte + 8 bit version + 8 bit type + 8 bit extensions
  *(b++) = UDX_MAGIC_BYTE;
  *(b++) = UDX_VERSION;
  *(b++) = (uint8_t) type;
  *(b++) = 0; // data offset

  uint32_t *i = (uint32_t *) b;

  // 32 bit (le) remote id
  *(i++) = udx__swap_uint32_if_be(stream->remote_id);
  // 32 bit (le) recv window
  *(i++) = udx__swap_uint32_if_be(get_recv_rwnd(stream));
  // 32 bit (le) seq
  *(i++) = udx__swap_uint32_if_be(stream->seq);
  // 32 bit (le) ack
  *(i++) = udx__swap_uint32_if_be(stream->ack);
}

// returns 1 on success, zero if packet can't be promoted to a probe packet
static int
mtu_probeify_packet (udx_packet_t *pkt, int wanted_size) {

  assert(wanted_size > pkt->size);

  // cannot probeify a packet with 1) no data 2) already has padding
  if (pkt->nwbufs < 1 || pkt->header[3] != 0) {
    return 0;
  }

  bool ipv4 = pkt->remote_addr.ss_family == AF_INET;

  int header_size = (ipv4 ? UDX_IPV4_HEADER_SIZE : UDX_IPV6_HEADER_SIZE) - 20;
  int padding_size = wanted_size - (pkt->size + (ipv4 ? UDX_IPV4_HEADER_SIZE : UDX_IPV6_HEADER_SIZE) - 20);
  if (padding_size > 255) {
    return 0;
  }
  debug_printf("mtu: probeify rid=%u seq=%u size=%u wanted=%d padding=%d\n", udx__swap_uint32_if_be(((unsigned int *) pkt->header)[1]), pkt->seq, pkt->size + header_size, wanted_size, padding_size);

  pkt->header[3] = padding_size;
  pkt->is_mtu_probe = true;
  return 1;
}

// removes probe padding and stream->mtu_probe_wanted
static void
mtu_unprobeify_packet (udx_packet_t *pkt, udx_stream_t *stream) {
  assert(pkt->is_mtu_probe);

  pkt->header[3] = 0;
  pkt->is_mtu_probe = false;

  debug_printf("mtu: probe failed rid=%u %d/%d", stream->remote_id, stream->mtu_probe_count, UDX_MTU_MAX_PROBES);
  if (stream->mtu_state == UDX_MTU_STATE_SEARCH) {
    if (stream->mtu_probe_count >= UDX_MTU_MAX_PROBES) {
      debug_printf(" established mtu=%d via timeout", stream->mtu);
      stream->mtu_state = UDX_MTU_STATE_SEARCH_COMPLETE;
    } else {
      stream->mtu_probe_wanted = true;
    }
  }
  debug_printf("\n");
}

static void
finalize_maybe (uv_handle_t *timer) {
  udx_stream_t *stream = timer->data;
  udx_t *udx = stream->udx;
  if (--stream->nrefs > 0) return;

  if (stream->on_finalize) {
    stream->on_finalize(stream);
  }
  ref_dec(udx);
}

// close stream immediately.
// 1. if you call this on the receive path (process_packet)
// you must immediately return and process another packet
// 2. if you call this on the send path, you must immediately return from
// send_stream_packets

static int
close_stream (udx_stream_t *stream, int err) {
  assert((stream->status & UDX_STREAM_CLOSED) == 0);
  stream->status |= UDX_STREAM_CLOSED;
  stream->status &= ~UDX_STREAM_CONNECTED;

  udx_t *udx = stream->udx;
  udx_socket_t *socket = stream->socket;

  if (socket != NULL) {
    udx__link_remove(socket->streams, stream);
  } else {
    udx__link_remove(udx->streams, stream);
  }

  udx__cirbuf_remove(&(udx->streams_by_id), stream->local_id);

  // stream on_close called before acks are cancelled!
  // this is to prevent on_ack / on_send reentry while
  // stream is closing

  if (stream->on_close != NULL) {
    stream->on_close(stream, err);
  }

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

    if (stream) {
      stream->relay_to = NULL;
      udx_stream_destroy(stream);
    }
  }

  udx__cirbuf_destroy(&stream->relaying_streams);
  udx__cirbuf_destroy(&stream->incoming);
  udx__cirbuf_destroy(&stream->outgoing);

  uv_timer_stop(&stream->rto_timer);
  uv_timer_stop(&stream->rack_reo_timer);
  uv_timer_stop(&stream->tlp_and_keepalive_timer);
  uv_timer_stop(&stream->zwp_timer);
  uv_timer_stop(&stream->refill_pacing_timer);

  uv_close((uv_handle_t *) &stream->rto_timer, finalize_maybe);
  uv_close((uv_handle_t *) &stream->rack_reo_timer, finalize_maybe);
  uv_close((uv_handle_t *) &stream->tlp_and_keepalive_timer, finalize_maybe);
  uv_close((uv_handle_t *) &stream->zwp_timer, finalize_maybe);
  uv_close((uv_handle_t *) &stream->refill_pacing_timer, finalize_maybe);
  uv_close((uv_handle_t *) &stream->pending_packet_prepare, finalize_maybe);

  if (udx->teardown && socket != NULL && socket->streams == NULL) {
    udx_socket_close(socket);
  }

  return 1;
}

static void
udx_rto_timeout (uv_timer_t *handle);

static bool
_maybe_adjust_ttl (udx_socket_t *socket) {

  if (socket->specific_ttl_send_queue.len == 0) {
    return false;
  }

  udx_socket_send_t *req = udx__queue_data(udx__queue_peek(&socket->specific_ttl_send_queue), udx_socket_send_t, queue);

  if (req->place_in_queue == socket->packets_sent_via_uv_send_queue) {
    uv_udp_set_ttl(&socket->uv_udp, req->ttl);
    return true;
  }
  return false;
}

// every packet sent via uv_udp_send() must call this function as part of their callback
static void
maybe_adjust_ttl (uv_udp_t *udp) {
  udx_socket_t *socket = (udx_socket_t *) udp; // todo: use offsetof instead?
  socket->packets_sent_via_uv_send_queue++;

  _maybe_adjust_ttl(socket);
}

// used to free simple (ack, probe, and relay) memory
// stream-write, stream-send and stream-destroy packets have their own callbacks
void
on_packet_send_slow (uv_udp_send_t *req, int status) {
  maybe_adjust_ttl(req->handle);

  UDX_UNUSED(status);
  free(req);
}

static void
send_probe (udx_stream_t *stream) {
  if (!stream->socket) {
    return;
  }

  // todo: send a data packet with seq=remote_acked-1 instead

  alignas(4) uint8_t header[20];

  udx_write_header(header, stream, UDX_HEADER_HEARTBEAT);

  // fast path
  uv_buf_t buf = uv_buf_init((char *) header, sizeof(header));
  int err = uv_udp_try_send(&stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr);

  if (err == UV_EAGAIN) {
    // slow path
    uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t) + buf.len);
    char *data = (char *) (req + 1);
    memcpy(data, buf.base, buf.len);
    buf.base = data;
    req->data = stream;
    int err = uv_udp_send(req, &stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr, on_packet_send_slow);
    if (err) {
      debug_printf("uv_udp_send error: %s\n", uv_strerror(err));
    }
  }

  // consider the probe to be sent, even on slow path.
  stream->packets_tx++;
  stream->bytes_tx += buf.len;

  stream->socket->packets_tx++;
  stream->socket->bytes_tx += buf.len;
  stream->udx->packets_tx++;
  stream->udx->bytes_tx += buf.len;
}

static void
udx_keepalive_timeout (uv_timer_t *timer) {
  udx_stream_t *stream = timer->data;
  assert(stream->seq == stream->remote_acked);

  send_probe(stream);

  uv_timer_start(&stream->tlp_and_keepalive_timer, udx_keepalive_timeout, stream->keepalive_timeout_ms, 0);
}

static void
schedule_loss_probe (udx_stream_t *stream);

// rack recovery implemented using https://datatracker.ietf.org/doc/rfc8985/

uint32_t
udx_rtt_min (udx_stream_t *stream) {
  return win_filter_get(&stream->rtt_min);
}

static inline uint32_t
rack_update_reo_wnd (udx_stream_t *stream) {
  // rack 6.2.4
  // TODO: add the DSACK logic also (skipped for now as we didnt impl and only recommended...)

  if (!stream->reordering_seen) {
    if (stream->ca_state == UDX_CA_RECOVERY || stream->ca_state == UDX_CA_LOSS) return 0;
    if (stream->sacks >= 3) return 0;
  }

  uint32_t r = udx_rtt_min(stream) / 4;
  return r < stream->srtt ? r : stream->srtt;
}

static void
send_ack (udx_stream_t *stream) {
  if (!stream->socket) {
    stream->ack_needed = true; // defer until we are connected
    return;
  }
  // todo: if data is available for writing then write a data + ack packet

  struct {
    uint8_t header[20];
    struct {
      uint32_t start;
      uint32_t end;
    } sacks[UDX_MAX_SACKS];
  } pkt;

  bool in_sack_block = false;
  int ooo = stream->out_of_order;
  int nsacks = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  // todo: highest sequence should end at rack.fack, use that as maximum?
  for (int i = 0; i < 65536 && ooo > 0 && nsacks < UDX_MAX_SACKS; i++) {
    uint32_t seq = stream->ack + 1 + i;
    bool received = udx__cirbuf_get(&stream->incoming, seq) != NULL;
    if (received) {
      if (!in_sack_block) {
        in_sack_block = true;
        start = seq;
        end = seq + 1;
      } else {
        end = seq + 1;
      }
    } else {
      ooo--;
      if (in_sack_block) {
        in_sack_block = false;
        pkt.sacks[nsacks].start = udx__swap_uint32_if_be(start);
        pkt.sacks[nsacks].end = udx__swap_uint32_if_be(end);
        nsacks++;
      }
    }
  }

  if (in_sack_block && nsacks < UDX_MAX_SACKS) {
    pkt.sacks[nsacks].start = udx__swap_uint32_if_be(start);
    pkt.sacks[nsacks].end = udx__swap_uint32_if_be(end);
    nsacks++;
  }

  // debug_printf("sending ack ack=%u nsasks=%d\n", stream->ack, nsacks);

  udx_write_header(pkt.header, stream, nsacks > 0 ? UDX_HEADER_SACK : 0);
  // fast path

  uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt.header) + sizeof(pkt.sacks[0]) * nsacks);
  int err = uv_udp_try_send(&stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr);

  if (err == UV_EAGAIN) {
    // slow path
    uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t) + buf.len);
    char *data = (char *) (req + 1);
    memcpy(data, buf.base, buf.len);
    buf.base = data;
    req->data = stream;

    int err = uv_udp_send(req, &stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr, on_packet_send_slow);
    if (err) {
      debug_printf("uv_udp_send: err=%s\n", uv_strerror(err));
    }
  }

  // consider the ack to be sent, even on slow path.
  stream->packets_tx++;
  stream->bytes_tx += buf.len;

  stream->socket->packets_tx++;
  stream->socket->bytes_tx += buf.len;
  stream->udx->packets_tx++;
  stream->udx->bytes_tx += buf.len;

  if ((stream->status & UDX_STREAM_SHOULD_END_REMOTE) == UDX_STREAM_END_REMOTE && seq_compare(stream->remote_ended, stream->ack) <= 0) {
    stream->status |= UDX_STREAM_ENDED_REMOTE;
    if (stream->on_read != NULL) {
      uv_buf_t b = uv_buf_init(NULL, 0);
      stream->on_read(stream, UV_EOF, &b);
    }
  }

  if ((stream->status & UDX_STREAM_ALL_ENDED) == UDX_STREAM_ALL_ENDED) {
    assert(stream->retransmit_queue.len == 0);
    assert(stream->write_queue.len == 0);
    close_stream(stream, 0);
  }
}

static bool
stream_may_send (udx_stream_t *stream) {
  update_pacing_time(stream);
  if (stream->tb_available == 0) {
    return false;
  }
  return stream->inflight_queue.len < send_window_in_packets(stream);
}

void
pacing_timer_timeout (uv_timer_t *timer);

static void
on_stream_data_write (uv_udp_send_t *send, int status) {
  udx_packet_t *pkt = (udx_packet_t *) ((char *) send - offsetof(udx_packet_t, uv_udp_send));

  if (status) {
    debug_printf("sendmsg: %s\n", uv_strerror(status));
  }

  maybe_adjust_ttl(send->handle); // send is freed with packet
  deref_packet(pkt);
}

// called by send_new_packet() and retransmit_packet()
static void
_send_packet (udx_stream_t *stream, udx_packet_t *pkt, bool is_retransmit) {

  udx__rate_check_app_limited(stream);

  if (pkt->transmits < 255) pkt->transmits++;
  if (is_retransmit) pkt->retransmitted = true; // true if packet was _ever_ retransmitted

  pkt->time_sent = uv_now(stream->udx->loop);
  udx__rate_pkt_sent(stream, pkt);

  if (pkt->lost) {
    // is a retransmit
    udx__queue_unlink(&stream->retransmit_queue, &pkt->queue);
    pkt->lost = false;
  }

  udx__queue_tail(&stream->inflight_queue, &pkt->queue);
  stream->inflight += pkt->size;

  uv_buf_t *bufs = pkt->bufs;
  int nbufs = pkt->nwbufs + 1; // udx header

  uv_buf_t _bufs[UDX_MAX_COMBINED_WRITES + 2];

  if (pkt->is_mtu_probe) {
    size_t padding_size = pkt->header[3];
    static char probe_data[256] = {0};
    _bufs[0] = bufs[0];
    _bufs[1].base = probe_data;
    _bufs[1].len = padding_size;

    for (int i = 1; i < nbufs; i++) {
      _bufs[1 + i] = bufs[i];
    }
    bufs = _bufs;
    nbufs = nbufs + 1;
  }

  bool drop_packet = false;

  // debug code
  if (stream->udx->debug_flags & UDX_DEBUG_FORCE_DROP_PROBES && pkt->is_mtu_probe) {
    drop_packet = true;
  }

  // debug code for testing mtu probe logic
  // drop every packet, but let them through if they've already been dropped twice to keep the test predictable
  const uint32_t drop_every_n = 3;
  static uint32_t n;
  if ((stream->udx->debug_flags & UDX_DEBUG_FORCE_DROP_DATA) && (n++ % drop_every_n == 0) && pkt->transmits < 3) {
    drop_packet = true;
  }

  pkt->ref_count++;
  assert(pkt->ref_count == 2);

  if (drop_packet) {
    deref_packet(pkt);
  } else {
    int err = uv_udp_send(&pkt->uv_udp_send, &stream->socket->uv_udp, bufs, nbufs, (struct sockaddr *) &pkt->remote_addr, on_stream_data_write);
    if (err) {
      debug_printf("uv_udp_send error: %s\n", uv_strerror(err));
    }
  }

  udx_socket_t *socket = stream->socket;
  udx_t *udx = stream->udx;

  stream->packets_tx++;
  stream->bytes_tx += pkt->size;

  socket->packets_tx++;
  socket->bytes_tx += pkt->size;

  udx->packets_tx++;
  udx->bytes_tx += pkt->size;

  stream->tb_available = pkt->size > stream->tb_available ? 0 : stream->tb_available - pkt->size;

  if (stream->tb_available == 0) {
    uv_timer_start(&stream->refill_pacing_timer, pacing_timer_timeout, 1, 0);
  }
}

static void
reset_next_packet (udx_stream_t *stream) {

  stream->pkt_capacity = udx__max_payload(stream);
  stream->pkt_header_flag = 0;
  stream->pkt = NULL;

  // allocate packet
  // most fields are not set until sending
  stream->pkt = malloc(sizeof(udx_packet_t));
  udx_packet_t *pkt = stream->pkt;
  memset(pkt, 0, sizeof(*pkt));
  pkt->ref_count = 1;
  pkt->bufs = &pkt->buf_sml[0];
  pkt->wbufs = &pkt->wbuf_sml[0];
  pkt->size = 20;
  pkt->nwbufs_capacity = UDX_ARRAY_SIZE(pkt->wbuf_sml);
  pkt->nwbufs = 0;

  uv_prepare_stop(&stream->pending_packet_prepare);
}

// called by send_new_packet and on_pending_packet_prepare
// sends stream->pkt
static void
_send_new_packet (udx_stream_t *stream, int probe_type) {

  assert((stream->pkt_header_flag & ~(UDX_HEADER_DATA_OR_END)) == 0);

  bool inflight_queue_was_empty = stream->inflight_queue.len == 0;
  bool tlp = probe_type == UDX_PROBE_TYPE_TLP;

  udx_packet_t *pkt = stream->pkt;

  udx_write_header(pkt->header, stream, stream->pkt_header_flag);
  pkt->seq = stream->seq;
  pkt->stream = stream; // todo: necessary?
  pkt->remote_addr = stream->remote_addr;
  pkt->remote_addr_len = stream->remote_addr_len;
  pkt->ref_count = 1;

  pkt->bufs[0] = uv_buf_init((char *) &pkt->header, UDX_HEADER_SIZE);

  bool mtu_probe = stream->mtu_probe_wanted && probe_type == UDX_PROBE_TYPE_NONE && !stream->remote_changing && mtu_probeify_packet(pkt, stream->mtu_probe_size);

  udx__cirbuf_set(&stream->outgoing, (udx_cirbuf_val_t *) pkt);
  _send_packet(stream, pkt, false);

  stream->seq++;

  if (inflight_queue_was_empty) {
    bbr_on_transmit_start(stream, uv_now(stream->udx->loop));
  }

  if (mtu_probe) {
    stream->mtu_probe_count++;
    stream->mtu_probe_wanted = false;
  }

  if (tlp) {
    stream->tlp_is_retrans = false;
    stream->tlp_in_flight = true;
    stream->tlp_end_seq = pkt->seq;
    stream->tlp_permitted = false;
  }

  arm_stream_timers(stream, tlp);

  assert(pkt->size > 0 && pkt->size < 1500);

  reset_next_packet(stream);
}

void
on_pending_packet_prepare (uv_prepare_t *check) {
  udx_stream_t *stream = container_of(check, udx_stream_t, pending_packet_prepare);
  _send_new_packet(stream, UDX_PROBE_TYPE_NONE); // we only defer non-probe (ZWP|TLP) packets
}

// called by send_packets and when a tlp or zwp is needed.
// return true  when successfully queueing a packet,
//        false when stream window is full or no data is available
// if probe is set (tlp or zwp) we can ignore the window for this packet
static bool
send_new_packet (udx_stream_t *stream, int probe_type) {
  if (stream->write_queue.len == 0) return false;
  if (!stream_may_send(stream) && probe_type == UDX_PROBE_TYPE_NONE) return false;

  bool tlp = probe_type == UDX_PROBE_TYPE_TLP;
  udx_packet_t *pkt = stream->pkt;

  while (stream->pkt_capacity > 0 && stream->write_queue.len > 0) {
    udx_stream_write_buf_t *wbuf = udx__queue_data(udx__queue_peek(&stream->write_queue), udx_stream_write_buf_t, queue);

    uint64_t writesz = wbuf->buf.len - wbuf->bytes_acked - wbuf->bytes_inflight;

    size_t len = min_uint64(stream->pkt_capacity, writesz);
    // printf("len=%lu capacity=%lu writesz=%lu\n", len, capacity, writesz);

    uv_buf_t partial = uv_buf_init(wbuf->buf.base + wbuf->bytes_acked + wbuf->bytes_inflight, len);
    wbuf->bytes_inflight += len;
    stream->pkt_capacity -= len;
    pkt->size += len;

    if (pkt->nwbufs == pkt->nwbufs_capacity) {
      pkt->nwbufs_capacity *= 2;
      bool first_alloc = pkt->bufs == pkt->buf_sml;

      if (first_alloc) {
        pkt->bufs = malloc((pkt->nwbufs_capacity + 1) * sizeof(pkt->bufs[0]));
        pkt->wbufs = malloc((pkt->nwbufs_capacity + 1) * sizeof(pkt->wbufs[0]));
        memcpy(pkt->bufs, pkt->buf_sml, sizeof(pkt->buf_sml));
        memcpy(pkt->wbufs, pkt->wbuf_sml, sizeof(pkt->wbuf_sml));
      } else {
        pkt->bufs = realloc(pkt->bufs, (pkt->nwbufs_capacity + 1) * sizeof(pkt->bufs[0]));
        pkt->wbufs = realloc(pkt->wbufs, pkt->nwbufs_capacity * sizeof(pkt->wbufs[0]));
      }
    }

    pkt->bufs[pkt->nwbufs + 1] = partial;
    pkt->wbufs[pkt->nwbufs] = wbuf;

    pkt->nwbufs++;

    if (len > 0) {
      stream->pkt_header_flag |= UDX_HEADER_DATA;
    }

    if ((wbuf->bytes_acked + wbuf->bytes_inflight) == wbuf->buf.len) {
      if (wbuf->is_write_end) {
        stream->pkt_header_flag |= UDX_HEADER_END;
      }
      udx__queue_shift(&stream->write_queue);
    }
  }

  if (stream->pkt_capacity == 0 || tlp || (stream->pkt_header_flag & UDX_HEADER_END)) {
    _send_new_packet(stream, probe_type);
    return true;
  }

  // we have a partial packet, defer sending until the 'check' phase
  // to give a chance to append more data before transmission
  if (stream->pkt_header_flag & UDX_HEADER_DATA) {
    uv_prepare_start(&stream->pending_packet_prepare, on_pending_packet_prepare);
  }

  return false;
}

void
retransmit_packet (udx_stream_t *stream, udx_packet_t *pkt) {
  bool inflight_queue_was_empty = stream->inflight_queue.len == 0;

  _send_packet(stream, pkt, true);

  stream->retransmit_count++;

  if (inflight_queue_was_empty) {
    bbr_on_transmit_start(stream, uv_now(stream->udx->loop));
  }

  arm_stream_timers(stream, false);
  return;
}

static void
send_packets (udx_stream_t *stream) {

  while (stream->retransmit_queue.len > 0 && stream_may_send(stream)) {
    udx_packet_t *pkt = udx__queue_data(udx__queue_peek(&stream->retransmit_queue), udx_packet_t, queue);
    assert(pkt != NULL);

    retransmit_packet(stream, pkt);
  }

  while (send_new_packet(stream, UDX_PROBE_TYPE_NONE)) {
    ;
  }
}

static void
udx_tlp_timeout (uv_timer_t *timer) {
  // rack 7.3
  udx_stream_t *stream = timer->data;

  assert(stream->status & UDX_STREAM_CONNECTED);

  if (stream->remote_acked == stream->seq) {
    return;
  }

  if (stream->tlp_in_flight || !stream->tlp_permitted) {
    schedule_loss_probe(stream);
    return;
  }

  // first try to send a new packet
  if (!send_new_packet(stream, UDX_PROBE_TYPE_TLP)) {

    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&stream->outgoing, stream->seq - 1);

    if (!pkt || pkt->lost || pkt->ref_count == 2) {
      schedule_loss_probe(stream);
      return;
    }

    debug_printf("udx: making tlp from existing packet %u\n", pkt->seq);

    udx__queue_unlink(&stream->inflight_queue, &pkt->queue); // retransmit will add it back
    retransmit_packet(stream, pkt);

    stream->tlp_is_retrans = true;
    stream->tlp_in_flight = true;
    stream->tlp_end_seq = pkt->seq;
    stream->tlp_permitted = false;
  }
}

// schedule or re-schedule TLP. fired on either
// 1. new data transmission
// 2. cumulative ack while not in recovery
static void
schedule_loss_probe (udx_stream_t *stream) {
  // rack 7.2
  uint32_t pto = 1000;

  if (stream->srtt) {
    pto = stream->srtt * 2;
    if (stream->inflight_queue.len == 1) {
      pto += UDX_TLP_MAX_ACK_DELAY;
    }
  }

  // clamp pto
  if (uv_timer_get_due_in(&stream->rto_timer) < pto) {
    pto = uv_timer_get_due_in(&stream->rto_timer);
  }

  uv_timer_start(&stream->tlp_and_keepalive_timer, udx_tlp_timeout, pto, 0);
}

static uint32_t
rack_detect_loss (udx_stream_t *stream) {
  uint64_t timeout = 0;
  uint32_t reo_wnd = rack_update_reo_wnd(stream);
  uint64_t now = uv_now(stream->udx->loop);

  int resending = 0;
  int mtu_probes_lost = 0;
  udx_queue_node_t *p = NULL;
  udx_queue_node_t *next = NULL; // save p->next so that we can remove it without breaking iteration

  // debug_print_outgoing(stream);

  // can't use udx__queue_foreach because we may delete nodes in the middle
  for (p = stream->inflight_queue.node.next, next = p->next;
       p != &stream->inflight_queue.node;
       p = next, next = p->next) {
    udx_packet_t *pkt = udx__queue_data(p, udx_packet_t, queue);
    assert(pkt->transmits > 0);

    if (pkt->time_sent > stream->rack_time_sent) {
      break;
    }

    // hack - skip the packet if it is already queued for transmission
    // by checking ref_count == 2. This is done to prevent de-queueing
    // and re-queueing packets that are timing out before being transmitted.
    // this can happen in tests on localhost where rtt is 0.
    if (pkt->ref_count == 2) continue;

    if (rack_sent_after(stream->rack_time_sent, stream->rack_next_seq, pkt->time_sent, pkt->seq + 1)) {

      int64_t remaining = pkt->time_sent + stream->rack_rtt + reo_wnd - now;

      if (remaining <= 0) {
        pkt->lost = true;
        stream->lost++;

        assert(pkt->size > 0 && pkt->size < 1500);
        stream->inflight -= pkt->size;

        udx__queue_unlink(&stream->inflight_queue, &pkt->queue);
        udx__queue_tail(&stream->retransmit_queue, &pkt->queue);

        if (pkt->is_mtu_probe) {
          mtu_unprobeify_packet(pkt, stream);
          mtu_probes_lost++;
        }

        resending++;

      } else if ((uint64_t) remaining > timeout) {
        timeout = remaining;
      }
    }
  }

  if (resending > mtu_probes_lost && stream->ca_state == UDX_CA_OPEN) {
    // debug_printf("rack: rid=%u lost=%d mtu_probe_lost=%d\n", stream->remote_id, resending, mtu_probes_lost);
    // debug_print_outgoing(stream);

    stream->fast_recovery_count++;

    // recover until the full window is acked
    stream->ca_state = UDX_CA_RECOVERY;
    stream->high_seq = stream->seq;
    // rack 7.1 TLP_init
    stream->tlp_in_flight = false;
    stream->tlp_is_retrans = false;

    // debug_printf("rack: fast recovery rid=%u start=[%u:%u] (%u pkts) inflight=%zu cwnd=%u srtt=%u\n", stream->remote_id, stream->remote_acked, stream->seq, stream->seq - stream->remote_acked, stream->inflight, stream->cwnd, stream->srtt);
  }

  send_packets(stream);

  return timeout;
}

static void
rack_detect_loss_and_arm_timer (uv_timer_t *timer) {
  udx_stream_t *stream = timer->data;
  uint32_t timeout = rack_detect_loss(stream);

  if (timeout > 0) {
    assert(!(stream->status & UDX_STREAM_CLOSED));
    uv_timer_start(&stream->rack_reo_timer, rack_detect_loss_and_arm_timer, timeout, 0);
  }
}

static void
udx_zwp_timeout (uv_timer_t *timer) {
  udx_stream_t *stream = timer->data;
  assert(stream->status & UDX_STREAM_CONNECTED);
  assert(stream->send_rwnd == 0);
  assert((stream->status & UDX_STREAM_CLOSED) == 0);

  stream->zwp_count++;
  debug_printf("zwp: stream=%u\n", stream->remote_id);
  send_new_packet(stream, UDX_PROBE_TYPE_ZWP);
}

static void
udx_rto_timeout (uv_timer_t *timer) {
  udx_stream_t *stream = timer->data;
  assert(stream->status & UDX_STREAM_CONNECTED);
  assert(stream->remote_acked != stream->seq);

  // exit fast recovery if we are in it
  stream->high_seq = stream->seq;
  stream->rto_count++;
  stream->ca_state = UDX_CA_LOSS;

  // rack 7.1 TLP_init
  stream->tlp_in_flight = false;
  stream->tlp_is_retrans = false;

  assert(!(stream->status & UDX_STREAM_CLOSED));
  uv_timer_start(&stream->rto_timer, udx_rto_timeout, stream->rto * 2, 0);

  // zero retransmit queue
  udx__queue_init(&stream->retransmit_queue);

  debug_printf("rto: lost rid=%u [%u:%u] inflight=%zu cwnd=%u srtt=%u\n", stream->remote_id, stream->remote_acked, stream->seq, stream->inflight, stream->cwnd, stream->srtt);

  uint64_t now = uv_now(timer->loop);
  uint32_t rack_reo_wnd = rack_update_reo_wnd(stream);

  // rack 6.3

  for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&stream->outgoing, seq);
    if (pkt == NULL) continue;

    if (pkt->lost) {
      udx__queue_tail(&stream->retransmit_queue, &pkt->queue);
      continue;
    }

    int64_t remaining = pkt->time_sent + stream->rack_rtt + rack_reo_wnd - now;

    if (pkt->seq == stream->remote_acked || remaining < 0) {
      if (pkt->rto_timeouts >= UDX_MAX_RTO_TIMEOUTS) {
        close_stream(stream, UV_ETIMEDOUT);
        break;
      }

      stream->lost++;
      pkt->rto_timeouts++;

      if (pkt->is_mtu_probe) {
        mtu_unprobeify_packet(pkt, stream);
      }

      if (pkt->ref_count == 2) continue; // packet is already scheduled to transmit, so don't reschedule

      pkt->lost = true;
      udx__queue_unlink(&stream->inflight_queue, &pkt->queue);
      udx__queue_tail(&stream->retransmit_queue, &pkt->queue);

      stream->inflight -= pkt->size;
    }
  }

  bbr_on_rto(stream);
  send_packets(stream);
}

static void
rack_detect_loss_and_arm_timer (uv_timer_t *timer);

// processing packets after waking from suspend may result in
// spurious RTT values, where the RTT value includes the time spent suspended.
// to prevent extremely long RTO timeouts we heuristically
// clamp RTT samples over 5 seconds and over srtt + 5 * rttvar
// to min(srtt + 5 * rttvar, 30s)

static uint32_t
clamp_rtt (udx_stream_t *stream, uint64_t rtt) {
  // first sample special case, just clamp to max
  if (stream->srtt == 0) {
    return min_uint32(rtt, UDX_RTT_MAX_MS);
  }
  const uint32_t outlier_threshold = stream->srtt + 5 * stream->rttvar;
  if (rtt > outlier_threshold && rtt > 5000) {
    rtt = min_uint32(outlier_threshold, UDX_RTT_MAX_MS);
    debug_printf("rtt: clamp rtt for stream=%u to rtt=%" PRIu64 "\n", stream->remote_id, rtt);
  }

  return rtt;
}

// receiver side calls this to mark a packet acked
static int
ack_packet (udx_stream_t *stream, uint32_t seq, int sack, udx_rate_sample_t *rs) {
  udx_cirbuf_t *out = &(stream->outgoing);
  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(out, seq);

  if (pkt == NULL) {
    if (!sack) stream->sacks--; // packet not here, was sacked before
    return 0;
  }

  if (stream->mtu_state == UDX_MTU_STATE_SEARCH && stream->mtu_probe_count > 0 && pkt->is_mtu_probe) {
    // debug_printf("mtu: probe acked rid=%u seq=%u mtu=%d->%d sack=%d\n", stream->remote_id, seq, stream->mtu, stream->mtu_probe_size, sack);

    stream->mtu_probe_count = 0;
    stream->mtu = stream->mtu_probe_size;

    if (stream->mtu_probe_size == stream->mtu_max) {
      stream->mtu_state = UDX_MTU_STATE_SEARCH_COMPLETE;
    } else {
      stream->mtu_probe_size += UDX_MTU_STEP;
      if (stream->mtu_probe_size >= stream->mtu_max) {
        stream->mtu_probe_size = stream->mtu_max;
      }
      stream->mtu_probe_wanted = true;
    }
  }

  if (stream->mtu_state == UDX_MTU_STATE_BASE || stream->mtu_state == UDX_MTU_STATE_ERROR) {
    stream->mtu_state = UDX_MTU_STATE_SEARCH;
    stream->mtu_probe_wanted = true;
  }

  if (sack) {
    stream->sacks++;
  }

  if (pkt->lost) {
    udx__queue_unlink(&stream->retransmit_queue, &pkt->queue);
  } else {
    udx__queue_unlink(&stream->inflight_queue, &pkt->queue);
    stream->inflight -= pkt->size;
  }

  udx__rate_pkt_delivered(stream, pkt, rs);

  const uint64_t time = uv_now(stream->udx->loop);
  const uint32_t rtt = clamp_rtt(stream, time - pkt->time_sent);
  const uint32_t next = seq + 1;

  if (!pkt->retransmitted) {
    // rack 6.2 step 1 update rack.min_RTT
    win_filter_apply_min(&stream->rtt_min, UDX_RTT_MIN_WINDOW_MS, time, rtt);

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

    stream->tlp_permitted = true;

    // RTO <- SRTT + max (G, K*RTTVAR) where K is 4 maxed with 1s
    stream->rto = max_uint32(stream->srtt + 4 * stream->rttvar, 1000);

    if (stream->rto > UDX_RTO_MAX_MS) {
      debug_printf("rto: computed rto=%u ms, capping to %u ms\n", stream->rto, UDX_RTO_MAX_MS);
      stream->rto = UDX_RTO_MAX_MS;
    }
  }

  // rack 6.2 step 2 update the state for the most recently sent segment

  if (!pkt->retransmitted || (rtt >= udx_rtt_min(stream))) {
    stream->rack_rtt = rtt;
    rs->rtt_ms = rtt;

    if (rack_sent_after(pkt->time_sent, next, stream->rack_time_sent, stream->rack_next_seq)) {
      stream->rack_time_sent = pkt->time_sent;
      stream->rack_next_seq = next;
    }
  }

  // rack 6.2 step 3 detect data segment reordering
  if (seq_compare(next, stream->rack_fack) > 0) {
    stream->rack_fack = next;
  } else if (seq_compare(next, stream->rack_fack) < 0 && !pkt->retransmitted) {
    stream->reordering_seen = true;
  }

  uv_buf_t *bufs = pkt->bufs;
  udx_stream_write_buf_t **wbufs = pkt->wbufs;

  for (int i = 0; i < pkt->nwbufs; i++) {
    size_t pkt_len = bufs[i + 1].len;
    udx_stream_write_buf_t *wbuf = wbufs[i];

    on_bytes_acked(wbuf, pkt_len, false);

    udx_stream_write_t *write = wbuf->write;

    if (write->bytes_acked == write->size && write->on_ack) {
      write->on_ack(write, 0, sack);

      // reentry from write->on_ack
      if (stream->status & UDX_STREAM_DEAD) {
        deref_packet(pkt);
        return 2;
      }
    }
  }

  deref_packet(pkt);

  // TODO: the end condition needs work here to be more "stateless"
  // ie if the remote has acked all our writes, then instead of waiting for retransmits, we should
  // clear those and mark as local ended NOW.
  if ((stream->status & UDX_STREAM_SHOULD_END) == UDX_STREAM_END && stream->inflight_queue.len == 0 && stream->retransmit_queue.len == 0 && stream->write_queue.len == 0) {
    stream->status |= UDX_STREAM_ENDED;
    return 2;
  }

  return 1;
}

static void
process_data_packet (udx_stream_t *stream, int type, uint32_t seq, char *data, ssize_t data_len) {
  if (seq == stream->ack && type & UDX_HEADER_DATA) {
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
relay_packet (udx_stream_t *stream, char *buf, ssize_t buf_len, int type, uint32_t seq) {

  stream->seq = seq_max(stream->seq, seq);

  udx_stream_t *relay = stream->relay_to;

  if (relay->socket != NULL) {

    uint32_t *h = (uint32_t *) buf;
    h[1] = udx__swap_uint32_if_be(relay->remote_id);

    uv_buf_t b = uv_buf_init(buf, buf_len);

    int err;

    if (stream->udx->debug_flags & UDX_DEBUG_FORCE_RELAY_SLOW_PATH) {
      err = UV_EAGAIN;
    } else {
      err = uv_udp_try_send(&stream->socket->uv_udp, &b, 1, (struct sockaddr *) &relay->remote_addr);
    }

    if (err == UV_EAGAIN) {
      // create a socket_send_t with no callback to send this packet on the relay's send_queue

      uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t) + b.len);

      char *data = (char *) (req + 1);
      memcpy(data, buf, buf_len);
      b = uv_buf_init(data, b.len);

      err = uv_udp_send(req, &stream->socket->uv_udp, &b, 1, (struct sockaddr *) &relay->remote_addr, on_packet_send_slow);
    }
  }

  if (type & UDX_HEADER_DESTROY) {
    close_stream(stream, UV_ECONNRESET);
  }

  return 1;
}

// rack 7.4.2
static void
detect_loss_repaired_by_loss_probe (udx_stream_t *stream, uint32_t ack) {
  if (stream->tlp_in_flight && seq_compare(ack, stream->tlp_end_seq) >= 0) {
    if (!stream->tlp_is_retrans) {
      stream->tlp_in_flight = false;
    } else if (seq_compare(ack, stream->tlp_end_seq) > 0) {
      debug_printf("tlp: loss probe retransmission masked lost packet, invoking congestion control\n");
      stream->tlp_in_flight = false;
    }
  }
}

static int
process_packet (udx_socket_t *socket, char *buf, ssize_t buf_len, struct sockaddr *addr) {
  udx_t *udx = socket->udx;

  socket->bytes_rx += buf_len;
  socket->packets_rx += 1;

  udx->bytes_rx += buf_len;
  udx->packets_rx += 1;

  if (!(udx->has_streams) || buf_len < UDX_HEADER_SIZE) return 0;

  uint8_t *b = (uint8_t *) buf;

  if ((*(b++) != UDX_MAGIC_BYTE) || (*(b++) != UDX_VERSION)) return 0;

  int type = (int) *(b++);
  uint8_t data_offset = *(b++);

  uint32_t *i = (uint32_t *) b;

  uint32_t local_id = udx__swap_uint32_if_be(*(i++));
  uint32_t rwnd = udx__swap_uint32_if_be(*(i++));
  uint32_t seq = udx__swap_uint32_if_be(*(i++));
  uint32_t ack = udx__swap_uint32_if_be(*i++);

  uint32_t *sacks = i;
  int nsack_blocks = 0;

  if (type & UDX_HEADER_SACK) {
    size_t payload_len = buf_len - UDX_HEADER_SIZE;
    size_t header_len = (data_offset > 0 && data_offset < payload_len) ? data_offset : payload_len;
    nsack_blocks = header_len / (2 * sizeof(*sacks));
  }

  udx_stream_t *stream = (udx_stream_t *) udx__cirbuf_get(socket->streams_by_id, local_id);

  if (stream == NULL || stream->status & UDX_STREAM_DEAD) return 0;

  stream->bytes_rx += buf_len;
  stream->packets_rx += 1;

  // We expect this to be a stream packet from now on
  if (stream->socket != socket && stream->on_firewall != NULL) {
    if (is_addr_v4_mapped((struct sockaddr *) addr)) {
      addr_to_v4((struct sockaddr_in6 *) addr);
    }

    if (stream->on_firewall(stream, socket, addr)) return 1;
    if (stream->status & UDX_STREAM_DEAD) return 1;
  }

  if (stream->relay_to) return relay_packet(stream, buf, buf_len, type, seq);

  // start ack code

  uint32_t delivered = stream->delivered;
  uint32_t lost = stream->lost;
  uint32_t prior_remote_acked = stream->remote_acked;
  bool ack_advanced = seq_diff(ack, prior_remote_acked) > 0;
  bool data_inflight = stream->remote_acked != stream->seq;
  // todo: send data packet with seq=remote_acked-1
  bool is_probe = type & UDX_HEADER_HEARTBEAT;

  if (is_probe) {
    send_ack(stream);
    return 1;
  }

  buf += UDX_HEADER_SIZE;
  buf_len -= UDX_HEADER_SIZE;

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
      if (stream->status & UDX_STREAM_DEAD) {
        return 1; // re-entry on read callback
      }
    }

    if (type & UDX_HEADER_END) {
      stream->status |= UDX_STREAM_ENDING_REMOTE;
      stream->remote_ended = seq;
    }

    if (type & UDX_HEADER_DESTROY) {
      close_stream(stream, UV_ECONNRESET);
      return 1;
    }
  }

  if (type & UDX_HEADER_MESSAGE) {
    if (stream->on_recv != NULL) {
      uv_buf_t b = uv_buf_init(buf, buf_len);
      stream->on_recv(stream, buf_len, &b);
      if (stream->status & UDX_STREAM_DEAD) {
        return 1;
      }
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
      if (stream->status & UDX_STREAM_DEAD) {
        free(pkt);
        return 1;
      }
    }

    free(pkt);
  }

  // Check if the ack is oob.
  // could also check ack > remote_acked - send_window
  if (seq_compare(stream->seq, ack) < 0) {
    return 1;
  }

  if (seq_compare(ack, stream->remote_acked) >= 0) {
    if (seq_compare(stream->send_wl1, seq) < 0 || (stream->send_wl1 == seq && seq_compare(stream->send_wl2, ack) <= 0)) {
      // update send window
      if (rwnd > 0) {
        uv_timer_stop(&stream->zwp_timer);
      }
      stream->send_rwnd = rwnd;
      stream->send_wl1 = seq;
      stream->send_wl2 = ack;

      if (rwnd == 0) {
        uv_timer_start(&stream->zwp_timer, udx_zwp_timeout, stream->rto, 0);
      }
    }
  }

  if (stream->remote_changing && seq_diff(ack, stream->seq_on_remote_changed) >= 0) {
    debug_printf("remote_change: packets to old remote acked. ack=%u, last=%u, seq_diff=%d\n", ack, stream->seq_on_remote_changed, seq_diff(ack, stream->seq_on_remote_changed));
    stream->remote_changing = false;
    if (stream->on_remote_changed) {
      stream->on_remote_changed(stream);
      if (stream->status & UDX_STREAM_DEAD) return 1;
    }
  }

  // rack 7.4.2
  if (stream->tlp_in_flight) {
    detect_loss_repaired_by_loss_probe(stream, ack);
  }

  bool is_limited = stream->ca_state == UDX_CA_RECOVERY || stream->ca_state == UDX_CA_LOSS;

  if (seq_compare(ack, stream->high_seq) > 0 && (stream->ca_state == UDX_CA_RECOVERY || stream->ca_state == UDX_CA_LOSS)) {
    if (stream->ca_state == UDX_CA_RECOVERY) {
      stream->cwnd = stream->ssthresh;
    }
    stream->ca_state = UDX_CA_OPEN;
  }

  bool ended = false;

  udx_rate_sample_t rs = {.rtt_ms = -1};

  for (uint32_t p = prior_remote_acked; seq_compare(p, ack) < 0; p++) {
    int a = ack_packet(stream, p, 0, &rs);
    if (a == 1) stream->delivered++;
    if (a == 2) {
      ended = true;
      break;
    }
  }

  if (ack_advanced) {
    stream->remote_acked = ack;
  }

  if (ended) {
    if (stream->status & UDX_STREAM_DEAD) {
      return 1;
    }

    if (stream->remote_acked == stream->seq) {
      uv_timer_stop(&stream->rto_timer);

      if (stream->keepalive_timeout_ms) {
        uv_timer_start(&stream->tlp_and_keepalive_timer, udx_keepalive_timeout, stream->keepalive_timeout_ms, 0);
      } else {
        uv_timer_stop(&stream->tlp_and_keepalive_timer);
      }
    }

    if ((stream->status & UDX_STREAM_ALL_ENDED) == UDX_STREAM_ALL_ENDED) {
      close_stream(stream, 0);
      return 1;
    }

    // send a final state packet to make sure we've acked the end packet
    send_ack(stream);
    return 1;
  }

  // process sacks
  for (int i = 0; i < nsack_blocks; i++) {
    uint32_t start = udx__swap_uint32_if_be(*sacks++);
    uint32_t end = udx__swap_uint32_if_be(*sacks++);

    for (uint32_t p = start; p != end; p++) {
      int a = ack_packet(stream, p, 1, &rs);
      if (a == 2) break;
      if (a == 1) stream->delivered++;
    }
  }

  if (stream->status & UDX_STREAM_DEAD) {
    return 1; /* re-entry check */
  }

  // we are user limited if queued bytes (that includes current inflight + a max packet) is less than the window
  // we are rwnd limited if rwnd < cwnd
  if (!is_limited) is_limited = stream->writes_queued_bytes + udx__max_payload(stream) < cwnd_in_bytes(stream) || send_rwnd_in_packets(stream) < stream->cwnd;

  delivered = stream->delivered - delivered;
  lost = stream->lost - lost;

  if (ack_advanced) {
    // rack 7.2
    if (stream->ca_state == UDX_CA_OPEN && !stream->sacks) {
      schedule_loss_probe(stream);
    }
  }

  if (delivered > 0) {
    if (stream->remote_acked == stream->seq) {
      assert(stream->inflight_queue.len == 0 && stream->retransmit_queue.len == 0);
      uv_timer_stop(&stream->rto_timer);

      if (stream->keepalive_timeout_ms) {
        uv_timer_start(&stream->tlp_and_keepalive_timer, udx_keepalive_timeout, stream->keepalive_timeout_ms, 0);
      } else {
        uv_timer_stop(&stream->tlp_and_keepalive_timer);
      }
    } else {
      assert(!(stream->status & UDX_STREAM_CLOSED));
      uv_timer_start(&stream->rto_timer, udx_rto_timeout, stream->rto, 0);
    }

    // rack 6.2.5
    rack_detect_loss_and_arm_timer(&stream->rack_reo_timer);
  }

  if (type & UDX_HEADER_DATA_OR_END) {
    send_ack(stream);
  }

  if (data_inflight) {
    // don't generate rates / do congestion control if nothing was in flight, and thus nothing could be acked and no new samples are generated
    udx__rate_gen(stream, delivered, lost, &rs);
    bbr_main(stream, &rs);
  }

  send_packets(stream);

  return 1;
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
update_pacing_time (udx_stream_t *stream) {
  uint64_t now = uv_now(stream->udx->loop); // 1ms granularity

  if (now > stream->tb_last_refill_ms) {
    uint64_t factor = now - stream->tb_last_refill_ms;
    assert(stream->pacing_bytes_per_ms > 0);
    stream->tb_available = factor * stream->pacing_bytes_per_ms;
    stream->tb_last_refill_ms = now;
  }
}

void
pacing_timer_timeout (uv_timer_t *timer) {
  udx_stream_t *stream = timer->data;

  update_pacing_time(stream);
  send_packets(stream);
}

// arms the retransmit timers (RTO, TLP, ZWP), called after data is transmitted
// or retransmitted.
static void
arm_stream_timers (udx_stream_t *stream, bool sent_tlp) {
  assert(stream->inflight_queue.len > 0);
  assert(stream->remote_acked != stream->seq);

  if (!uv_is_active((uv_handle_t *) &stream->rto_timer)) {
    assert(stream->rto >= 1);
    assert(stream->status != UDX_STREAM_CLOSED);
    uv_timer_start(&stream->rto_timer, udx_rto_timeout, stream->rto, 0);
  }

  // rack 7.2 rearm tlp timer

  if (stream->ca_state != UDX_CA_OPEN || stream->sacks) {
    uv_timer_stop(&stream->tlp_and_keepalive_timer);
  } else {
    if (!sent_tlp) {
      schedule_loss_probe(stream);
    }
  }

  if (stream->send_rwnd == 0) {
    uv_timer_start(&stream->zwp_timer, udx_zwp_timeout, stream->rto, 0);
  } else {
    uv_timer_stop(&stream->zwp_timer);
  }
}

// our uv_udp_recv_cb
static void
on_uv_udp_recv (uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
  if (nread == 0 && addr == NULL) return;

  udx_socket_t *socket = handle->data; // todo: cast instead, save a dereference ?

  assert(!(socket->status & UDX_SOCKET_CLOSED));

  if (flags & UV_UDP_PARTIAL) {
    assert(false && "todo: log error for large messages?");
  }

  assert((size_t) nread <= buf->len);
  if (!process_packet(socket, buf->base, nread, (struct sockaddr *) addr) && socket->on_recv) {
    if (is_addr_v4_mapped((struct sockaddr *) addr)) {
      addr_to_v4((struct sockaddr_in6 *) addr);
    }
    uv_buf_t copy = *buf; // current API returns the number of valid bytes in buf->len
    copy.len = nread;     // and we can't modify the buf libuv returns without discarding const
                          // this is kind of ugly too but it works with the current test & bindings
    socket->on_recv(socket, nread, &copy, (struct sockaddr *) addr);
  }
}

int
udx_init (uv_loop_t *loop, udx_t *udx, udx_idle_cb on_idle) {
  udx->refs = 0;
  udx->teardown = false;
  udx->has_streams = false;
  udx->on_idle = on_idle;

  udx->sockets = NULL;
  udx->streams = NULL;
  udx->listeners = NULL;

  udx->bytes_rx = 0;
  udx->bytes_tx = 0;
  udx->packets_rx = 0;
  udx->packets_tx = 0;

  udx->packets_dropped_by_kernel = -1;
  udx->loop = loop;

  udx->debug_flags = 0;

  return 0;
}

void
udx_idle (udx_t *udx, udx_idle_cb cb) {
  udx->on_idle = cb;
}

int
udx_is_idle (udx_t *udx) {
  return udx->refs == 0;
}

void
udx_teardown (udx_t *udx) {
  udx->teardown = true;

  udx_socket_t *socket;
  udx_stream_t *stream;
  udx_interface_event_t *listener;

  udx__link_foreach(udx->sockets, socket) {
    if (socket->streams == NULL) {
      udx_socket_close(socket);
      continue;
    }

    udx__link_foreach(socket->streams, stream) {
      udx_stream_destroy(stream);
    }
  }

  udx__link_foreach(udx->streams, stream) {
    udx_stream_destroy(stream);
  }

  udx__link_foreach(udx->listeners, listener) {
    udx_interface_event_close(listener);
  }
}

int
udx_socket_init (udx_t *udx, udx_socket_t *socket, udx_socket_close_cb cb) {
  if (udx->teardown) return UV_EINVAL;

  udx->refs++;

  memset(socket, 0, sizeof(*socket));
  udx__link_add(udx->sockets, socket);

  socket->ttl = UDX_DEFAULT_TTL;
  udx__queue_init(&socket->specific_ttl_send_queue);

  socket->udx = udx;
  socket->streams_by_id = &(udx->streams_by_id);

  socket->on_recv = NULL;
  socket->on_close = cb;

  socket->bytes_rx = 0;
  socket->bytes_tx = 0;
  socket->packets_rx = 0;
  socket->packets_tx = 0;

  socket->packets_dropped_by_kernel = -1;
  uv_udp_t *handle = &socket->uv_udp;

  // Asserting all the errors here as it massively simplifies error handling.
  // In practice these will never fail.

  int err = uv_udp_init(udx->loop, handle);
  assert(err == 0);

  handle->data = socket;

  return err;
}

int
udx_socket_get_send_buffer_size (udx_socket_t *socket, int *value) {
  *value = 0;
  return uv_send_buffer_size((uv_handle_t *) &socket->uv_udp, value);
}

int
udx_socket_set_send_buffer_size (udx_socket_t *socket, int value) {
  if (value < 1) return UV_EINVAL;
  return uv_send_buffer_size((uv_handle_t *) &socket->uv_udp, &value);
}

int
udx_socket_get_recv_buffer_size (udx_socket_t *socket, int *value) {
  *value = 0;
  return uv_recv_buffer_size((uv_handle_t *) &socket->uv_udp, value);
}

int
udx_socket_set_recv_buffer_size (udx_socket_t *socket, int value) {
  if (value < 1) return UV_EINVAL;
  return uv_recv_buffer_size((uv_handle_t *) &socket->uv_udp, &value);
}

int
udx_socket_get_ttl (udx_socket_t *socket, int *ttl) {
  *ttl = socket->ttl;
  return 0;
}

int
udx_socket_set_ttl (udx_socket_t *socket, int ttl) {
  if (ttl < 1 || ttl > 255) return UV_EINVAL;
  socket->ttl = ttl;
  return uv_udp_set_ttl((uv_udp_t *) &socket->uv_udp, ttl);
}

static void
receive_alloc (uv_handle_t *uv_udp, size_t suggested_size, uv_buf_t *buf) {
  UDX_UNUSED(suggested_size);
  udx_socket_t *socket = (udx_socket_t *) ((char *) uv_udp - offsetof(udx_socket_t, uv_udp)); // todo: container_of macro

  buf->base = (char *) socket->buffer;
  buf->len = sizeof(socket->buffer);
}

int
udx_socket_bind (udx_socket_t *socket, const struct sockaddr *addr, unsigned int flags) {
  uv_udp_t *uv_udp = &socket->uv_udp;
  uv_os_fd_t fd;

  if (addr->sa_family == AF_INET) {
    socket->family = 4;
  } else if (addr->sa_family == AF_INET6) {
    socket->family = 6;
  } else {
    return UV_EINVAL;
  }

  // This might actually fail in practice, so
  int err = uv_udp_bind(uv_udp, addr, flags);
  if (err) return err;

  // Asserting all the errors here as it massively simplifies error handling
  // and in practice non of these will fail, as all our handles are valid and alive.

  err = uv_udp_set_ttl(uv_udp, socket->ttl);
  assert(err == 0);

  int sndbuf_size = UDX_DEFAULT_SNDBUF_SIZE;
  err = uv_send_buffer_size((uv_handle_t *) uv_udp, &sndbuf_size);
  assert(err == 0);

  // setting SO_RCVBUF
  // on MacOS setsockopt() fails if the user requests more memory than can be allocated;
  // to accomodate this, we try setting decreasing buffer sizes until we succeed.
  // on other platforms setsockopt() may succeed even if the full amount of the requested
  // memory can't be allocated, which is fine.

  int buffer_sizes[] = {
    1024 * 1024, // 1MB
    512 * 1024,  // 512k
    256 * 1024,  // 256k
    208 * 1024   // 212k this old maximum is known to work well
  };

  int rcvbuf_size = buffer_sizes[0];

  for (uint32_t i = 0; i < (sizeof(buffer_sizes) / sizeof(buffer_sizes[0])); i++) {
    rcvbuf_size = buffer_sizes[i];

    err = uv_recv_buffer_size((uv_handle_t *) uv_udp, &rcvbuf_size);
    if (err == 0) break;
  }

  assert(err == 0); // only asserts if we can't allocate 212k

  int actual_rcvbuf = 0;

  uv_recv_buffer_size((uv_handle_t *) uv_udp, &actual_rcvbuf);
  if (actual_rcvbuf < rcvbuf_size) {
    debug_printf("udx: SO_RCVBUF: less than requested. requested=%d allocated=%d\n", rcvbuf_size, actual_rcvbuf);
  }

  err = uv_fileno((const uv_handle_t *) uv_udp, &fd);
  assert(err == 0);

  err = udx__udp_set_dontfrag((uv_os_sock_t) fd, socket->family == 6);
  if (err) {
    debug_printf("udx: failed to set IP Don't Fragment socket option\n");
  }
  // should probably wait for udx_socket_recv_start / udx_socket_recv_stop
  // but this is how it works in main, receiving and discarding packets until recv_start is called

  return uv_udp_recv_start(&socket->uv_udp, receive_alloc, on_uv_udp_recv);
}

int
udx_socket_getsockname (udx_socket_t *socket, struct sockaddr *name, int *name_len) {
  return uv_udp_getsockname(&socket->uv_udp, name, name_len);
}

int
udx_socket_set_membership (udx_socket_t *socket, const char *multicast_addr, const char *interface_addr, uv_membership membership) {
  return uv_udp_set_membership(&socket->uv_udp, multicast_addr, interface_addr, membership);
}

int
udx_socket_set_source_membership (udx_socket_t *socket, const char *multicast_addr, const char *interface_addr, const char *source_addr, uv_membership membership) {
  return uv_udp_set_source_membership(&socket->uv_udp, multicast_addr, interface_addr, source_addr, membership);
}

int
udx_socket_set_multicast_loop (udx_socket_t *socket, int on) {
  return uv_udp_set_multicast_loop(&socket->uv_udp, on);
}

int
udx_socket_set_multicast_interface (udx_socket_t *socket, const char *addr) {
  return uv_udp_set_multicast_interface(&socket->uv_udp, addr);
}

int
udx_socket_send (udx_socket_send_t *req, udx_socket_t *socket, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *dest, udx_socket_send_cb cb) {
  return udx_socket_send_ttl(req, socket, bufs, bufs_len, dest, 0, cb);
}

static void
on_socket_send_slow (uv_udp_send_t *_req, int status) {
  udx_socket_send_t *req = (udx_socket_send_t *) ((char *) _req - offsetof(udx_socket_send_t, uv_udp_send));

  udx_socket_t *socket = req->socket;
  // 1. if packet was sent with specifc ttl, remove it from queue and reset ttl
  if (req->ttl) {
    udx_socket_send_t *removed = udx__queue_data(udx__queue_shift(&socket->specific_ttl_send_queue), udx_socket_send_t, queue);
    assert(removed == req);
    // restore ttl after sending
    uv_udp_set_ttl(&socket->uv_udp, socket->ttl);
  }

  // 2. if next packet is also a specific ttl it will be re-set here
  maybe_adjust_ttl(_req->handle);
  if (req->on_send) {
    req->on_send(req, status);
  }
}

int
udx_socket_send_ttl (udx_socket_send_t *req, udx_socket_t *socket, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *dest, int ttl, udx_socket_send_cb cb) {
  if (ttl < 0 /* 0 is "default" */ || ttl > 255) return UV_EINVAL;

  req->ttl = ttl;
  req->on_send = cb;
  req->socket = socket;

  struct sockaddr_in6 dest6;
  if (socket->family == 6 && dest->sa_family == AF_INET) {
    memset(&dest6, 0, sizeof(dest6));
    dest6.sin6_family = AF_INET6;
    dest6.sin6_port = ((struct sockaddr_in *) dest)->sin_port;
#ifdef SIN6_LEN
    dest6.sin6_len = sizeof(struct sockaddr_in6);
#endif
    dest6.sin6_addr.s6_addr[10] = 0xff;
    dest6.sin6_addr.s6_addr[11] = 0xff;
    memcpy(&dest6.sin6_addr.s6_addr[12], &((struct sockaddr_in *) dest)->sin_addr, 4);

    dest = (struct sockaddr *) &dest6;
  }

  assert(bufs_len == 1);
  int err;

  // fast path
  if (socket->udx->debug_flags & UDX_DEBUG_FORCE_SEND_SLOW_PATH) {
    err = UV_EAGAIN;
  } else {
    if (ttl) uv_udp_set_ttl(&socket->uv_udp, ttl);
    err = uv_udp_try_send(&socket->uv_udp, bufs, bufs_len, dest);
    if (ttl) uv_udp_set_ttl(&socket->uv_udp, socket->ttl);
  }

  if (err >= 0 && req->on_send) {
    req->on_send(req, 0);
    return 0;
  }

  if (err == UV_EAGAIN) {
    // slow path
    if (ttl) {
      req->place_in_queue = socket->packets_sent_via_uv_send_queue + socket->uv_udp.send_queue_count;
      udx__queue_tail(&socket->specific_ttl_send_queue, &req->queue);
    }

    err = uv_udp_send(&req->uv_udp_send, &socket->uv_udp, bufs, bufs_len, dest, on_socket_send_slow);
    _maybe_adjust_ttl(socket); // edge case: queue was empty

    return err;
  }

  return err;
}

int
udx_socket_recv_start (udx_socket_t *socket, udx_socket_recv_cb cb) {
  if (socket->status & UDX_SOCKET_RECEIVING) return UV_EALREADY;

  socket->on_recv = cb;
  socket->status |= UDX_SOCKET_RECEIVING;

  return 0;
}

int
udx_socket_recv_stop (udx_socket_t *socket) {
  if ((socket->status & UDX_SOCKET_RECEIVING) == 0) return 0;

  socket->on_recv = NULL;
  socket->status &= ~UDX_SOCKET_RECEIVING;

  return 0;
}

int
udx_socket_close (udx_socket_t *socket) {
  if (socket->streams != NULL) return UV_EBUSY;

  socket->status |= UDX_SOCKET_CLOSED;

  uv_close((uv_handle_t *) &socket->uv_udp, on_uv_close);

  udx_t *udx = socket->udx;
  udx__link_remove(udx->sockets, socket);

  return 0;
}

// called on init and on_change_remote
// if we decide to repacketize the retransmit queue on whole-flight loss ("black hole detection"
// in the pmtud RFCs) then we should reset there too
static void
reset_mtu_state_machine (udx_stream_t *stream) {
  stream->mtu = UDX_MTU_BASE;
  stream->mtu_state = UDX_MTU_STATE_BASE;
  stream->mtu_probe_count = 0;
  stream->mtu_probe_size = UDX_MTU_BASE; // starts with first ack, counts as a confirmation of base
  stream->mtu_max = UDX_MTU_MAX;         // revised in connect()
}

int
udx_stream_init (udx_t *udx, udx_stream_t *stream, uint32_t local_id, udx_stream_close_cb close_cb, udx_stream_finalize_cb finalize_cb) {
  if (udx->teardown) return UV_EINVAL;

  memset(stream, 0, sizeof(*stream));

  udx->refs++;

  if (!(udx->has_streams)) {
    udx__cirbuf_init(&(udx->streams_by_id), 16);
    udx->has_streams = true;
  }

  udx__link_add(udx->streams, stream);

  stream->local_id = local_id;
  stream->ca_state = UDX_CA_OPEN;
  stream->udx = udx;
  reset_mtu_state_machine(stream);

  // initially stream is application limited, since we haven't
  // discovered a network limit.
  stream->app_limited = ~0;
  stream->rate_sample_is_app_limited = true;

  stream->rto = 1000;

  uv_timer_init(udx->loop, &stream->rto_timer);
  stream->rto_timer.data = stream;

  win_filter_reset(&stream->rtt_min, uv_now(udx->loop), ~0U);

  stream->tb_available = UDX_INIT_PACING_RATE;
  stream->tb_last_refill_ms = uv_now(udx->loop);

  uv_prepare_init(udx->loop, &stream->pending_packet_prepare);
  stream->pending_packet_prepare.data = stream;

  uv_timer_init(udx->loop, &stream->rack_reo_timer);
  stream->rack_reo_timer.data = stream;

  uv_timer_init(udx->loop, &stream->tlp_and_keepalive_timer);
  stream->tlp_and_keepalive_timer.data = stream;

  uv_timer_init(udx->loop, &stream->zwp_timer);
  stream->zwp_timer.data = stream;

  uv_timer_init(udx->loop, &stream->refill_pacing_timer);
  stream->refill_pacing_timer.data = stream;

  stream->nrefs = 6; // rack_reo_timer, tlp_timer, zwp_timer, refill_pacing_timer, rto_timer, pending_packet_prepare

  udx__queue_init(&stream->inflight_queue);
  udx__queue_init(&stream->retransmit_queue);
  udx__queue_init(&stream->write_queue);

  stream->ssthresh = 0xffff;
  stream->cwnd = 10;
  stream->recv_rwnd_max = UDX_DEFAULT_RWND_MAX;
  stream->send_rwnd = UDX_DEFAULT_RWND_MAX;
  stream->on_close = close_cb;
  stream->on_finalize = finalize_cb;

  bbr_init(stream);

  udx__cirbuf_init(&(stream->relaying_streams), 2);

  // Init stream write/read buffers
  udx__cirbuf_init(&(stream->outgoing), 16);
  udx__cirbuf_init(&(stream->incoming), 16);
  udx__queue_init(&stream->inflight_queue);
  udx__queue_init(&stream->retransmit_queue);

  // Add the socket to the active set
  udx__cirbuf_set(&(udx->streams_by_id), (udx_cirbuf_val_t *) stream);

  debug_throughput_init(stream);
  reset_next_packet(stream);

  return 0;
}

int
udx_stream_get_mtu (udx_stream_t *stream, uint16_t *mtu) {
  *mtu = stream->mtu;
  return 0;
}

int
udx_stream_get_seq (udx_stream_t *stream, uint32_t *seq) {
  *seq = stream->seq;
  return 0;
}

int
udx_stream_set_seq (udx_stream_t *stream, uint32_t seq) {
  stream->seq = seq;
  stream->remote_acked = seq;
  stream->send_wl2 = seq; // ensure the next ack will be a valid wl2
  return 0;
}

int
udx_stream_set_keepalive (udx_stream_t *stream, uint32_t keepalive_timeout_ms) {

  stream->keepalive_timeout_ms = keepalive_timeout_ms;

  if (stream->remote_acked == stream->seq && keepalive_timeout_ms && stream->status & UDX_STREAM_CONNECTED) {
    uv_timer_start(&stream->tlp_and_keepalive_timer, udx_keepalive_timeout, stream->keepalive_timeout_ms, 0);
  }

  return 0;
}

int
udx_stream_get_ack (udx_stream_t *stream, uint32_t *ack) {
  *ack = stream->ack;
  return 0;
}

int
udx_stream_set_ack (udx_stream_t *stream, uint32_t ack) {
  stream->ack = ack;
  stream->send_wl1 = ack; // ensure the next seq will be a valid wl1
  return 0;
}

int
udx_stream_get_rwnd_max (udx_stream_t *stream, uint32_t *size) {
  *size = stream->recv_rwnd_max;
  return 0;
}

int
udx_stream_set_rwnd_max (udx_stream_t *stream, uint32_t size) {
  stream->recv_rwnd_max = size;
  return 0;
}

int
udx_stream_firewall (udx_stream_t *stream, udx_stream_firewall_cb cb) {
  stream->on_firewall = cb;
  return 0;
}

int
udx_stream_recv_start (udx_stream_t *stream, udx_stream_recv_cb cb) {
  if (stream->status & UDX_STREAM_RECEIVING) return UV_EALREADY;

  stream->on_recv = cb;
  stream->status |= UDX_STREAM_RECEIVING;

  return 0;
}

int
udx_stream_recv_stop (udx_stream_t *stream) {
  if ((stream->status & UDX_STREAM_RECEIVING) == 0) return 0;

  stream->on_recv = NULL;
  stream->status ^= UDX_STREAM_RECEIVING;

  return 0;
}

int
udx_stream_read_start (udx_stream_t *stream, udx_stream_read_cb cb) {
  if (stream->status & UDX_STREAM_READING) return UV_EALREADY;

  stream->on_read = cb;
  stream->status |= UDX_STREAM_READING;

  return 0;
}

int
udx_stream_read_stop (udx_stream_t *stream) {
  if ((stream->status & UDX_STREAM_READING) == 0) return 0;

  stream->on_read = NULL;
  stream->status ^= UDX_STREAM_READING;

  return 0;
}

static void
set_stream_socket (udx_stream_t *stream, udx_socket_t *socket) {
  if (stream->socket == socket) return; // just in case

  udx_socket_t *prev = stream->socket;

  // technically its unsafe to remove and add it to another queue
  // if iterating the queue we removed from.
  if (prev == NULL) {
    udx_t *udx = stream->udx;
    udx__link_remove(udx->streams, stream);
  } else {
    udx__link_remove(prev->streams, stream);
  }

  stream->socket = socket;
  udx__link_add(socket->streams, stream);
}

int
udx_stream_change_remote (udx_stream_t *stream, udx_socket_t *socket, uint32_t remote_id, const struct sockaddr *remote_addr, udx_stream_remote_changed_cb on_remote_changed) {
  // the since the udx_t object stores streams_by_id, we cannot migrate streams across udx objects
  // the local id's of different udx streams may collide.
  assert(socket->udx == stream->socket->udx);

  if (stream->status & UDX_STREAM_DEAD || stream->udx->teardown) {
    return UV_EINVAL;
  }

  if (!(stream->status & UDX_STREAM_CONNECTED)) {
    return UV_EINVAL;
  }

  if (remote_addr->sa_family == AF_INET) {
    stream->remote_addr_len = sizeof(struct sockaddr_in);
    if (((struct sockaddr_in *) remote_addr)->sin_port == 0) {
      return UV_EINVAL;
    }
  } else if (remote_addr->sa_family == AF_INET6) {
    stream->remote_addr_len = sizeof(struct sockaddr_in6);
    if (((struct sockaddr_in6 *) remote_addr)->sin6_port == 0) {
      return UV_EINVAL;
    }
  } else {
    return UV_EINVAL;
  }

  memcpy(&stream->remote_addr, remote_addr, stream->remote_addr_len);

  if (stream->socket->family == 6 && stream->remote_addr.ss_family == AF_INET) {
    addr_to_v6((struct sockaddr_in *) &stream->remote_addr);
    stream->remote_addr_len = sizeof(struct sockaddr_in6);
  }

  stream->remote_id = remote_id;
  set_stream_socket(stream, socket);

  // defer changing the remote until all of our in-flight packets have been acked.

  uint32_t seq_of_change = stream->seq + ((stream->pkt_header_flag & UDX_HEADER_DATA) ? 1 : 0);

  bool defer_change = seq_of_change != stream->remote_acked;
  if (defer_change) {
    debug_printf("change_remote: id=%u RA=%u Seq Of Change=%u\n", stream->local_id, stream->remote_acked, stream->seq);
    stream->remote_changing = true;
    stream->seq_on_remote_changed = seq_of_change;
    stream->on_remote_changed = on_remote_changed;
  } else {
    debug_printf("change_remote: id=%u RA=%u Seq=%u, acting now!\n", stream->local_id, stream->remote_acked, stream->seq);
  }

  reset_mtu_state_machine(stream);

  return !defer_change;
}

int
udx_stream_connect (udx_stream_t *stream, udx_socket_t *socket, uint32_t remote_id, const struct sockaddr *remote_addr) {
  if (stream->status & UDX_STREAM_DEAD || stream->udx->teardown) {
    return UV_EINVAL;
  }

  if (stream->status & UDX_STREAM_CONNECTED) {
    return UV_EISCONN;
  }

  stream->status |= UDX_STREAM_CONNECTED;

  stream->remote_id = remote_id;
  set_stream_socket(stream, socket);

  if (remote_addr->sa_family == AF_INET) {
    stream->remote_addr_len = sizeof(struct sockaddr_in);
    if (((struct sockaddr_in *) remote_addr)->sin_port == 0) {
      return UV_EINVAL;
    }
  } else if (remote_addr->sa_family == AF_INET6) {
    stream->remote_addr_len = sizeof(struct sockaddr_in6);
    if (((struct sockaddr_in6 *) remote_addr)->sin6_port == 0) {
      return UV_EINVAL;
    }
  } else {
    return UV_EINVAL;
  }

  memcpy(&(stream->remote_addr), remote_addr, stream->remote_addr_len);

  if (socket->family == 6 && stream->remote_addr.ss_family == AF_INET) {
    addr_to_v6((struct sockaddr_in *) &(stream->remote_addr));
    stream->remote_addr_len = sizeof(struct sockaddr_in6);
  }

  int mtu = udx__get_link_mtu(remote_addr);

  if (mtu == -1 || mtu > UDX_MTU_MAX) {
    mtu = UDX_MTU_MAX;
  } else if (mtu <= UDX_MTU_BASE) {
    debug_printf("mtu: OS-Discovered pMTU to host is less than UDX_MTU_BASE (%u < %u), disabling MTU discovery\n", mtu, UDX_MTU_BASE);
    stream->mtu_state = UDX_MTU_STATE_SEARCH_COMPLETE;
  }

  stream->mtu_max = mtu;

  if (stream->ack_needed) {
    send_ack(stream);
    stream->ack_needed = false;
  }

  if (stream->keepalive_timeout_ms) {
    uv_timer_start(&stream->tlp_and_keepalive_timer, udx_keepalive_timeout, stream->keepalive_timeout_ms, 0);
  }

  return 0;
}

int
udx_stream_relay_to (udx_stream_t *stream, udx_stream_t *destination) {
  if (stream->relayed || (destination->status & UDX_STREAM_CLOSED) != 0) return UV_EINVAL;

  stream->relayed = true;
  stream->relay_to = destination;

  udx__cirbuf_set(&(destination->relaying_streams), (udx_cirbuf_val_t *) stream);

  return 0;
}

static void
on_stream_send_slow (uv_udp_send_t *send, int status) {
  UDX_UNUSED(status);
  udx_stream_send_t *req = (udx_stream_send_t *) send; // todo: container_of
  if (req->on_send) {
    req->on_send(req, 0);
  }
}

int
udx_stream_send (udx_stream_send_t *req, udx_stream_t *stream, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_send_cb cb) {
  UDX_UNUSED(bufs_len);

  if (!(stream->status & UDX_STREAM_CONNECTED)) {
    return UV_ENOTCONN;
  }

  assert(bufs_len == 1);

  req->stream = stream;
  req->on_send = cb;

  udx_write_header(req->header, stream, UDX_HEADER_MESSAGE);
  req->bufs[0].base = (char *) req->header;
  req->bufs[0].len = sizeof(req->header);
  req->bufs[1] = bufs[0];
  // does a fastpath make sense here? the slow path doesn't avoid an allocation here

  int err = uv_udp_try_send(&stream->socket->uv_udp, req->bufs, 2, (struct sockaddr *) &stream->remote_addr);

  if (err == UV_EAGAIN) {
    // slow path
    err = uv_udp_send(&req->uv_udp_send, &stream->socket->uv_udp, req->bufs, 2, (struct sockaddr *) &stream->remote_addr, on_stream_send_slow);
  } else {
    if (req->on_send) {
      req->on_send(req, 0);
    }
  }

  return err;
}

int
udx_stream_write_resume (udx_stream_t *stream, udx_stream_drain_cb drain_cb) {
  stream->on_drain = drain_cb;
  return 0;
}

static void
_udx_stream_write (udx_stream_write_t *write, udx_stream_t *stream, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb, bool is_write_end) {
  assert(bufs_len > 0);

  // initialize write object

  write->size = 0;
  write->bytes_acked = 0;
  write->is_write_end = is_write_end;
  write->stream = stream;
  write->on_ack = ack_cb;

  // consider:
  // make `udx_stream_write` re-entrant, allowing write to be called again to add more buffers to the same request object

  // can't create the buffers in a block because it is hard to determine where to free them:
  // the write request object is owned by the user, we can't hook into it's destruction
  // the stream_t object could hold writes but they may grow indefinitely

  for (unsigned int i = 0; i < bufs_len; i++) {
    udx_stream_write_buf_t *wbuf = &write->wbuf[i];

    wbuf->buf = bufs[i];
    wbuf->bytes_inflight = 0;
    wbuf->bytes_acked = 0;
    wbuf->write = write;
    wbuf->is_write_end = false;

    write->size += bufs[i].len;
    stream->writes_queued_bytes += bufs[i].len;

    if (is_write_end && i == bufs_len - 1) {
      wbuf->is_write_end = true;
    }
    udx__queue_tail(&stream->write_queue, &wbuf->queue);
  }

  // if an idle, zero window stream has data queued, send a zero-window probe immediately
  if (stream->writes_queued_bytes > 0 && stream->send_rwnd == 0) {
    send_new_packet(stream, UDX_PROBE_TYPE_ZWP);
  }
  send_packets(stream);
}

int
udx_stream_write (udx_stream_write_t *req, udx_stream_t *stream, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb) {
  if (!(stream->status & UDX_STREAM_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (stream->status & UDX_STREAM_ENDING) {
    return UV_EPIPE;
  }

  if (bufs_len == 0) {
    return UV_EINVAL;
  }

  req->nwbufs = bufs_len;

  _udx_stream_write(req, stream, bufs, bufs_len, ack_cb, false);

  if (stream->writes_queued_bytes > UDX_HIGH_WATERMARK + send_window_in_bytes(stream)) {
    stream->hit_high_watermark = true;
    return 0;
  }

  return 1;
}

int
udx_stream_write_end (udx_stream_write_t *req, udx_stream_t *stream, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb) {
  if (!(stream->status & UDX_STREAM_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (stream->status & UDX_STREAM_ENDING) {
    return UV_EPIPE;
  }

  stream->status |= UDX_STREAM_ENDING;

  if (bufs_len > 0) {
    req->nwbufs = bufs_len;
    _udx_stream_write(req, stream, bufs, bufs_len, ack_cb, true);
  } else {
    req->nwbufs = 1;
    uv_buf_t buf = uv_buf_init("", 0);
    _udx_stream_write(req, stream, &buf, 1, ack_cb, true);
  }

  if (stream->writes_queued_bytes > UDX_HIGH_WATERMARK + send_window_in_bytes(stream)) {
    stream->hit_high_watermark = true;
    return 0;
  }

  return 1;
}

static void
stream_on_destroy_send (udx_stream_t *stream) {
  stream->packets_tx++;
  stream->bytes_tx += UDX_HEADER_SIZE;

  udx_socket_t *socket = stream->socket;
  socket->packets_tx++;
  socket->bytes_tx += UDX_HEADER_SIZE;

  udx_t *udx = socket->udx;
  udx->packets_tx++;
  udx->bytes_tx += UDX_HEADER_SIZE;

  close_stream(stream, 0);
}

static void
_stream_on_destroy_send (uv_udp_send_t *req, int status) {
  if (status < 0) {
    debug_printf("udx destroy send: err=%s\n", uv_strerror(status));
  }
  udx_stream_t *stream = req->data;
  stream_on_destroy_send(stream);
  free(req);
}

int
udx_stream_destroy (udx_stream_t *stream) {
  if (stream->status & UDX_STREAM_CLOSED) {
    debug_printf("udx: closing already closed stream %u", stream->local_id);
    return 0;
  }

  if ((stream->status & UDX_STREAM_CONNECTED) == 0) {
    close_stream(stream, 0);
    return 0;
  }

  stream->status |= UDX_STREAM_DESTROYING;

  if (stream->relayed) {
    close_stream(stream, 0);
    return 0;
  }

  // write destroy packet

  alignas(4) uint8_t header[20];

  udx_write_header(header, stream, UDX_HEADER_DESTROY);
  stream->seq++;

  uv_buf_t buf = uv_buf_init((char *) header, sizeof(header));
  int err = uv_udp_try_send(&stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr);

  if (err == UV_EAGAIN) {
    // slow path
    uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t) + buf.len);
    char *data = (char *) (req + 1);
    memcpy(data, buf.base, buf.len);
    buf.base = data;
    req->data = stream;
    err = uv_udp_send(req, &stream->socket->uv_udp, &buf, 1, (struct sockaddr *) &stream->remote_addr, _stream_on_destroy_send);
  } else {
    if (err < 0) debug_printf("uv_udp_send: error=%s\n", uv_strerror(err));
    stream_on_destroy_send(stream);
  }

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

  ref_dec(lookup->udx);
}

int
udx_lookup (udx_t *udx, udx_lookup_t *req, const char *host, unsigned int flags, udx_lookup_cb cb) {
  if (udx->teardown) return UV_EINVAL;

  udx->refs++;

  req->udx = udx;
  req->on_lookup = cb;
  req->req.data = req;

  memset(&req->hints, 0, sizeof(struct addrinfo));

  int family = AF_UNSPEC;

  if (flags & UDX_LOOKUP_FAMILY_IPV4) family = AF_INET;
  if (flags & UDX_LOOKUP_FAMILY_IPV6) family = AF_INET6;

  req->hints.ai_family = family;
  req->hints.ai_socktype = SOCK_STREAM;

  return uv_getaddrinfo(udx->loop, &req->req, on_uv_getaddrinfo, host, NULL, &req->hints);
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

  udx_t *udx = event->udx;
  udx__link_remove(udx->listeners, event);

  if (event->on_close != NULL) {
    event->on_close(event);
  }

  ref_dec(event->udx);
}

int
udx_interface_event_init (udx_t *udx, udx_interface_event_t *handle, udx_interface_event_close_cb cb) {
  if (udx->teardown) return UV_EINVAL;

  handle->udx = udx;
  handle->loop = udx->loop;
  handle->sorted = false;
  handle->on_close = cb;

  int err = uv_interface_addresses(&(handle->addrs), &(handle->addrs_len));
  if (err < 0) return err;

  err = uv_timer_init(handle->loop, &(handle->timer));
  if (err < 0) return err;

  handle->timer.data = handle;

  udx->refs++;
  udx__link_add(udx->listeners, handle);

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
udx_interface_event_close (udx_interface_event_t *handle) {
  handle->on_event = NULL;

  uv_free_interface_addresses(handle->addrs, handle->addrs_len);

  int err = uv_timer_stop(&(handle->timer));
  if (err < 0) return err;

  uv_close((uv_handle_t *) &(handle->timer), on_interface_event_close);

  return 0;
}
