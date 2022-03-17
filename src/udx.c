#include <assert.h>
#include <string.h>

#include "../include/udx.h"

#include "cirbuf.h"
#include "fifo.h"
#include "io.h"

#define UDX_STREAM_ALL_DESTROYED (UDX_STREAM_DESTROYED | UDX_STREAM_DESTROYED_REMOTE)
#define UDX_STREAM_ALL_ENDED (UDX_STREAM_ENDED | UDX_STREAM_ENDED_REMOTE)
#define UDX_STREAM_DEAD (UDX_STREAM_ALL_DESTROYED | UDX_STREAM_DESTROYING | UDX_STREAM_CLOSED)

#define UDX_STREAM_SHOULD_READ (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD)
#define UDX_STREAM_READ 0

#define UDX_STREAM_SHOULD_END (UDX_STREAM_ENDING | UDX_STREAM_ENDED | UDX_STREAM_DEAD)
#define UDX_STREAM_END UDX_STREAM_ENDING

#define UDX_STREAM_SHOULD_END_REMOTE (UDX_STREAM_ENDED_REMOTE | UDX_STREAM_DEAD | UDX_STREAM_ENDING_REMOTE)
#define UDX_STREAM_END_REMOTE UDX_STREAM_ENDING_REMOTE

#define UDX_PACKET_CALLBACK (UDX_PACKET_STREAM_SEND | UDX_PACKET_STREAM_DESTROY | UDX_PACKET_SEND)
#define UDX_PACKET_FREE_ON_SEND (UDX_PACKET_STREAM_STATE | UDX_PACKET_STREAM_DESTROY)

#define UDX_HEADER_DATA_OR_END (UDX_HEADER_DATA | UDX_HEADER_END)

#define UDX_MAX_TRANSMITS 5

static uint32_t
random_id () {
  uint32_t id;
  uv_random(NULL, NULL, &id, sizeof(id), 0, NULL);
  return id;
}

static uint64_t
get_microseconds () {
  return uv_hrtime() / 1000;
}

static uint64_t
get_milliseconds () {
  return get_microseconds() / 1000;
}

static uint32_t
(max) (a, b) {
  return a < b ? b : a;
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events);

static void
on_uv_close (uv_handle_t *handle) {
  udx_t *self = (udx_t *) handle->data;

  if (--self->pending_closes == 0 && self->on_close != NULL) {
    self->on_close(self);
  }
}

static void
on_uv_interval (uv_timer_t *req) {
  udx_t *udx = req->data;
  udx_check_timeouts(udx);
}

static int
update_poll (udx_t *self) {
  int events = (self->send_queue.len > 0 ? UV_WRITABLE : 0) | (self->readers ? UV_READABLE : 0);
  if (events == self->events) return 0;

  self->events = events;
  return uv_poll_start(&(self->io_poll), events, on_uv_poll);
}

static void
init_stream_packet (udx_packet_t *pkt, int type, udx_stream_t *stream, const char *buf, size_t buf_len) {
  uint8_t *b = (uint8_t *) &(pkt->header);

  // 8 bit magic byte + 8 bit version + 8 bit type + 8 bit extensions
  *(b++) = UDX_MAGIC_BYTE;
  *(b++) = UDX_VERSION;
  *(b++) = (uint8_t) type;
  *(b++) = 0; // data offset

  uint32_t *i = (uint32_t *) b;

  // TODO: the header is ALWAYS little endian, make this work on big endian archs also

  // 32 bit (le) remote id
  *(i++) = stream->remote_id;
  // 32 bit (le) recv window
  *(i++) = 0xffffffff; // hardcode max recv window
  // 32 bit (le) seq
  *(i++) = pkt->seq = stream->seq;
  // 32 bit (le) ack
  *(i++) = stream->ack;

  pkt->transmits = 0;
  pkt->size = (uint16_t) (UDX_HEADER_SIZE + buf_len);
  pkt->dest = stream->remote_addr;

  pkt->bufs_len = 2;

  pkt->bufs[0].base = &(pkt->header);
  pkt->bufs[0].len = UDX_HEADER_SIZE;

  pkt->bufs[1].base = (void *) buf;
  pkt->bufs[1].len = buf_len;
}

static int
send_state_packet (udx_stream_t *stream) {
  uint32_t *sacks = NULL;
  uint32_t start = 0;
  uint32_t end = 0;

  udx_packet_t *pkt = NULL;

  void *payload = NULL;
  size_t payload_len = 0;

  uint32_t max = 512;
  for (uint32_t i = 0; i < max && payload_len < 400; i++) {
    uint32_t seq = stream->ack + 1 + i;
    if (udx__cirbuf_get(&(stream->incoming), seq) == NULL) continue;

    if (sacks == NULL) {
      pkt = malloc(sizeof(udx_packet_t) + 1024);
      payload = (((char *) pkt) + sizeof(udx_packet_t));
      sacks = (uint32_t *) payload;
      start = seq;
      end = seq + 1;
    } else if (seq == end) {
      end++;
    } else {
      *(sacks++) = start;
      *(sacks++) = end;
      start = seq;
      end = seq + 1;
      payload_len += 8;
    }

    max = i + 512;
  }

  if (start != end) {
    *(sacks++) = start;
    *(sacks++) = end;
    payload_len += 8;
  }

  if (pkt == NULL) pkt = malloc(sizeof(udx_packet_t));

  init_stream_packet(pkt, payload ? UDX_HEADER_SACK : 0, stream, payload, payload_len);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_STATE;

  stream->stats_pkts_sent++;

  udx__fifo_push(&(stream->udx->send_queue), pkt);
  return update_poll(stream->udx);
}

static int
send_data_packet (udx_stream_t *stream, udx_packet_t *pkt) {
  if (stream->inflight + pkt->size > stream->cwnd) {
    return 0;
  }

  assert(pkt->status == UDX_PACKET_WAITING);

  pkt->status = UDX_PACKET_SENDING;

  stream->pkts_waiting--;
  stream->pkts_inflight++;
  stream->inflight += pkt->size;
  if (pkt->transmits > 0) stream->retransmits_waiting--;

  stream->stats_pkts_sent++;
  pkt->fifo_gc = udx__fifo_push(&(stream->udx->send_queue), pkt);

  int err = update_poll(stream->udx);
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

  udx_t *u = stream->udx;
  u->streams[stream->set_id] = u->streams[--(u->streams_len)];

  // TODO: Dealloc all remaning state such as
  // - pending reads
  // - destroy alloc'ed cirbufs
  // (anything else from stream init)

  udx__cirbuf_remove(&(stream->udx->streams_by_id), stream->local_id);
  // TODO: move the instance to a TIME_WAIT state, so we can handle retransmits

  if (stream->status & UDX_STREAM_CONNECTED) {
    // TODO: move this to read_stop
    stream->udx->readers--;
    update_poll(stream->udx);
  }

  if (stream->on_close != NULL) {
    stream->on_close(stream, err);
  }

  return 1;
}

static int
ack_packet (udx_stream_t *stream, uint32_t seq, int sack) {
  udx_cirbuf_t *out = &(stream->outgoing);
  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(out, seq);

  if (pkt == NULL) return 0;

  if (pkt->status == UDX_PACKET_INFLIGHT) {
    stream->pkts_inflight--;
    stream->inflight -= pkt->size;
  }

  if (pkt->transmits == 1) {
    const uint32_t rtt = (uint32_t) (get_milliseconds() - pkt->time_sent);

    // First round trip time sample
    if (stream->srtt == 0) {
      stream->srtt = rtt;
      stream->rttvar = rtt / 2;
      stream->rto = stream->srtt + max(UDX_CLOCK_GRANULARITY_MS, 4 * stream->rttvar);
    } else {
      const uint32_t delta = rtt < stream->srtt ? stream->srtt - rtt : rtt - stream->srtt;
      // RTTVAR <- (1 - beta) * RTTVAR + beta * |SRTT - R'| where beta is 1/4
      stream->rttvar = (3 * stream->rttvar + delta) / 4;

      // SRTT <- (1 - alpha) * SRTT + alpha * R' where alpha is 1/8
      stream->srtt = (7 * stream->srtt + rtt) / 8;
    }

    // RTO <- SRTT + max (G, K*RTTVAR) where K is 4 maxed with 1s
    stream->rto = max(stream->srtt + max(UDX_CLOCK_GRANULARITY_MS, 4 * stream->rttvar), 1000);
  }

  if (!sack) { // Reset rto timer when new data is ack'ed (inorder)
    stream->rto_timeout = get_milliseconds() + stream->rto;
  }

  udx_stream_write_t *w = (udx_stream_write_t *) pkt->ctx;

  // If this packet was queued for sending we need to remove it from the queue.
  if (pkt->status == UDX_PACKET_SENDING) {
    udx__fifo_remove(&(stream->udx->send_queue), pkt, pkt->fifo_gc);
  }

  free(pkt);

  if (--(w->packets) == 0) {
    if (stream->on_ack != NULL) {
      stream->on_ack(stream, w, 0, sack);
      if (stream->status & UDX_STREAM_DEAD) return 2;
    }

    // TODO: the end condition needs work here to be more "stateless"
    // ie if the remote has acked all our writes, then instead of waiting for retransmits, we should
    // clear those and mark as local ended NOW.
    if ((stream->status & UDX_STREAM_SHOULD_END) == UDX_STREAM_END && stream->pkts_waiting == 0 && stream->pkts_inflight == 0) {
      stream->status |= UDX_STREAM_ENDED;
      return 2;
    }
  }

  return 1;
}

static void
process_sacks (udx_stream_t *stream, char *buf, size_t buf_len) {
  uint32_t n = 0;
  uint32_t *sacks = (uint32_t *) buf;

  for (size_t i = 0; i + 8 <= buf_len; i += 8) {
    uint32_t start = *(sacks++);
    uint32_t end = *(sacks++);
    uint32_t len = end - start;

    for (uint32_t j = 0; j < len; j++) {
      int a = ack_packet(stream, start + j, 1);
      if (a == 2) return; // ended
      if (a == 1) {
        n++;
      }
    }
  }

  if (n) {
    stream->stats_sacks += n;
  }
}

static void
fast_retransmit (udx_stream_t *stream) {
  udx_cirbuf_t *out = &(stream->outgoing);
  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(out, stream->remote_acked);

  if (pkt == NULL || pkt->transmits != 1 || pkt->status != UDX_PACKET_INFLIGHT) return;

  pkt->status = UDX_PACKET_WAITING;

  stream->inflight -= pkt->size;
  stream->pkts_waiting++;
  stream->pkts_inflight--;
  stream->retransmits_waiting++;
  stream->stats_fast_rt++;

  // Shrink the window
  stream->cwnd = max(UDX_MTU, stream->cwnd / 2);
}

static void
clear_outgoing_packets (udx_stream_t *stream) {
  // We should make sure all existing packets do not send, and notify the user that they failed
  for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
    udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_remove(&(stream->outgoing), seq);

    if (pkt == NULL) continue;

    // Make sure to remove it from the fifo, if it was added
    if (pkt->status == UDX_PACKET_SENDING) {
      udx__fifo_remove(&(stream->udx->send_queue), pkt, pkt->fifo_gc);
    }

    udx_stream_write_t *w = (udx_stream_write_t *) pkt->ctx;

    if (--(w->packets) == 0 && stream->on_ack != NULL) {
      stream->on_ack(stream, w, 1, 0);
    }

    free(pkt);
  }
}

static int
process_packet (udx_t *self, char *buf, ssize_t buf_len) {
  if (buf_len < UDX_HEADER_SIZE) return 0;

  uint8_t *b = (uint8_t *) buf;

  if ((*(b++) != UDX_MAGIC_BYTE) || (*(b++) != UDX_VERSION)) return 0;

  int type = (int) *(b++);
  uint8_t data_offset = *(b++);

  uint32_t *i = (uint32_t *) b;

  uint32_t local_id = *(i++);
  uint32_t recv_win = *(i++);
  uint32_t seq = *(i++);
  uint32_t ack = *i;

  buf += UDX_HEADER_SIZE;
  buf_len -= UDX_HEADER_SIZE;

  udx_stream_t *stream = (udx_stream_t *) udx__cirbuf_get(&(self->streams_by_id), local_id);
  if (stream == NULL || stream->status & UDX_STREAM_DEAD) return 0;

  udx_cirbuf_t *inc = &(stream->incoming);

  if (type & UDX_HEADER_SACK) {
    process_sacks(stream, buf, buf_len);
  }

  if (type & UDX_HEADER_DATA_OR_END && udx__cirbuf_get(inc, seq) == NULL && (stream->status & UDX_STREAM_SHOULD_READ) == UDX_STREAM_READ) {
    // Copy over incoming buffer as we CURRENTLY do not own it (stack allocated upstream)
    // TODO: if this is the next packet we expect (it usually is!), then there is no need
    // for the malloc and memcpy - we just need a way to not free it then

    if (data_offset) {
      if (data_offset > buf_len) return 0;
      buf += data_offset;
      buf_len -= data_offset;
    }

    char *ptr = malloc(sizeof(udx_pending_read_t) + buf_len);

    udx_pending_read_t *pkt = (udx_pending_read_t *) ptr;
    char *cpy = ptr + sizeof(udx_pending_read_t);

    memcpy(cpy, buf, buf_len);

    pkt->seq = seq;
    pkt->buf.base = cpy;
    pkt->buf.len = buf_len;

    udx__cirbuf_set(inc, (udx_cirbuf_val_t *) pkt);
  }

  if (type & UDX_HEADER_END) {
    stream->status |= UDX_STREAM_ENDING_REMOTE;
    stream->remote_ended = seq;
  }

  if (type & UDX_HEADER_MESSAGE) {
    if (data_offset) {
      if (data_offset > buf_len) return 0;
      buf += data_offset;
      buf_len -= data_offset;
    }

    if (stream->on_message != NULL) {
      stream->on_message(stream, buf, buf_len);
    }
  }

  if (type & UDX_HEADER_DESTROY) {
    stream->status |= UDX_STREAM_DESTROYED_REMOTE;
    clear_outgoing_packets(stream);
    close_maybe(stream, UDX_ERROR_DESTROYED_REMOTE);
    return 1;
  }

  // process the read queue
  while ((stream->status & UDX_STREAM_SHOULD_READ) == UDX_STREAM_READ) {
    udx_pending_read_t *pkt = (udx_pending_read_t *) udx__cirbuf_remove(inc, stream->ack);
    if (pkt == NULL) break;

    stream->ack++;

    if (pkt->buf.len > 0 && stream->on_data != NULL) {
      stream->on_data(stream, pkt->buf.base, pkt->buf.len);
    }

    free(pkt);
  }

  // Check if the ack is oob.
  if (stream->seq < ack) {
    return 1;
  }

  // Congestion control...
  if (stream->remote_acked != ack) {
    if (stream->cwnd < stream->ssthresh) {
      stream->cwnd += UDX_MTU;
    } else {
      stream->cwnd += max((UDX_MTU * UDX_MTU) / stream->cwnd, 1);
    }
    stream->dup_acks = 0;
  } else if ((type & UDX_HEADER_DATA_OR_END) == 0) {
    stream->dup_acks++;
    if (stream->dup_acks >= 3) {
      fast_retransmit(stream);
    }
  }

  while (stream->remote_acked < ack) {
    int a = ack_packet(stream, stream->remote_acked++, 0);
    if (a == 1) continue;
    if (a == 2) { // it ended, so ack that and trigger close
      // TODO: make this work as well, if the ack packet is lost, ie
      // have some internal (capped) queue of "gracefully closed" streams
      send_state_packet(stream);
      close_maybe(stream, 0);
    }
    return 1;
  }

  // if data pkt, send an ack - use deferred acks as well...
  if (type & UDX_HEADER_DATA_OR_END) {
    send_state_packet(stream);
  }

  if ((stream->status & UDX_STREAM_SHOULD_END_REMOTE) == UDX_STREAM_END_REMOTE && stream->remote_ended <= stream->ack) {
    stream->status |= UDX_STREAM_ENDED_REMOTE;
    if (stream->on_end != NULL) {
      stream->on_end(stream);
    }
    if (close_maybe(stream, 0)) return 1;
  }

  if (stream->pkts_waiting > 0) {
    udx_stream_check_timeouts(stream);
  }

  return 1;
}

static void
trigger_send_callback (udx_t *self, udx_packet_t *pkt) {
  if (pkt->type == UDX_PACKET_SEND) {
    if (self->on_send != NULL) {
      self->on_send(self, pkt->ctx, 0);
    }
    return;
  }

  if (pkt->type == UDX_PACKET_STREAM_SEND) {
    udx_stream_send_t *req = pkt->ctx;
    udx_stream_t *stream = req->stream;

    if (stream->on_send != NULL) {
      stream->on_send(stream, req, 0);
    }
    return;
  }

  if (pkt->type == UDX_PACKET_STREAM_DESTROY) {
    udx_stream_t *stream = pkt->ctx;

    stream->status |= UDX_STREAM_DESTROYED;
    close_maybe(stream, UDX_ERROR_DESTROYED);
    return;
  }
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events) {
  udx_t *self = handle->data;

  if (self->send_queue.len > 0 && events & UV_WRITABLE) {
    udx_packet_t *pkt = (udx_packet_t *) udx__fifo_shift(&(self->send_queue));

    if (pkt == NULL) return;

    assert(pkt->status == UDX_PACKET_SENDING);
    pkt->status = UDX_PACKET_INFLIGHT;
    pkt->transmits++;

    udx__sendmsg(self, pkt);

    int type = pkt->type;

    if (type & UDX_PACKET_CALLBACK) {
      trigger_send_callback(self, pkt);
      // TODO: watch for re-entry here!
    }

    if (type & UDX_PACKET_FREE_ON_SEND) {
      free(pkt);
    }

    // queue another write, might be able to do this smarter...
    if (self->send_queue.len > 0) {
      return;
    }
  }

  if (events & UV_READABLE) {
    struct sockaddr addr;
    uv_buf_t buf;

    char b[2048];
    buf.base = &b;
    buf.len = 2048;

    ssize_t size = udx__recvmsg(self, &buf, &addr);

    if (size > 0 && !process_packet(self, b, size) && self->on_message != NULL) {
      self->on_message(self, b, size, &addr);
    }

    return;
  }

  update_poll(self);
}

int
udx_init (udx_t *self, uv_loop_t *loop) {
  self->status = 0;
  self->readers = 0;
  self->events = 0;

  self->streams_len = 0;
  self->streams_max_len = 16;
  self->streams = malloc(self->streams_max_len * sizeof(udx_stream_t *));

  self->loop = loop;

  self->on_message = NULL;
  self->on_send = NULL;
  self->on_close = NULL;

  udx__fifo_init(&(self->send_queue), 16);
  udx__cirbuf_init(&(self->streams_by_id), 1);

  uv_udp_t *handle = &(self->handle);
  uv_timer_t *timer = &(self->timer);

  // Asserting all the errors here as it massively simplifies error handling.
  // In practice these will never fail.

  int err = uv_timer_init(loop, timer);
  assert(err == 0);

  err = uv_udp_init(loop, handle);
  assert(err == 0);

  timer->data = self;
  handle->data = self;

  return 0;
}

void
udx_set_on_send(udx_t *self, udx_send_cb cb) {
  self->on_send = cb;
}

void
udx_set_on_message(udx_t *self, udx_message_cb cb) {
  self->on_message = cb;
}

void
udx_set_on_close(udx_t *self, udx_close_cb cb) {
  self->on_close = cb;
}

int
udx_send_buffer_size(udx_t *self, int *value) {
  return uv_send_buffer_size((uv_handle_t *) &(self->handle), value);
}

int
udx_recv_buffer_size(udx_t *self, int *value) {
  return uv_recv_buffer_size((uv_handle_t *) &(self->handle), value);
}

int
udx_set_ttl(udx_t *self, int ttl) {
  return uv_udp_set_ttl((uv_udp_t *) &(self->handle), ttl);
}

int
udx_bind (udx_t *self, const struct sockaddr *addr) {
  uv_udp_t *handle = &(self->handle);
  uv_poll_t *poll = &(self->io_poll);
  uv_os_fd_t fd;

  // This might actually fail in practice, so
  int err = uv_udp_bind(handle, addr, 0);
  if (err) return err;

  // Asserting all the errors here as it massively simplifies error handling
  // and in practice non of these will fail, as all our handles are valid and alive.

  err = uv_fileno((const uv_handle_t *) handle, &fd);
  assert(err == 0);

  err = uv_poll_init(self->loop, poll, fd);
  assert(err == 0);

  err = uv_timer_start(&(self->timer), on_uv_interval, UDX_CLOCK_GRANULARITY_MS, UDX_CLOCK_GRANULARITY_MS);
  assert(err == 0);

  self->status |= UDX_SOCKET_BOUND;
  poll->data = self;

  return 0;
}

int
udx_getsockname (udx_t *self, struct sockaddr * name, int *name_len) {
  return uv_udp_getsockname(&(self->handle), name, name_len);
}

int
udx_send (udx_t *self, udx_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *dest) {
  udx_packet_t *pkt = &(req->pkt);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_SEND;
  pkt->ctx = req;
  pkt->dest = *dest;

  pkt->transmits = 0;

  pkt->bufs_len = 1;

  pkt->bufs[0].base = (void *) buf;
  pkt->bufs[0].len = buf_len;

  pkt->fifo_gc = udx__fifo_push(&(self->send_queue), pkt);

  return update_poll(self);
}

int
udx_read_start (udx_t *self) {
  if (self->status & UDX_SOCKET_READING) return 0;

  self->status |= UDX_SOCKET_READING;
  self->readers++;

  return update_poll(self);
}

int
udx_read_stop (udx_t *self) {
  if ((self->status & UDX_SOCKET_READING) == 0) return 0;

  self->status ^= UDX_SOCKET_PAUSED;
  self->readers--;

  return update_poll(self);
}

int
udx_check_timeouts (udx_t *self) {
  for (uint32_t i = 0; i < self->streams_len; i++) {
    int err = udx_stream_check_timeouts(self->streams[i]);
    if (err < 0) return err;
    if (err == 1) i--; // stream was closed, the index again
  }
  return 0;
}

int
udx_close (udx_t *self) {
  if (self->streams_len > 0) return UV_EBUSY;

  self->pending_closes = 2;
  uv_timer_stop(&(self->timer));

  if (self->status & UDX_SOCKET_BOUND) {
    self->pending_closes++;
    uv_poll_stop(&(self->io_poll));
    uv_close((uv_handle_t *) &(self->io_poll), on_uv_close);
  }

  uv_close((uv_handle_t *) &(self->handle), on_uv_close);
  uv_close((uv_handle_t *) &(self->timer), on_uv_close);

  while (1) {
    udx_packet_t *pkt = udx__fifo_shift(&self->send_queue);
    if (pkt == NULL) break;

    if (pkt->type == UDX_PACKET_SEND && self->on_send != NULL) {
      self->on_send(self, pkt->ctx, UDX_ERROR_DESTROYED);
    }
  }

  return 0;
}

int
udx_stream_init (udx_t *self, udx_stream_t *stream, uint32_t *local_id) {
  if (self->streams_len >= 65536) return -1;

  // Get a free socket id (pick a random one until we get a free one)
  uint32_t id;
  while (1) {
    id = random_id();
    udx_cirbuf_val_t *v = udx__cirbuf_get(&(self->streams_by_id), id);
    if (v == NULL) break;
  }

  if (self->streams_len == self->streams_max_len) {
    self->streams_max_len *= 2;
    self->streams = realloc(self->streams, self->streams_max_len * sizeof(udx_stream_t *));
  }

  stream->status = 0;

  *local_id = stream->local_id = id;
  stream->remote_id = 0;
  stream->set_id = self->streams_len++;
  stream->udx = self;

  self->streams[stream->set_id] = stream;

  stream->seq = 0;
  stream->ack = 0;
  stream->remote_acked = 0;

  stream->srtt = 0;
  stream->rttvar = 0;
  stream->rto = 1000;
  stream->rto_timeout = get_milliseconds() + stream->rto;

  stream->pkts_waiting = 0;
  stream->pkts_inflight = 0;
  stream->dup_acks = 0;
  stream->retransmits_waiting = 0;

  stream->inflight = 0;
  stream->ssthresh = 65535;
  stream->cwnd = 2 * UDX_MTU;
  stream->rwnd = 0;

  stream->stats_sacks = 0;
  stream->stats_pkts_sent = 0;
  stream->stats_fast_rt = 0;
  stream->stats_last_seq = 0;

  stream->on_data = NULL;
  stream->on_end = NULL;
  stream->on_drain = NULL;
  stream->on_ack = NULL;
  stream->on_send = NULL;
  stream->on_message = NULL;
  stream->on_close = NULL;

  // Add the socket to the active set
  udx__cirbuf_set(&(self->streams_by_id), (udx_cirbuf_val_t *) stream);

  // Init stream write/read buffers
  udx__cirbuf_init(&(stream->outgoing), 16);
  udx__cirbuf_init(&(stream->incoming), 16);

  return 0;
}

void
udx_stream_set_on_data(udx_stream_t *stream, udx_stream_data_cb cb) {
  stream->on_data = cb;
}

void
udx_stream_set_on_end(udx_stream_t *stream, udx_stream_end_cb cb) {
  stream->on_end = cb;
}

void
udx_stream_set_on_drain(udx_stream_t *stream, udx_stream_drain_cb cb) {
  stream->on_drain = cb;
}

void
udx_stream_set_on_ack(udx_stream_t *stream, udx_stream_ack_cb cb) {
  stream->on_ack = cb;
}

void
udx_stream_set_on_send(udx_stream_t *stream, udx_stream_send_cb cb) {
  stream->on_send = cb;
}

void
udx_stream_set_on_message(udx_stream_t *stream, udx_stream_message_cb cb) {
  stream->on_message = cb;
}

void
udx_stream_set_on_close(udx_stream_t *stream, udx_stream_close_cb cb) {
  stream->on_close = cb;
}

int
udx_stream_check_timeouts (udx_stream_t *stream) {
  if (stream->remote_acked == stream->seq) return 0;

  const uint64_t now = stream->inflight ? get_milliseconds() : 0;

  if (now > stream->rto_timeout) {
    // Ensure it backs off until data is acked...
    stream->rto_timeout = now + 2 * stream->rto;

    // Consider all packet losts - seems to be the simple consensus across different stream impls
    // which we like cause it is nice and simple to implement.
    for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
      udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&(stream->outgoing), seq);

      if (pkt == NULL || pkt->status != UDX_PACKET_INFLIGHT) continue;

      if (pkt->transmits >= UDX_MAX_TRANSMITS) {
        stream->status |= UDX_STREAM_DESTROYED;
        close_maybe(stream, UDX_ERROR_TIMEOUT);
        return 1;
      }

      pkt->status = UDX_PACKET_WAITING;

      stream->inflight -= pkt->size;
      stream->pkts_waiting++;
      stream->pkts_inflight--;
      stream->retransmits_waiting++;
    }

    stream->cwnd = max(UDX_MTU, stream->cwnd / 2);

    printf("pkt loss! stream is congested, scaling back (requeued the full window)\n");
  }

  int err = flush_waiting_packets(stream);
  return err < 0 ? err : 0;
}

void
udx_stream_connect (udx_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr) {
  int already_connected = stream->status & UDX_STREAM_CONNECTED;

  stream->status |= UDX_STREAM_CONNECTED;

  stream->remote_id = remote_id;
  stream->remote_addr = *remote_addr;

  if (already_connected == 0) {
    // TODO: move this to read_start once we have that
    stream->udx->readers++;
    update_poll(stream->udx);
  }
}

int
udx_stream_send (udx_stream_t *stream, udx_stream_send_t *req, const char *buf, size_t buf_len) {
  udx_t *self = stream->udx;
  udx_packet_t *pkt = &(req->pkt);

  init_stream_packet(pkt, UDX_HEADER_MESSAGE, stream, buf, buf_len);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_SEND;
  pkt->ctx = req;
  pkt->transmits = 0;

  req->stream = stream;

  pkt->fifo_gc = udx__fifo_push(&(self->send_queue), pkt);
  return update_poll(self);
}

int
udx_stream_write (udx_stream_t *stream, udx_stream_write_t *req, const char *buf, size_t buf_len) {
  int err = 0;

  req->packets = 0;
  req->stream = stream;

  // if this is the first inflight packet, we should "restart" rto timer
  if (stream->inflight == 0) {
    stream->rto_timeout = get_milliseconds() + stream->rto;
  }

  while (buf_len > 0 || err < 0) {
    udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

    size_t buf_partial_len = buf_len < UDX_MAX_DATA_SIZE ? buf_len : UDX_MAX_DATA_SIZE;

    init_stream_packet(pkt, UDX_HEADER_DATA, stream, buf, buf_partial_len);

    pkt->status = UDX_PACKET_WAITING;
    pkt->type = UDX_PACKET_STREAM_WRITE;
    pkt->ctx = req;

    stream->seq++;
    req->packets++;

    buf_len -= buf_partial_len;
    buf += buf_partial_len;

    udx__cirbuf_set(&(stream->outgoing), (udx_cirbuf_val_t *) pkt);

    // If we are not the first packet in the queue, wait to send us until the queue is flushed...
    if (stream->pkts_waiting++ > 0) continue;
    err = send_data_packet(stream, pkt);
  }

  return err;
}

int
udx_stream_end (udx_stream_t *stream, udx_stream_write_t *req) {
  stream->status |= UDX_STREAM_ENDING;

  req->packets = 1;
  req->stream = stream;

  udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

  init_stream_packet(pkt, UDX_HEADER_END, stream, NULL, 0);

  pkt->status = UDX_PACKET_WAITING;
  pkt->type = UDX_PACKET_STREAM_WRITE;
  pkt->ctx = req;

  stream->seq++;

  udx__cirbuf_set(&(stream->outgoing), (udx_cirbuf_val_t *) pkt);

  if (stream->pkts_waiting++ > 0) return 0;
  return send_data_packet(stream, pkt);
}

int
udx_stream_destroy (udx_stream_t *stream) {
  if ((stream->status & UDX_STREAM_CONNECTED) == 0) {
    stream->status |= UDX_STREAM_DESTROYED;
    close_maybe(stream, UDX_ERROR_DESTROYED);
    return 0;
  }

  stream->status |= UDX_STREAM_DESTROYING;

  clear_outgoing_packets(stream);

  udx_packet_t *pkt = malloc(sizeof(udx_packet_t));

  init_stream_packet(pkt, UDX_HEADER_DESTROY, stream, NULL, 0);

  pkt->status = UDX_PACKET_SENDING;
  pkt->type = UDX_PACKET_STREAM_DESTROY;
  pkt->ctx = stream;

  stream->seq++;

  udx__fifo_push(&(stream->udx->send_queue), pkt);

  int err = update_poll(stream->udx);
  return err < 0 ? err : 1;
}
