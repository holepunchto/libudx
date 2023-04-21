#include <string.h>
#include <uv.h>
#include <stdlib.h>     // for calloc
#include <sys/socket.h> // for sendmmsg
#include <assert.h>

#include "io.h"


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

int udx__sendmmsg(udx_socket_t *socket, udx_packet_t *pkts[], unsigned int pkts_len) {
#if defined(__linux__) || defined(__FreeBSD__)

  // todo: consider batches of 20 at a time, avoiding calloc?

  struct mmsghdr *h = calloc(pkts_len, sizeof(struct mmsghdr));

  // todo:
  for (int i = 0; i < pkts_len; i++) {
    udx_packet_t *pkt = pkts[i];
    assert(pkt != NULL && "null packet in sendmmsg" );

    h[i].msg_hdr.msg_name = &pkt->dest;
    h[i].msg_hdr.msg_namelen = pkt->dest_len;

    h[i].msg_hdr.msg_iov = (struct iovec *) pkt->bufs;
    h[i].msg_hdr.msg_iovlen = pkt->bufs_len;
  }
  int npkts;

  do {
    npkts = sendmmsg(socket->io_poll.io_watcher.fd, h, pkts_len, 0);
    // debug_printf("sendmmsg sent %d/%u\n", size, pkts_len);
  } while (npkts == -1 && errno == EINTR);

  free (h);

  return npkts == -1 ? uv_translate_sys_error(errno) : npkts;

#else /* no sendmmsg */
  int npkts = 0;
  for (; npkts < pkts_len; npkts++) {
    udx_packet_t *pkt = pkts[npkts];
    if (pkt == NULL) continue;

    bool adjust_ttl = pkt->ttl > 0 && socket->ttl != pkt->ttl;

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, pkt->ttl);

    int rc = udx__sendmsg(socket, pkt->bufs, pkt->bufs_len, (struct sockaddr *) &(pkt->dest), pkt->dest_len);

    if (adjust_ttl) uv_udp_set_ttl((uv_udp_t *) socket, socket->ttl);

    if (rc < 0) {
      return npkts > 0 ? npkts : rc; /* if it fails on the first call (npkts == 0) return rc to mimic sendmmsg */
    }
  }

  return npkts;
#endif
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
