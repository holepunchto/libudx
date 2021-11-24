#include "ucp.h"
#include "fifo.h"
#include "cirbuf.h"
#include "utils.h"

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>

// #define UCP_DEBUG(fmt, ...) printf("DEBUG: " fmt, ##__VA_ARGS__)
#define UCP_DEBUG(fmt, ...) {}

#define UCP_MAX(x, y) (((x) > (y)) ? (x) : (y))

#define UCP_MIN_WINDOW_SIZE 10
#define UCP_MAX_CWND_INCREASE_BYTES_PER_RTT 10

static uint32_t
random_id () {
  return 0x10000 * (rand() & 0xffff) + (rand() & 0xffff);
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events);

static int
update_poll (ucp_t *self) {
  int events = (self->send_queue.len > 0 ? UV_WRITABLE : 0) | UV_READABLE;
  if (events == self->events) return 0;

// printf("update io polling flags=%i\n", events);

  self->events = events;
  return uv_poll_start(&(self->io_poll), events, on_uv_poll);
}

static int
ack_packet (ucp_stream_t *stream, uint32_t seq, int sack) {
  ucp_cirbuf_t *out = &(stream->outgoing);
  ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_remove(out, seq);

  if (pkt == NULL) return 0;

  if (pkt->status == UCP_PACKET_INFLIGHT) {
    stream->pkts_inflight--;
    stream->cur_window_bytes -= pkt->size;

    // TODO: do some proper congestion control stuff...
    stream->max_window_bytes += UCP_MAX_CWND_INCREASE_BYTES_PER_RTT;
  }

  if (pkt->transmits == 1) {
    const uint32_t ertt = (uint32_t) (ucp_get_microseconds() / 1000 - pkt->time_sent);

    if (stream->rtt == 0) {
      // First round trip time sample
      stream->rtt = ertt;
      stream->rtt_var = ertt / 2;
    } else {
      // Compute new round trip times
      const int32_t delta = (int32_t) (stream->rtt - ertt);

      stream->rtt_var = UCP_MAX(stream->rtt_var + (int32_t) (abs(delta) - stream->rtt_var) / 4, 0);
      stream->rtt = stream->rtt - stream->rtt / 8 + ertt / 8;
    }

    stream->rto = UCP_MAX(stream->rtt + stream->rtt_var * 4, 1000);
  }

  ucp_write_t *w = (ucp_write_t *) pkt->write;

  // If this packet was queued for sending we cannot free it now,
  // mark it as acked and free it in the poll / when it's next used
  if (pkt->status == UCP_PACKET_SENDING) {
    pkt->status = UCP_PACKET_ACKED;
  } else {
    free(pkt);
  }

  if (--(w->packets) == 0 && stream->on_write != NULL) {
    stream->on_write(stream, w, 0, sack);
  }

  return 1;
}

static void
process_sacks (ucp_stream_t *stream, char *buf, size_t buf_len) {
  uint32_t n = 0;
  uint32_t *sacks = (uint32_t *) buf;

  for (int i = 0; i + 8 <= buf_len; i += 8) {
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

static int
process_packet (ucp_t *self, char *buf, ssize_t buf_len) {
  if (buf_len < UCP_HEADER_SIZE) return 0;

  uint32_t *u = (uint32_t *) buf;

  uint32_t h = *(u++);
  uint32_t local_id = *(u++);
  uint32_t seq = *(u++);
  uint32_t ack = *(u++);

  buf += UCP_HEADER_SIZE;
  buf_len -= UCP_HEADER_SIZE;

  ucp_stream_t *stream = (ucp_stream_t *) ucp_cirbuf_get(&(self->sockets), local_id);
  if (stream == NULL) return 0;

  UCP_DEBUG("incoming packet header:\n  h = %u\n  id = %u\n  seq = %u\n  ack = %u\n", h, local_id, seq, ack);

  ucp_cirbuf_t *inc = &(stream->incoming);
  ucp_cirbuf_t *out = &(stream->outgoing);

  switch (h) {
    case UCP_PACKET_DATA_ID: {
      if (buf_len == 0 || ucp_cirbuf_get(inc, seq) != NULL) break;
      // copy over incoming buffer as we CURRENTLY do not own it (stack allocated upstream)
      // also malloc the packet wrap which needs to be freed at some point obvs
      char *ptr = malloc(sizeof(ucp_incoming_packet_t) + buf_len);

      ucp_incoming_packet_t *pkt = (ucp_incoming_packet_t *) ptr;
      char *cpy = ptr + sizeof(ucp_incoming_packet_t);

      memcpy(cpy, buf, buf_len);

      pkt->seq = seq;
      pkt->buf.iov_base = cpy;
      pkt->buf.iov_len = buf_len;

      ucp_cirbuf_set(inc, (ucp_cirbuf_val_t *) pkt);
      break;
    }

    case UCP_PACKET_STATE_ID: {
      if (buf_len == 0) break;
      process_sacks(stream, buf, buf_len);
      break;
    }
  }

  while (1) {
    ucp_incoming_packet_t *pkt = (ucp_incoming_packet_t *) ucp_cirbuf_remove(inc, stream->ack);
    if (pkt == NULL) break;

    stream->ack++;
    if (stream->on_read != NULL) {
      stream->on_read(stream, pkt->buf.iov_base, pkt->buf.iov_len);
    }

    free(pkt);
  }

  // Check if the ack is oob.
  if (stream->seq < ack) {
    return 1;
  }

  while (stream->remote_acked < ack) {
    ack_packet(stream, stream->remote_acked++, 0);
  }

  ucp_stream_check_timeouts(stream);

  // if data pkt, send an ack - use deferred acks as well...
  if (h == UCP_PACKET_DATA_ID) {
    ucp_stream_send_state(stream);
  }

  return 1;
}

static void
on_uv_poll (uv_poll_t *handle, int status, int events) {
  ucp_t *self = handle->data;
  uv_poll_t *poll = &(self->io_poll);
  const uint32_t pending = self->send_queue.len;

  if (self->send_queue.len > 0 && events & UV_WRITABLE) {
    ssize_t size;
    ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_fifo_shift(&(self->send_queue));
    const struct msghdr *h = &(pkt->h);
    const struct sockaddr_in *d = h->msg_name;

    // This was unqueued and therefore no longer valid, free it and restart.
    if (pkt->status != UCP_PACKET_SENDING) {
      free(pkt);
      return;
    }

    pkt->transmits++;
    pkt->status = UCP_PACKET_INFLIGHT;

    do {
      pkt->time_sent = ucp_get_microseconds() / 1000;
      size = sendmsg(handle->io_watcher.fd, h, 0);
    } while (size == -1 && errno == EINTR);

    if (pkt->send != NULL) {
      // TODO: needs free'ing also
      UCP_DEBUG("sent udp packet!\n");
    } else if (pkt->write != NULL) {
      UCP_DEBUG("sent stream packet!\n");
      pkt->write->stream->stats_pkts_sent++;
      pkt->write->stream->stats_last_seq = pkt->seq;
    } else { // an ack etc
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

    char b[4096];
    buf.iov_base = &b;
    buf.iov_len = 4096;

    h.msg_name = &(self->on_message_addr);
    h.msg_namelen = sizeof(struct sockaddr_in);
    h.msg_iov = &buf;
    h.msg_iovlen = 1;

    do {
      size = recvmsg(handle->io_watcher.fd, &h, 0);
    } while (size == -1 && errno == EINTR);

    if (!process_packet(self, b, size) && self->on_message != NULL) {
      self->on_message(self, b, size, h.msg_name);
    }

    return;
  }

  update_poll(self);
}

int
ucp_init (ucp_t *self, uv_loop_t *loop) {
  int err;

  self->active_sockets = 0;
  self->events = 0;
  self->bound = 0;
  self->loop = loop;
  self->on_message = NULL;

  ucp_fifo_init(&(self->send_queue), 16);
  ucp_cirbuf_init(&(self->sockets), 1);

  err = uv_udp_init(loop, &(self->handle));
  return err;
}

int
ucp_set_callback (ucp_t *self, enum UCP_TYPE name, void *fn) {
  switch (name) {
    case UCP_ON_MESSAGE: {
      self->on_message = fn;
      return self->bound ? update_poll(self) : 0;
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
ucp_bind (ucp_t *self, const struct sockaddr *addr) {
  int err;

  if (self->bound) return -1;

  uv_udp_t *handle = &(self->handle);
  uv_poll_t *poll = &(self->io_poll);
  uv_os_fd_t fd;

  err = uv_udp_bind(handle, addr, 0);
  if (err) return err;

  err = uv_fileno((const uv_handle_t *) handle, &fd);
  if (err) return err;

  err = uv_poll_init(self->loop, poll, fd);
  if (err) return err;

  self->bound = 1;
  poll->data = self;

  err = update_poll(self);
  return err;
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
ucp_stream_init (ucp_t *self, ucp_stream_t *stream) {
  if (self->active_sockets >= 65536) return -1;

  // Get a free socket id (pick a random one until we get a free one)
  uint32_t id;
  while (1) {
    id = random_id();
    ucp_cirbuf_val_t *v = ucp_cirbuf_get_stored(&(self->sockets), id);
    if (v == NULL) break;
  }

  stream->local_id = id;
  stream->remote_id = 0;
  stream->ucp = self;
  stream->seq = 0;
  stream->ack = 0;
  stream->remote_acked = 0;

  stream->rtt = 0;
  stream->rtt_var = 0;
  stream->rto = 1000;

  stream->pkts_waiting = 0;
  stream->pkts_inflight = 0;

  stream->cur_window_bytes = 0;
  stream->max_window_bytes = UCP_PACKET_SIZE;

  stream->stats_sacks = 0;
  stream->stats_pkts_sent = 0;
  stream->stats_last_seq = 0;

  stream->on_read = NULL;
  stream->on_write = NULL;

  // Add the socket to the active set
  self->active_sockets++;
  ucp_cirbuf_set(&(self->sockets), (ucp_cirbuf_val_t *) stream);

  // Init stream write/read buffers
  ucp_cirbuf_init(&(stream->outgoing), 16);
  ucp_cirbuf_init(&(stream->incoming), 16);

  return 0;
}

int
ucp_stream_set_callback (ucp_stream_t *self, enum UCP_TYPE name, void *fn) {
  switch (name) {
    case UCP_ON_READ: {
      self->on_read = fn;
      return 0;
    }
    case UCP_ON_WRITE: {
      self->on_write = fn;
      return 0;
    }
    default: {
      return -1;
    }
  }

  return -1;
}

static int
send_data_packet (ucp_stream_t *stream, ucp_outgoing_packet_t *pkt) {
  if (stream->cur_window_bytes + pkt->size > stream->max_window_bytes) {
    return 0;
  }

  if (pkt->status != UCP_PACKET_WAITING) {
    printf("assertion failure!\n");
    exit(1);
  }

  pkt->status = UCP_PACKET_SENDING;

  stream->pkts_waiting--;
  stream->pkts_inflight++;
  stream->cur_window_bytes += pkt->size;

  ucp_fifo_push(&(stream->ucp->send_queue), pkt);
  return update_poll(stream->ucp) < 0 ? -1 : 1;
}

int
ucp_stream_check_timeouts (ucp_stream_t *stream) {
  const uint32_t cur_range = stream->seq - stream->remote_acked;

  ucp_stream_resend(stream);
  int sent = 0;

  for (uint32_t i = 0; i < cur_range && stream->pkts_waiting > 0; i++) {
    uint32_t seq = stream->remote_acked + i;
    ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_get(&(stream->outgoing), seq);

    if (pkt == NULL) continue;
    if (pkt->status != UCP_PACKET_WAITING || pkt->transmits > 0) continue;

    sent = send_data_packet(stream, pkt);
    if (sent <= 0) break;
  }

  return sent > 0 ? 0 : sent;
}

int
ucp_stream_resend (ucp_stream_t *stream) {
  if (stream->remote_acked == stream->seq) return 0;

  const uint32_t cur_range = stream->seq - stream->remote_acked;
  const uint64_t ms = ucp_get_microseconds() / 1000;

  ucp_fifo_t *queue = &(stream->ucp->send_queue);

  int sent = 0;
  int congested = 0;

  uint32_t queued = queue->len;

  for (uint32_t i = 0; i < cur_range; i++) {
    uint32_t seq = stream->remote_acked + i;
    ucp_outgoing_packet_t *pkt = (ucp_outgoing_packet_t *) ucp_cirbuf_get(&(stream->outgoing), seq);

    if (pkt == NULL) continue;
    if (pkt->status != UCP_PACKET_INFLIGHT) continue;

    const uint32_t delta = (uint32_t) (ms - pkt->time_sent);
    if (delta < 3 * stream->rto) continue;

    // Consider this packet lost

    pkt->status = UCP_PACKET_WAITING;
    stream->cur_window_bytes -= pkt->size;
    stream->pkts_waiting++;
    stream->pkts_inflight--;

    if (congested == 0) {
      congested = 1;
      stream->max_window_bytes = UCP_MAX(UCP_PACKET_SIZE, stream->max_window_bytes / 2);
    }

    sent = send_data_packet(stream, pkt);
    if (sent <= 0) break;
  }

  if (congested) {
    printf("stream is congested, scaling back (requeued %u)\n", queue->len - queued);
  }

  return sent < 0 ? -1 : 0;
}

void
ucp_stream_connect (ucp_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr) {
  stream->remote_id = remote_id;
  stream->remote_addr = *remote_addr;
}

int
ucp_stream_send_state (ucp_stream_t *stream) {
  UCP_DEBUG("writing ack\n");

  uint32_t *sacks = NULL;
  uint32_t start = -1;
  uint32_t end = -1;

  ucp_outgoing_packet_t *pkt = NULL;

  void *payload = NULL;
  size_t payload_len = 0;

  int max = 512;
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

  if (start != -1) {
    *(sacks++) = start;
    *(sacks++) = end;
    payload_len += 8;
  }

  if (pkt == NULL) pkt = malloc(sizeof(ucp_outgoing_packet_t));

  uint32_t *p = (uint32_t *) &(pkt->header);

  *(p++) = 1;
  *(p++) = stream->remote_id;
  *(p++) = (pkt->seq = stream->seq);
  *(p++) = stream->ack;

  int err;

  memset(&(pkt->h), 0, sizeof(struct msghdr));

  pkt->transmits = 0;
  pkt->status = UCP_PACKET_SENDING;

  pkt->h.msg_name = &(stream->remote_addr);
  pkt->h.msg_namelen = sizeof(struct sockaddr_in);

  pkt->h.msg_iov = (struct iovec *) &(pkt->buf);
  pkt->h.msg_iovlen = payload_len == 0 ? 1 : 2;

  pkt->buf[0].iov_base = &(pkt->header);
  pkt->buf[0].iov_len = UCP_HEADER_SIZE;

  if (payload_len > 0) {
    pkt->buf[1].iov_base = payload;
    pkt->buf[1].iov_len = payload_len;
  }

  pkt->send = NULL;
  pkt->write = NULL;

  ucp_fifo_push(&(stream->ucp->send_queue), pkt);
  err = update_poll(stream->ucp);

  return err;
}

int
ucp_stream_write (ucp_stream_t *stream, ucp_write_t *req, const char *buf, size_t buf_len) {
  UCP_DEBUG("writing data (%zu bytes) - packet size is %zu bytes\n", buf_len, buf_len + UCP_HEADER_SIZE);

  ucp_outgoing_packet_t *pkt = malloc(sizeof(ucp_outgoing_packet_t));

  req->packets = 1;
  req->stream = stream;

  uint32_t *p = (uint32_t *) &(pkt->header);

  *(p++) = UCP_PACKET_DATA_ID;
  *(p++) = stream->remote_id;
  *(p++) = (pkt->seq = stream->seq++);
  *(p++) = stream->ack;

  UCP_DEBUG("outgoing packet header:\n  h = 0\n  id = %u\n  seq = %u\n  ack = %u\n", stream->remote_id, pkt->seq, stream->ack);

  memset(&(pkt->h), 0, sizeof(struct msghdr));

  pkt->status = UCP_PACKET_WAITING;
  pkt->transmits = 0;
  pkt->size = (uint16_t) (UCP_HEADER_SIZE + buf_len);

  pkt->h.msg_name = &(stream->remote_addr);
  pkt->h.msg_namelen = sizeof(struct sockaddr_in);

  pkt->h.msg_iov = (struct iovec *) &(pkt->buf);
  pkt->h.msg_iovlen = 2;

  pkt->buf[0].iov_base = &(pkt->header);
  pkt->buf[0].iov_len = UCP_HEADER_SIZE;

  pkt->buf[1].iov_base = (void *) buf;
  pkt->buf[1].iov_len = buf_len;

  pkt->send = NULL;
  pkt->write = req;

  ucp_cirbuf_set(&(stream->outgoing), (ucp_cirbuf_val_t *) pkt);

  // If we are not the first packet in the queue, wait to send us until the queue is flushed...
  if (stream->pkts_waiting++ > 0) return 0;
  return send_data_packet(stream, pkt);
}
