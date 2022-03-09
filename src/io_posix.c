#include <string.h>

#include "io.h"

ssize_t
udx__sendmsg (udx_t *handle, udx_packet_t *pkt) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = &(pkt->dest);
  h.msg_namelen = sizeof(pkt->dest);

  h.msg_iov = (struct iovec *) &(pkt->bufs);
  h.msg_iovlen = pkt->bufs_len;

  do {
    pkt->time_sent = uv_hrtime() / 1e6;
    size = sendmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size;
}

ssize_t
udx__recvmsg (udx_t *handle, uv_buf_t *buf, struct sockaddr *addr) {
  ssize_t size;
  struct msghdr h;

  memset(&h, 0, sizeof(h));

  h.msg_name = addr;
  h.msg_namelen = sizeof(*addr);

  h.msg_iov = (struct iovec *) buf;
  h.msg_iovlen = 1;

  do {
    size = recvmsg(handle->io_poll.io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  return size;
}