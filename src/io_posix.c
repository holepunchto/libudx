#define _GNU_SOURCE

#if defined(__linux__) || defined(__FreeBSD__)
#define UDX_PLATFORM_HAS_SENDMMSG
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

#include "../include/udx.h"
#include "fifo.h"
#include "internal.h"
#include "io.h"

#if defined(__APPLE__)

int
udx__get_link_mtu (const struct sockaddr *addr) {
  return -1;
}

#else

int
udx__get_link_mtu (const struct sockaddr *addr) {
  assert(addr->sa_family == AF_INET || addr->sa_family == AF_INET6);

  int s = socket(addr->sa_family, SOCK_DGRAM, 0);
  if (s == -1) {
    return -1;
  }

  int rc = connect(s, addr, addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));

  if (rc == -1) {
    return -1;
  }

  int mtu;
  socklen_t mtu_opt_size = sizeof mtu;
  if (addr->sa_family == AF_INET) {
    rc = getsockopt(s, IPPROTO_IP, IP_MTU, &mtu, &mtu_opt_size);
  } else {
    rc = getsockopt(s, IPPROTO_IPV6, IPV6_MTU, &mtu, &mtu_opt_size);
  }
  if (rc == -1) {
    close(s);
    return -1;
  }

  close(s);
  return mtu;
}
#endif

ssize_t
udx__sendmsg (udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, struct sockaddr *addr, int addr_len) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = addr_len;

  h.msg_iov = (struct iovec *) bufs;
  h.msg_iovlen = bufs_len;

  do {
    size = sendmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size == -1 ? uv_translate_sys_error(errno) : size;
}

ssize_t
udx__recvmsg (udx_socket_t *handle, uv_buf_t *buf, struct sockaddr *addr, int addr_len) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = addr_len;

  h.msg_iov = (struct iovec *) buf;
  h.msg_iovlen = 1;

  do {
    size = recvmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size == -1 ? uv_translate_sys_error(errno) : size;
}

#define UDX_SENDMMSG_BATCH_SIZE 20

void
udx__on_writable (udx_socket_t *socket) {
#ifdef UDX_PLATFORM_HAS_SENDMMSG
  udx_fifo_t *fifo = &socket->send_queue;

  while (fifo->len > 0) {
    udx_packet_t *batch[UDX_SENDMMSG_BATCH_SIZE];
    struct mmsghdr h[UDX_SENDMMSG_BATCH_SIZE];

    while (fifo->len > 0 && udx__fifo_peek(fifo) == NULL) {
      udx__fifo_shift(fifo);
    }

    if (fifo->len == 0) {
      return;
    }

    int pkts = 0;

    udx_packet_t *pkt = udx__fifo_peek(fifo);
    int ttl = pkt->ttl;
    bool adjust_ttl = ttl > 0 && socket->ttl != ttl;

    while (pkts < UDX_SENDMMSG_BATCH_SIZE && fifo->len > 0) {
      udx_packet_t *pkt = udx__fifo_shift(fifo);

      if (pkt == NULL) continue;
      // packet is null when descheduled after being acked
      if (pkt->ttl != ttl) {
        udx__fifo_undo(fifo);
        break;
      }

      if (socket->family == 6 && pkt->dest.ss_family == AF_INET) {
        addr_to_v6((struct sockaddr_in *) &(pkt->dest));
        pkt->dest_len = sizeof(struct sockaddr_in6);
      }

      udx__ensure_latest_stream_ack(pkt);

      batch[pkts] = pkt;
      struct mmsghdr *p = &h[pkts];
      memset(p, 0, sizeof(*p));
      p->msg_hdr.msg_name = &pkt->dest;
      p->msg_hdr.msg_namelen = pkt->dest_len;

      p->msg_hdr.msg_iov = (struct iovec *) pkt->bufs;
      p->msg_hdr.msg_iovlen = pkt->bufs_len;

      pkts++;
    }
    uint64_t time_sent = uv_hrtime() / 1e6;

    int rc;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, ttl);

    do {
      rc = sendmmsg(socket->io_poll.io_watcher.fd, h, pkts, 0);
    } while (rc == -1 && errno == EINTR);

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, socket->ttl);

    rc = rc == -1 ? uv_translate_sys_error(errno) : rc;

    int nsent = rc > 0 ? rc : 0;

    if (rc < 0 && rc != UV_EAGAIN) {
      nsent = pkts; // something errored badly, assume all packets sent and lost
    }

    int unsent = pkts - nsent;

    while (unsent > 0) {
      // restore an unsent packet
      udx__fifo_undo(fifo);
      if (udx__fifo_peek(fifo) != NULL) {
        unsent--;
      }
    }

    // update packet status for sent packets

    for (int i = 0; i < nsent; i++) {
      udx_packet_t *pkt = batch[i];

      assert(pkt->status == UDX_PACKET_SENDING);
      pkt->status = UDX_PACKET_INFLIGHT;
      pkt->transmits++;
      pkt->time_sent = time_sent;

      int type = pkt->type;

      if (type & (UDX_PACKET_STREAM_SEND | UDX_PACKET_STREAM_DESTROY | UDX_PACKET_SEND)) {
        udx__trigger_send_callback(pkt);
        // TODO: watch for re-entry here!
      }

      if (type & UDX_PACKET_FREE_ON_SEND) {
        free(pkt);
      }
    }

    if (rc == UV_EAGAIN) {
      break;
    }
  }
#else /* no sendmmsg */
  while (socket->send_queue.len > 0) {
    udx_packet_t *pkt = (udx_packet_t *) udx__fifo_shift(&(socket->send_queue));
    if (pkt == NULL) continue;

    bool adjust_ttl = pkt->ttl > 0 && socket->ttl != pkt->ttl;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, pkt->ttl);

    if (socket->family == 6 && pkt->dest.ss_family == AF_INET) {
      addr_to_v6((struct sockaddr_in *) &(pkt->dest));
      pkt->dest_len = sizeof(struct sockaddr_in6);
    }

    udx__ensure_latest_stream_ack(pkt);

    ssize_t size = udx__sendmsg(socket, pkt->bufs, pkt->bufs_len, (struct sockaddr *) &(pkt->dest), pkt->dest_len);

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
      udx__trigger_send_callback(pkt);
      // TODO: watch for re-entry here!
    }

    if (type & UDX_PACKET_FREE_ON_SEND) {
      free(pkt);
    }
  }
#endif
}
