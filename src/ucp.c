#include "ucp.h"
#include "fifo.h"
#include "cirbuf.h"
#include "utils.h"

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define UCP_MIN_WINDOW_SIZE 10
#define UCP_MAX_CWND_INCREASE_BYTES_PER_RTT 1

static uint32_t
random_id () {
  return 0x10000 * (rand() & 0xffff) + (rand() & 0xffff);
}

static uint32_t
max (a, b) {
  return a < b ? b : a;
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events);

static int
update_poll (ucp_t *self) {
  int events = (self->send_queue.len > 0 ? UV_WRITABLE : 0) | UV_READABLE;
  if (events == self->events) return 0;

  self->events = events;
  return uv_poll_start(&(self->io_poll), events, on_uv_poll);
}

static void
init_packet (ucp_outgoing_packet_t *pkt, enum UCP_HEADER_TYPE type, ucp_stream_t *stream, const char *buf, size_t buf_len) {
  memset(&(pkt->h), 0, sizeof(struct msghdr));

  uint8_t *b = (uint8_t *) &(pkt->header);

  // 8 bit magic byte + 8 bit version + 8 bit type + 8 bit extensions
  *(b++) = UCP_MAGIC_BYTE;
  *(b++) = UCP_VERSION;
  *(b++) = (uint8_t) type;
  *(b++) = 0; // no extensions atm

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
  pkt->size = (uint16_t) (UCP_HEADER_SIZE + buf_len);

  pkt->h.msg_name = &(stream->remote_addr);
  pkt->h.msg_namelen = sizeof(struct sockaddr_in);

  pkt->h.msg_iov = (struct iovec *) &(pkt->buf);
  pkt->h.msg_iovlen = buf_len ? 2 : 1;

  pkt->buf[0].iov_base = &(pkt->header);
  pkt->buf[0].iov_len = UCP_HEADER_SIZE;

  pkt->buf[1].iov_base = (void *) buf;
  pkt->buf[1].iov_len = buf_len;
}

static int
send_state_packet (ucp_stream_t *stream) {
  uint32_t *sacks = NULL;
  uint32_t start = 0;
  uint32_t end = 0;

  ucp_outgoing_packet_t *pkt = NULL;

  void *payload = NULL;
  size_t payload_len = 0;

  uint32_t max = 512;
  for (uint32_t i = 0; i < max && payload_len < 400; i++) {
    uint32_t seq = stream->ack + 1 + i;
    if (ucp_cirbuf_get(&(stream->incoming), seq) == NULL) continue;

    if (sacks == NULL) {
      pkt = malloc(sizeof(ucp_outgoing_packet_t) + 1024);
      payload = (((void *) pkt) + sizeof(ucp_outgoing_packet_t));
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

  if (pkt == NULL) pkt = malloc(sizeof(ucp_outgoing_packet_t));

  init_packet(pkt, UCP_HEADER_STATE, stream, payload, payload_len);

  pkt->status = UCP_PACKET_SENDING;
  pkt->send = NULL;
  pkt->write = NULL;

  stream->stats_pkts_sent++;

  ucp_fifo_push(&(stream->ucp->send_queue), pkt);
  return update_poll(stream->ucp);
}

static int
send_data_packet (ucp_stream_t *stream, ucp_outgoing_packet_t *pkt) {
  if (stream->inflight + pkt->size > stream->cwnd) {
    return 0;
  }

  assert(pkt->status == UCP_PACKET_WAITING);

  pkt->status = UCP_PACKET_SENDING;

  stream->pkts_waiting--;
  stream->pkts_inflight++;
  stream->inflight += pkt->size;
  if (pkt->transmits > 0) stream->retransmits_waiting--;

  stream->stats_pkts_sent++;

  ucp_fifo_push(&(stream->ucp->send_queue), pkt);
  int err = update_poll(stream->ucp);
  return err < 0 ? err : 1;
}

static int
flush_waiting_packets (ucp_stream_t *stream) {
  const uint32_t was_waiting = stream->pkts_waiting;
  uint32_t seq = stream->retransmits_waiting ? stream->remote_acked : (stream->seq - stream->pkts_waiting);

  int sent = 0;

  while (seq != stream->seq && stream->pkts_waiting > 0) {
    ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_get(&(stream->outgoing), seq++);

    if (pkt == NULL || pkt->status != UCP_PACKET_WAITING) continue;

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

static void
close_maybe (ucp_stream_t *stream) {
  // if BOTH closed or ANY destroyed
  if ((stream->state & UCP_ST_ALL_ENDED) != UCP_ST_ALL_ENDED && !(stream->state & UCP_ST_ANY_DESTROYED)) return;

  if (stream->state & UCP_ST_CLOSED) return;
  stream->state |= UCP_ST_CLOSED;

  if (stream->on_close != NULL) {
    stream->on_close(stream, stream->state & UCP_ST_ANY_DESTROYED ? 1 : 0);
  }
}

static int
ack_packet (ucp_stream_t *stream, uint32_t seq, int sack) {
  ucp_cirbuf_t *out = &(stream->outgoing);
  ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_remove(out, seq);

  if (pkt == NULL) return 0;

  if (pkt->status == UCP_PACKET_INFLIGHT) {
    stream->pkts_inflight--;
    stream->inflight -= pkt->size;
  }

  if (pkt->transmits == 1) {
    const uint32_t rtt = (uint32_t) (ucp_get_milliseconds() - pkt->time_sent);

    // First round trip time sample
    if (stream->srtt == 0) {
      stream->srtt = rtt;
      stream->rttvar = rtt / 2;
      stream->rto = stream->srtt + max(UCP_CLOCK_GRANULARITY_MS, 4 * stream->rttvar);
    } else {
      const uint32_t delta = rtt < stream->srtt ? stream->srtt - rtt : rtt - stream->srtt;
      // RTTVAR <- (1 - beta) * RTTVAR + beta * |SRTT - R'| where beta is 1/4
      stream->rttvar = (3 * stream->rttvar + delta) / 4;

      // SRTT <- (1 - alpha) * SRTT + alpha * R' where alpha is 1/8
      stream->srtt = (7 * stream->srtt + rtt) / 8;
    }

    // RTO <- SRTT + max (G, K*RTTVAR) where K is 4 maxed with 1s
    stream->rto = max(stream->srtt + max(UCP_CLOCK_GRANULARITY_MS, 4 * stream->rttvar), 1000);
  }

  if (!sack) { // Reset rto timer when new data is ack'ed (inorder)
    stream->rto_timeout = ucp_get_milliseconds() + stream->rto;
  }

  ucp_write_t *w = (ucp_write_t *) pkt->write;

  // If this packet was queued for sending we cannot free it now,
  // mark it as acked and free it in the poll / when it's next used
  if (pkt->status == UCP_PACKET_SENDING) {
    pkt->status = UCP_PACKET_ACKED;
  } else {
    free(pkt);
  }

  if (--(w->packets) == 0) {
    if (stream->on_ack != NULL) {
      stream->on_ack(stream, w, 0, sack);
    }
    if ((stream->state & UCP_ST_SHOULD_END) == UCP_ST_END && stream->pkts_waiting == 0 && stream->pkts_inflight == 0) {
      stream->state |= UCP_ST_ENDED;
      close_maybe(stream);
    }
  }

  return 1;
}

static void
process_sacks (ucp_stream_t *stream, char *buf, size_t buf_len) {
  uint32_t n = 0;
  uint32_t *sacks = (uint32_t *) buf;

  for (size_t i = 0; i + 8 <= buf_len; i += 8) {
    uint32_t start = *(sacks++);
    uint32_t end = *(sacks++);
    uint32_t len = end - start;

    for (uint32_t j = 0; j < len; j++) {
      if (ack_packet(stream, start + j, 1)) {
        n++;
      }
    }
  }

  if (n) {
    stream->stats_sacks += n;
  }
}

static void
fast_retransmit (ucp_stream_t *stream) {
  ucp_cirbuf_t *out = &(stream->outgoing);
  ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_get(out, stream->remote_acked);

  if (pkt == NULL || pkt->transmits != 1 || pkt->status != UCP_PACKET_INFLIGHT) return;

  pkt->status = UCP_PACKET_WAITING;
  stream->inflight -= pkt->size;
  stream->pkts_waiting++;
  stream->pkts_inflight--;
  stream->retransmits_waiting++;
  stream->stats_fast_rt++;

  // Shrink the window
  stream->cwnd = max(UCP_MTU, stream->cwnd / 2);
}

static int
process_packet (ucp_t *self, char *buf, ssize_t buf_len) {
  if (buf_len < UCP_HEADER_SIZE) return 0;

  uint8_t *b = (uint8_t *) buf;

  if ((*(b++) != UCP_MAGIC_BYTE) || (*(b++) != UCP_VERSION)) return 0;

  enum UCP_HEADER_TYPE type = (enum UCP_HEADER_TYPE) *(b++);
  uint8_t ext = *(b++);

  uint32_t *i = (uint32_t *) b;

  uint32_t local_id = *(i++);
  uint32_t recv_win = *(i++);
  uint32_t seq = *(i++);
  uint32_t ack = *i;

  buf += UCP_HEADER_SIZE;
  buf_len -= UCP_HEADER_SIZE;

  ucp_stream_t *stream = (ucp_stream_t *) ucp_cirbuf_get(&(self->streams_by_id), local_id);
  if (stream == NULL) return 0;

  ucp_cirbuf_t *inc = &(stream->incoming);

  switch (type) {
    case UCP_HEADER_STATE: {
      if (buf_len == 0) break;
      process_sacks(stream, buf, buf_len);
      break;
    }

    case UCP_HEADER_DATA:
    case UCP_HEADER_END: {
      if (ucp_cirbuf_get(inc, seq) != NULL) break;
      // Copy over incoming buffer as we CURRENTLY do not own it (stack allocated upstream)
      // TODO: if this is the next packet we expect (it usually is!), then there is no need
      // for the malloc and memcpy - we just need a way to not free it then
      char *ptr = malloc(sizeof(ucp_incoming_packet_t) + buf_len);

      ucp_incoming_packet_t *pkt = (ucp_incoming_packet_t *) ptr;
      char *cpy = ptr + sizeof(ucp_incoming_packet_t);

      memcpy(cpy, buf, buf_len);

      pkt->seq = seq;
      pkt->ended = type == UCP_HEADER_END;
      pkt->buf.iov_base = cpy;
      pkt->buf.iov_len = buf_len;

      ucp_cirbuf_set(inc, (ucp_cirbuf_val_t *) pkt);
      break;
    }
  }

  while (1) {
    ucp_incoming_packet_t *pkt = (ucp_incoming_packet_t *) ucp_cirbuf_remove(inc, stream->ack);
    if (pkt == NULL) break;

    stream->ack++;

    if (pkt->buf.iov_len > 0 && stream->on_read != NULL) {
      stream->on_read(stream, pkt->buf.iov_base, pkt->buf.iov_len);
    }
    if (pkt->ended && (stream->state & UCP_ST_SHOULD_END_REMOTE) == UCP_ST_END_REMOTE && stream->on_end != NULL) {
      stream->state |= UCP_ST_ENDED_REMOTE;
      stream->on_end(stream);
      close_maybe(stream);
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
      stream->cwnd += UCP_MTU;
    } else {
      stream->cwnd += max((UCP_MTU * UCP_MTU) / stream->cwnd, 1);
    }
    stream->dup_acks = 0;
  } else {
    stream->dup_acks++;
    if (stream->dup_acks >= 3) {
      fast_retransmit(stream);
    }
  }

  while (stream->remote_acked < ack) {
    ack_packet(stream, stream->remote_acked++, 0);
  }

  // if data pkt, send an ack - use deferred acks as well...
  if (type == UCP_HEADER_DATA || type == UCP_HEADER_END) {
    send_state_packet(stream);
  }

  if (stream->pkts_waiting > 0) {
    ucp_stream_check_timeouts(stream);
  }

  return 1;
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events) {
  ucp_t *self = handle->data;

  if (self->send_queue.len > 0 && events & UV_WRITABLE) {
    ssize_t size;
    ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_fifo_shift(&(self->send_queue));
    const struct msghdr *h = &(pkt->h);

    // This was unqueued and therefore no longer valid, free it and restart.
    if (pkt->status != UCP_PACKET_SENDING) {
      free(pkt);
      return;
    }

    pkt->transmits++;
    pkt->status = UCP_PACKET_INFLIGHT;

    do {
      pkt->time_sent = ucp_get_milliseconds();
      size = sendmsg(handle->io_watcher.fd, h, 0);
    } while (size == -1 && errno == EINTR);

    if (pkt->write != NULL) {
      pkt->write->stream->stats_last_seq = pkt->seq;
    } else if (pkt->send != NULL) {
      if (self->on_send != NULL) {
        self->on_send(self, pkt->send, 0);
        // TODO: watch for re-entry here!
      }
    } else {
      // Fire and forget package - ie an ack, destroy etc
      free(pkt);
    }

    // queue another write, might be able to do this smarter...
    if (self->send_queue.len > 0) {
      return;
    }
  }

  if (events & UV_READABLE) {
    ssize_t size;
    struct msghdr h;
    struct iovec buf;

    char b[2048];
    buf.iov_base = &b;
    buf.iov_len = 2048;

    h.msg_name = &(self->on_message_addr);
    h.msg_namelen = sizeof(struct sockaddr_in);
    h.msg_iov = &buf;
    h.msg_iovlen = 1;

    do {
      size = recvmsg(handle->io_watcher.fd, &h, 0);
    } while (size == -1 && errno == EINTR);

    if (size > 0 && !process_packet(self, b, size) && self->on_message != NULL) {
      self->on_message(self, b, size, h.msg_name);
      // TODO: watch for re-entry here!
    }

    return;
  }

  update_poll(self);
}

int
ucp_init (ucp_t *self, uv_loop_t *loop) {
  int err;

  self->streams_len = 0;
  self->streams_max_len = 16;
  self->events = 0;
  self->loop = loop;
  self->on_message = NULL;
  self->on_send = NULL;

  self->streams = malloc(self->streams_max_len * sizeof(ucp_stream_t *));
  ucp_fifo_init(&(self->send_queue), 16);
  ucp_cirbuf_init(&(self->streams_by_id), 1);

  err = uv_udp_init(loop, &(self->handle));
  return err;
}

int
ucp_set_callback (ucp_t *self, enum UCP_CALLBACK name, void *fn) {
  switch (name) {
    case UCP_ON_SEND: {
      self->on_send = fn;
      return 0;
    }
    case UCP_ON_MESSAGE: {
      self->on_message = fn;
      return 0;
    }
    default: {
      return -1;
    }
  }
}

int
ucp_send_buffer_size(ucp_t *self, int *value) {
  return uv_send_buffer_size((uv_handle_t *) &(self->handle), value);
}

int
ucp_recv_buffer_size(ucp_t *self, int *value) {
  return uv_recv_buffer_size((uv_handle_t *) &(self->handle), value);
}

int
ucp_set_ttl(ucp_t *self, int ttl) {
  return uv_udp_set_ttl((uv_udp_t *) &(self->handle), ttl);
}

int
ucp_bind (ucp_t *self, const struct sockaddr *addr) {
  int err;

  uv_udp_t *handle = &(self->handle);
  uv_poll_t *poll = &(self->io_poll);
  uv_os_fd_t fd;

  err = uv_udp_bind(handle, addr, 0);
  if (err) return err;

  err = uv_fileno((const uv_handle_t *) handle, &fd);
  if (err) return err;

  err = uv_poll_init(self->loop, poll, fd);
  if (err) return err;

  poll->data = self;

  err = update_poll(self);
  return err;
}

int
ucp_getsockname (ucp_t *self, struct sockaddr * name, int *name_len) {
  return uv_udp_getsockname(&(self->handle), name, name_len);
}

int
ucp_send (ucp_t *self, ucp_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *dest) {
  int err;

  ucp_outgoing_packet_t *pkt = &(req->pkt);

  req->dest = *dest;

  pkt->transmits = 0;
  pkt->status = UCP_PACKET_SENDING;

  memset(&(pkt->h), 0, sizeof(struct msghdr));

  pkt->h.msg_name = &(req->dest);
  pkt->h.msg_namelen = sizeof(struct sockaddr_in);

  pkt->h.msg_iov = (struct iovec *) &(pkt->buf);
  pkt->h.msg_iovlen = 1;

  pkt->buf[0].iov_base = (void *) buf;
  pkt->buf[0].iov_len = buf_len;

  pkt->send = req;
  pkt->write = NULL;

  ucp_fifo_push(&(self->send_queue), pkt);

  err = update_poll(self);

  return err;
}

int
ucp_check_timeouts (ucp_t *self) {
  for (uint32_t i = 0; i < self->streams_len; i++) {
    // TODO: if this results in the stream being removed, we should decrement i
    int err = ucp_stream_check_timeouts(self->streams[i]);
    if (err < 0) return err;
  }
  return 0;
}

int
ucp_stream_init (ucp_t *self, ucp_stream_t *stream, uint32_t *local_id) {
  if (self->streams_len >= 65536) return -1;

  // Get a free socket id (pick a random one until we get a free one)
  uint32_t id;
  while (1) {
    id = random_id();
    ucp_cirbuf_val_t *v = ucp_cirbuf_get_stored(&(self->streams_by_id), id);
    if (v == NULL) break;
  }

  if (self->streams_len == self->streams_max_len) {
    self->streams_max_len *= 2;
    self->streams = realloc(self->streams, self->streams_max_len * sizeof(ucp_stream_t *));
  }

  stream->state = 0;

  *local_id = stream->local_id = id;
  stream->remote_id = 0;
  stream->set_id = self->streams_len++;
  stream->ucp = self;

  self->streams[stream->set_id] = stream;

  stream->seq = 0;
  stream->ack = 0;
  stream->remote_acked = 0;

  stream->srtt = 0;
  stream->rttvar = 0;
  stream->rto = 1000;
  stream->rto_timeout = ucp_get_milliseconds() + stream->rto;

  stream->pkts_waiting = 0;
  stream->pkts_inflight = 0;
  stream->dup_acks = 0;
  stream->retransmits_waiting = 0;

  stream->inflight = 0;
  stream->ssthresh = 65535;
  stream->cwnd = 2 * UCP_MTU;
  stream->rwnd = 0;

  stream->stats_sacks = 0;
  stream->stats_pkts_sent = 0;
  stream->stats_fast_rt = 0;
  stream->stats_last_seq = 0;

  stream->on_read = NULL;
  stream->on_end = NULL;
  stream->on_drain = NULL;
  stream->on_ack = NULL;
  stream->on_close = NULL;

  // Add the socket to the active set
  ucp_cirbuf_set(&(self->streams_by_id), (ucp_cirbuf_val_t *) stream);

  // Init stream write/read buffers
  ucp_cirbuf_init(&(stream->outgoing), 16);
  ucp_cirbuf_init(&(stream->incoming), 16);

  return 0;
}

int
ucp_stream_set_callback (ucp_stream_t *self, enum UCP_CALLBACK name, void *fn) {
  switch (name) {
    case UCP_ON_READ: {
      self->on_read = fn;
      return 0;
    }
    case UCP_ON_END: {
      self->on_end = fn;
      return 0;
    }
    case UCP_ON_DRAIN: {
      self->on_drain = fn;
      return 0;
    }
    case UCP_ON_ACK: {
      self->on_ack = fn;
      return 0;
    }
    case UCP_ON_CLOSE: {
      self->on_close = fn;
      return 0;
    }
    default: {
      return UV_EINVAL;
    }
  }

  return UV_EINVAL;
}

int
ucp_stream_check_timeouts (ucp_stream_t *stream) {
  if (stream->remote_acked == stream->seq) return 0;

  const uint64_t now = stream->inflight ? ucp_get_milliseconds() : 0;

  if (now > stream->rto_timeout) {
    // Ensure it backs off until data is acked...
    stream->rto_timeout = now + 2 * stream->rto;

    // Consider all packet losts - seems to be the simple consensus across different stream impls
    // which we like cause it is nice and simple to implement.
    for (uint32_t seq = stream->remote_acked; seq != stream->seq; seq++) {
      ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_get(&(stream->outgoing), seq);

      if (pkt == NULL || pkt->status != UCP_PACKET_INFLIGHT) continue;

      pkt->status = UCP_PACKET_WAITING;

      stream->inflight -= pkt->size;
      stream->pkts_waiting++;
      stream->pkts_inflight--;
      stream->retransmits_waiting++;
    }

    stream->cwnd = max(UCP_MTU, stream->cwnd / 2);

    printf("pkt loss! stream is congested, scaling back (requeued the full window)\n");
  }

  return flush_waiting_packets(stream);
}

void
ucp_stream_connect (ucp_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr) {
  stream->remote_id = remote_id;
  stream->remote_addr = *remote_addr;
}

int
ucp_stream_write (ucp_stream_t *stream, ucp_write_t *req, const char *buf, size_t buf_len) {
  int err = 0;

  req->packets = 0;
  req->stream = stream;

  // if this is the first inflight packet, we should "restart" rto timer
  if (stream->inflight == 0) {
    stream->rto_timeout = ucp_get_milliseconds() + stream->rto;
  }

  while (buf_len > 0 || err < 0) {
    ucp_outgoing_packet_t *pkt = malloc(sizeof(ucp_outgoing_packet_t));

    size_t buf_partial_len = buf_len < UCP_MAX_DATA_SIZE ? buf_len : UCP_MAX_DATA_SIZE;

    init_packet(pkt, UCP_HEADER_DATA, stream, buf, buf_partial_len);

    pkt->status = UCP_PACKET_WAITING;
    pkt->send = NULL;
    pkt->write = req;

    stream->seq++;
    req->packets++;

    buf_len -= buf_partial_len;
    buf += buf_partial_len;

    ucp_cirbuf_set(&(stream->outgoing), (ucp_cirbuf_val_t *) pkt);

    // If we are not the first packet in the queue, wait to send us until the queue is flushed...
    if (stream->pkts_waiting++ > 0) continue;
    err = send_data_packet(stream, pkt);
  }

  return err;
}

int
ucp_stream_end (ucp_stream_t *stream, ucp_write_t *req) {
  stream->state |= UCP_ST_ENDING;

  req->packets = 1;
  req->stream = stream;

  ucp_outgoing_packet_t *pkt = malloc(sizeof(ucp_outgoing_packet_t));

  init_packet(pkt, UCP_HEADER_END, stream, NULL, 0);

  pkt->status = UCP_PACKET_WAITING;
  pkt->send = NULL;
  pkt->write = req;

  stream->seq++;

  ucp_cirbuf_set(&(stream->outgoing), (ucp_cirbuf_val_t *) pkt);

  if (stream->pkts_waiting++ > 0) return 0;
  return send_data_packet(stream, pkt);
}

int
ucp_stream_destroy (ucp_stream_t *stream) {
  ucp_outgoing_packet_t *pkt = malloc(sizeof(ucp_outgoing_packet_t));

  init_packet(pkt, UCP_HEADER_DESTROY, stream, NULL, 0);

  pkt->status = UCP_PACKET_SENDING;
  pkt->send = NULL;
  pkt->write = NULL;

  stream->stats_pkts_sent++;

  ucp_fifo_push(&(stream->ucp->send_queue), pkt);
  return update_poll(stream->ucp);
}
